#include "query_server.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

// ─── Helpers ──────────────────────────────────────────────────────────────────

// Format a double without trailing zeros, 8 significant digits.
static std::string fmt_dbl(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.8g", v);
    return buf;
}

// JSON-escape a plain ASCII string (exchange/symbol names never need full escape).
static std::string jstr(std::string_view s) {
    return "\"" + std::string(s) + "\"";
}

// Render one [price, qty] level pair.
static std::string level_pair(const PriceLevel& lvl) {
    return "[" + fmt_dbl(lvl.price) + "," + fmt_dbl(lvl.quantity) + "]";
}

// ─── Request parsing ──────────────────────────────────────────────────────────

ParsedRequest parse_request(std::string_view line) {
    // strip trailing whitespace / CR
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        line.remove_suffix(1);

    ParsedRequest req;

    if (line == "HEALTH" || line == "health") {
        req.type = RequestType::HEALTH;
        return req;
    }

    // "SNAPSHOT <symbol>" (case-insensitive prefix)
    if (line.size() > 9) {
        auto prefix = line.substr(0, 9);
        // compare case-insensitively
        std::string p(prefix);
        std::transform(p.begin(), p.end(), p.begin(), ::toupper);
        if (p == "SNAPSHOT ") {
            req.type   = RequestType::SNAPSHOT;
            req.symbol = std::string(line.substr(9));
            // upper-case the symbol
            std::transform(req.symbol.begin(), req.symbol.end(),
                           req.symbol.begin(), ::toupper);
            return req;
        }
    }

    req.type = RequestType::UNKNOWN;
    return req;
}

// ─── Response builders ────────────────────────────────────────────────────────

std::string snapshot_response(const std::string& canonical_sym,
                               const std::vector<OrderBookSnapshot>& books,
                               int max_levels) {
    if (books.empty()) {
        return error_response("symbol not found: " + canonical_sym);
    }

    std::string s;
    s.reserve(2048);

    s += "{\"status\":\"ok\",\"symbol\":";
    s += jstr(canonical_sym);
    s += ",\"books\":[";

    for (size_t i = 0; i < books.size(); ++i) {
        const auto& b = books[i];
        if (i > 0) s += ',';
        s += "{\"exchange\":";
        s += jstr(b.exchange);
        s += ",\"symbol\":";
        s += jstr(b.symbol);
        s += ",\"bids\":[";

        int n = std::min(max_levels, static_cast<int>(b.bids.size()));
        for (int j = 0; j < n; ++j) {
            if (j > 0) s += ',';
            s += level_pair(b.bids[j]);
        }
        s += "],\"asks\":[";
        n = std::min(max_levels, static_cast<int>(b.asks.size()));
        for (int j = 0; j < n; ++j) {
            if (j > 0) s += ',';
            s += level_pair(b.asks[j]);
        }
        s += "],\"timestamp_ms\":";
        s += std::to_string(b.timestamp_ms);
        s += '}';
    }

    s += "]}\n";
    return s;
}

std::string health_response(
        const std::unordered_map<std::string, uint64_t>& staleness_ms,
        uint64_t live_threshold_ms) {
    // stable ordering for readability
    const char* order[] = {"Binance", "Coinbase", "Kraken", "Bybit", "FIX"};

    std::string s;
    s.reserve(256);
    s += "{\"status\":\"ok\",\"feeds\":[";

    bool first = true;
    for (const char* ex : order) {
        auto it = staleness_ms.find(ex);
        if (it == staleness_ms.end()) continue;

        if (!first) s += ',';
        first = false;

        uint64_t ms   = it->second;
        bool     live = (ms != UINT64_MAX) && (ms < live_threshold_ms);

        s += "{\"exchange\":";
        s += jstr(ex);
        s += ",\"staleness_ms\":";
        s += (ms == UINT64_MAX) ? "-1" : std::to_string(ms);
        s += ",\"live\":";
        s += live ? "true" : "false";
        s += '}';
    }

    s += "]}\n";
    return s;
}

std::string error_response(std::string_view message) {
    std::string s;
    s += "{\"status\":\"error\",\"message\":\"";
    // simple escape: replace " with '
    for (char c : message) s += (c == '"') ? '\'' : c;
    s += "\"}\n";
    return s;
}
