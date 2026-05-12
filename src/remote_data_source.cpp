#include "remote_data_source.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

// ─── Constructor ──────────────────────────────────────────────────────────────

RemoteDataSource::RemoteDataSource(std::string name_label,
                                   std::string host,
                                   uint16_t    port)
    : name_(std::move(name_label)), host_(std::move(host)), port_(port) {}

// ─── TCP helper ───────────────────────────────────────────────────────────────

std::string RemoteDataSource::send_command(const std::string& cmd) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return {};

    // 2 s connect + recv timeout
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

    // Send command terminated by '\n'
    std::string req = cmd + "\n";
    if (::send(fd, req.data(), req.size(), MSG_NOSIGNAL) < 0) {
        ::close(fd); return {};
    }

    // Read one response line (ends with '\n')
    std::string resp;
    resp.reserve(4096);
    char buf[4096];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, static_cast<size_t>(n));
        if (resp.back() == '\n') break;
    }
    ::close(fd);

    // Strip trailing '\n'
    if (!resp.empty() && resp.back() == '\n') resp.pop_back();
    return resp;
}

// ─── resolve ─────────────────────────────────────────────────────────────────

ResolutionResult RemoteDataSource::resolve(
        const std::vector<std::string>& /*symbols*/,
        uint64_t max_staleness_ms)
{
    std::string resp = send_command("HEALTH");
    if (resp.empty()) {
        return {ResolutionStatus::UNREACHABLE,
                name_ + ": no response from " + host_ + ":" + std::to_string(port_)};
    }
    try {
        auto j = json::parse(resp);
        if (j.value("status", "") != "ok") {
            return {ResolutionStatus::UNREACHABLE, name_ + ": HEALTH not ok"};
        }
        // Check that at least one feed is live within our threshold.
        bool any_live = false;
        for (const auto& feed : j["feeds"]) {
            int64_t ms = feed.value("staleness_ms", int64_t(-1));
            if (ms >= 0 && static_cast<uint64_t>(ms) <= max_staleness_ms) {
                any_live = true;
                break;
            }
        }
        if (!any_live) {
            return {ResolutionStatus::STALE,
                    name_ + ": all remote feeds stale beyond threshold"};
        }
    } catch (...) {
        return {ResolutionStatus::UNREACHABLE, name_ + ": HEALTH parse error"};
    }
    return {ResolutionStatus::OK, ""};
}

// ─── fetch ────────────────────────────────────────────────────────────────────

SourceData RemoteDataSource::fetch(const std::vector<std::string>& symbols) {
    SourceData sd;
    sd.source_name = name_;

    for (const auto& sym : symbols) {
        std::string resp = send_command("SNAPSHOT " + sym);
        if (resp.empty()) continue;
        try {
            auto j = json::parse(resp);
            if (j.value("status", "") != "ok") continue;

            // Parse books array: each entry has exchange, bids[[p,q],...], asks[...]
            for (const auto& book : j["books"]) {
                std::string exchange = book.value("exchange", "");
                if (exchange.empty()) continue;

                BboData bbo;
                bbo.timestamp_ms = book.value("timestamp_ms", uint64_t(0));

                // bids[0] = best bid, asks[0] = best ask
                if (!book["bids"].empty()) {
                    bbo.bid = book["bids"][0][0].get<double>();
                }
                if (!book["asks"].empty()) {
                    bbo.ask = book["asks"][0][0].get<double>();
                }
                if (bbo.bid > 0 && bbo.ask > 0) {
                    bbo.mid        = (bbo.bid + bbo.ask) / 2.0;
                    bbo.spread_bps = ((bbo.ask - bbo.bid) / bbo.bid) * 10000.0;
                }
                sd.live[sym][exchange] = bbo;
            }
        } catch (...) {
            // Malformed response — skip this symbol.
        }
    }
    return sd;
}
