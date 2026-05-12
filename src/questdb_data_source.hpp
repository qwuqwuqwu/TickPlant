#pragma once

// questdb_data_source.hpp
//
// DataSource that fetches historical aggregates from QuestDB via HTTP REST.
//
// resolve() — opens a TCP socket to port 9000 and issues a lightweight
//             COUNT query to confirm QuestDB is alive and has recent rows.
//
// fetch()   — issues one SQL query per call:
//               SELECT exchange, symbol,
//                      avg(spread_bps), min(bid), max(ask), count()
//               FROM   order_book
//               WHERE  timestamp > dateadd('h', -1, now())
//             Parses the JSON dataset and returns HistoricalStats keyed by
//             canonical symbol (aggregated across all exchanges).
//
// Uses raw POSIX sockets and HTTP/1.0 — no libcurl dependency.
// nlohmann_json (already linked) is used to parse QuestDB's JSON response.

#include "data_source.hpp"
#include <cstdint>
#include <string>

class QuestDbDataSource : public DataSource {
public:
    explicit QuestDbDataSource(std::string host     = "127.0.0.1",
                               uint16_t    rest_port = 9000);

    std::string name() const override { return "QuestDB"; }

    ResolutionResult resolve(const std::vector<std::string>& symbols,
                             uint64_t max_staleness_ms) override;

    SourceData fetch(const std::vector<std::string>& symbols) override;

private:
    std::string host_;
    uint16_t    port_;

    // Open a fresh TCP connection, send one HTTP/1.0 GET, return the body.
    // Returns empty string on any failure.
    std::string http_get(const std::string& path);

    // Percent-encode a string for use in a URL query parameter.
    static std::string url_encode(const std::string& s);
};
