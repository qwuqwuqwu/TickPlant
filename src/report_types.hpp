#pragma once

// report_types.hpp
//
// Shared POD types for the Phase 6 reporting pipeline.
//
// Data flow:
//   ReportConfig  — loaded once from reports.json
//   ResolutionResult — per-source outcome from the resolution phase
//   SourceData    — per-source payload from the pipeline phase
//   BboData / HistoricalStats — leaf values inside SourceData

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// ─── Leaf types ───────────────────────────────────────────────────────────────

// Best-bid/offer snapshot for one (symbol, exchange) pair.
struct BboData {
    double   bid          = 0.0;
    double   ask          = 0.0;
    double   mid          = 0.0;
    double   spread_bps   = 0.0;
    uint64_t timestamp_ms = 0;
};

// 1-hour aggregate from QuestDB for one canonical symbol.
struct HistoricalStats {
    double   avg_spread_bps = 0.0;
    double   min_bid        = 0.0;
    double   max_ask        = 0.0;
    uint64_t sample_count   = 0;
};

// ─── Resolution ───────────────────────────────────────────────────────────────

enum class ResolutionStatus { OK, STALE, UNREACHABLE };

struct ResolutionResult {
    ResolutionStatus status  = ResolutionStatus::UNREACHABLE;
    std::string      message;   // human-readable detail (empty on OK)
};

// ─── Per-source payload ───────────────────────────────────────────────────────

// One source's contribution to a report.
//   live[canonical_symbol][exchange_name] = BboData
//   historical[canonical_symbol]          = HistoricalStats  (QuestDB only)
struct SourceData {
    std::string source_name;
    std::unordered_map<std::string,
        std::unordered_map<std::string, BboData>> live;
    std::unordered_map<std::string, HistoricalStats> historical;
};

// ─── Config (reports.json) ────────────────────────────────────────────────────

struct SourceConfig {
    std::string name;                    // must match a registered DataSource name
    bool        required       = false;  // if true, UNREACHABLE aborts the report
    uint64_t    max_staleness_ms = 5000;
};

struct ReportConfig {
    std::string               name;
    std::vector<std::string>  symbols;   // canonical symbols, e.g. ["BTC","ETH"]
    std::vector<SourceConfig> sources;
};
