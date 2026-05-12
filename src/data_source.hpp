#pragma once

// data_source.hpp
//
// Abstract interface for all Phase 6 data sources.
//
// Three methods mirror the three reporting phases:
//   name()    — unique key used by the registry (pure constant, no locking needed)
//   resolve() — Phase 1: check freshness / reachability (called concurrently)
//   fetch()   — Phase 2: pull data (called only when resolve() returned OK)
//
// Implementations:
//   ExchangeDataSource  — wraps ArbitrageEngine; live BBO for one exchange
//   QuestDbDataSource   — HTTP REST to QuestDB; historical 1-hour aggregates
//   RemoteDataSource    — TCP to a remote TickPlant query server; location-transparent

#include "report_types.hpp"
#include <string>
#include <vector>

class DataSource {
public:
    virtual ~DataSource() = default;

    // Unique name used as the registry key and in JSON output.
    // Constant — no locking required.
    virtual std::string name() const = 0;

    // Check whether this source can serve fresh data for the given symbols.
    //   max_staleness_ms: data older than this → ResolutionStatus::STALE
    // Called concurrently from multiple std::async tasks — must be thread-safe.
    virtual ResolutionResult resolve(const std::vector<std::string>& symbols,
                                     uint64_t max_staleness_ms) = 0;

    // Fetch data for the given canonical symbols.
    // Called only when resolve() returned OK.
    // Must be thread-safe (may be called from multiple threads simultaneously).
    virtual SourceData fetch(const std::vector<std::string>& symbols) = 0;
};
