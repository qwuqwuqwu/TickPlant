#pragma once

// remote_data_source.hpp
//
// DataSource that connects to a remote TickPlant query server over TCP.
//
// This enables horizontal scale-out: a report running on Machine A can
// transparently pull live BBO data from a TickPlant instance on Machine B
// (e.g. a co-located box with a different exchange feed mix).
//
// Wire protocol — same as EpollQueryServer / IoUringQueryServer:
//   resolve(): sends "HEALTH\n",  checks per-feed staleness in JSON response.
//   fetch():   sends "SNAPSHOT <sym>\n" for each symbol, parses L2 books.
//
// Each send_command() opens a fresh short-lived TCP connection so that a
// failed remote never blocks the local pipeline beyond the 2 s timeout.

#include "data_source.hpp"
#include <cstdint>
#include <string>

class RemoteDataSource : public DataSource {
public:
    // name_label: registry key, e.g. "Remote-NYC" or "Remote-London"
    explicit RemoteDataSource(std::string name_label,
                              std::string host,
                              uint16_t    port = 9092);

    std::string name() const override { return name_; }

    ResolutionResult resolve(const std::vector<std::string>& symbols,
                             uint64_t max_staleness_ms) override;

    SourceData fetch(const std::vector<std::string>& symbols) override;

private:
    std::string name_;
    std::string host_;
    uint16_t    port_;

    // Open TCP connection, send one line + '\n', read one response line.
    // Returns the response (without trailing '\n'), or empty on failure.
    std::string send_command(const std::string& cmd);
};
