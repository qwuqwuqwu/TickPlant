#include "questdb_data_source.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

// ─── Constructor ──────────────────────────────────────────────────────────────

QuestDbDataSource::QuestDbDataSource(std::string host, uint16_t rest_port)
    : host_(std::move(host)), port_(rest_port) {}

// ─── URL encoding ─────────────────────────────────────────────────────────────

std::string QuestDbDataSource::url_encode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// ─── HTTP/1.0 GET (raw POSIX sockets) ────────────────────────────────────────

std::string QuestDbDataSource::http_get(const std::string& path) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return {};

    // 2 s connect + recv timeout so a slow QuestDB doesn't block the reporter.
    struct timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port_);
    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
        ::close(fd); return {};
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd); return {};
    }

    // Send HTTP/1.0 GET (Connection: close — server closes after response)
    std::string req =
        "GET " + path + " HTTP/1.0\r\n"
        "Host: " + host_ + "\r\n"
        "Connection: close\r\n"
        "\r\n";
    if (::send(fd, req.data(), req.size(), MSG_NOSIGNAL) < 0) {
        ::close(fd); return {};
    }

    // Read full response
    std::string resp;
    resp.reserve(4096);
    char buf[4096];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);

    // Strip HTTP headers — body starts after "\r\n\r\n"
    auto pos = resp.find("\r\n\r\n");
    if (pos == std::string::npos) return {};
    return resp.substr(pos + 4);
}

// ─── resolve ─────────────────────────────────────────────────────────────────

ResolutionResult QuestDbDataSource::resolve(
        const std::vector<std::string>& /*symbols*/,
        uint64_t /*max_staleness_ms*/)
{
    // Lightweight probe: verify the table exists and has at least one row.
    // No timestamp filter — avoids false STALE when QuestDB just restarted
    // and the tick logger hasn't reconnected yet (last-N-minutes window empty
    // even though the table has 48 k+ historical rows).
    const std::string sql = "SELECT count() FROM order_book LIMIT 1";
    std::string body = http_get("/exec?query=" + url_encode(sql) + "&limit=1");

    if (body.empty()) {
        return {ResolutionStatus::UNREACHABLE, "QuestDB HTTP endpoint unreachable"};
    }
    try {
        auto j = json::parse(body);
        // QuestDB wraps errors as {"error":"<msg>"}
        if (j.contains("error")) {
            return {ResolutionStatus::UNREACHABLE,
                    "QuestDB error: " + j["error"].get<std::string>()};
        }
        // Guard: dataset may be empty if the table has no rows at all.
        if (!j.contains("dataset") ||
            j["dataset"].empty() ||
            j["dataset"][0].empty()) {
            return {ResolutionStatus::STALE, "QuestDB: order_book table is empty"};
        }
        auto cnt = j["dataset"][0][0].get<uint64_t>();
        if (cnt == 0) {
            return {ResolutionStatus::STALE, "QuestDB: order_book table is empty"};
        }
    } catch (...) {
        return {ResolutionStatus::UNREACHABLE, "QuestDB: failed to parse probe response"};
    }
    return {ResolutionStatus::OK, ""};
}

// ─── fetch ────────────────────────────────────────────────────────────────────

SourceData QuestDbDataSource::fetch(const std::vector<std::string>& symbols) {
    SourceData sd;
    sd.source_name = "QuestDB";

    // One query for all symbols — filter in C++ for simplicity.
    const std::string sql =
        "SELECT symbol, avg(spread_bps), min(bid), max(ask), count() "
        "FROM order_book "
        "WHERE timestamp > dateadd('h',-1,now())";

    std::string body = http_get("/exec?query=" + url_encode(sql) + "&limit=10000");
    if (body.empty()) return sd;

    try {
        auto j = json::parse(body);
        if (!j.contains("dataset")) return sd;

        // Build a set of requested symbols for fast lookup.
        std::unordered_map<std::string, bool> want;
        for (const auto& s : symbols) want[s] = true;

        // dataset row: [symbol, avg_spread_bps, min_bid, max_ask, count]
        for (const auto& row : j["dataset"]) {
            if (row.size() < 5) continue;
            std::string sym = row[0].get<std::string>();
            if (!want.count(sym)) continue;

            HistoricalStats hs;
            hs.avg_spread_bps = row[1].is_null() ? 0.0 : row[1].get<double>();
            hs.min_bid        = row[2].is_null() ? 0.0 : row[2].get<double>();
            hs.max_ask        = row[3].is_null() ? 0.0 : row[3].get<double>();
            hs.sample_count   = row[4].is_null() ? 0   : row[4].get<uint64_t>();
            sd.historical[sym] = hs;
        }
    } catch (...) {
        // Malformed JSON — return whatever we parsed so far.
    }
    return sd;
}
