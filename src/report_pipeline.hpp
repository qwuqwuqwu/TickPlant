#pragma once

// report_pipeline.hpp
//
// Three-phase reporting pipeline — Bloomberg EPP-inspired architecture.
//
// ┌─────────────────────────────────────────────────────────────────────┐
// │  Phase 1 — Resolution                                               │
// │    For each source in the report config, fire std::async to call    │
// │    DataSource::resolve() concurrently.  Collect outcomes:           │
// │      OK          → proceed to pipeline phase                        │
// │      STALE       → skip (or abort if source.required)              │
// │      UNREACHABLE → skip (or abort if source.required)              │
// ├─────────────────────────────────────────────────────────────────────┤
// │  Phase 2 — Pipeline                                                 │
// │    For each source that resolved OK, fire std::async to call        │
// │    DataSource::fetch() concurrently.  Collect SourceData objects.  │
// ├─────────────────────────────────────────────────────────────────────┤
// │  Phase 3 — Generation                                               │
// │    Merge all SourceData objects and render a compact JSON response. │
// └─────────────────────────────────────────────────────────────────────┘
//
// run()         — execute the named report; returns JSON + '\n'
// list_reports() — compact JSON array of configured report names + '\n'

#include "data_source_registry.hpp"
#include "report_types.hpp"

#include <string>
#include <unordered_map>
#include <vector>

class ReportPipeline {
public:
    explicit ReportPipeline(DataSourceRegistry* registry,
                            const std::string&  config_path = "reports.json");

    // Execute a named report.  Thread-safe — each call is fully independent.
    // Returns compact JSON + '\n'.
    std::string run(const std::string& report_name);

    // List all configured report names.  Returns compact JSON + '\n'.
    std::string list_reports() const;

private:
    DataSourceRegistry* registry_;
    std::unordered_map<std::string, ReportConfig> configs_;

    // Phase 1: concurrent resolution.
    std::unordered_map<std::string, ResolutionResult>
    resolve_phase(const ReportConfig& cfg);

    // Phase 2: concurrent fetch from OK sources.
    std::vector<SourceData>
    pipeline_phase(const ReportConfig& cfg,
                   const std::unordered_map<std::string, ResolutionResult>& res);

    // Phase 3: render final JSON.
    std::string generate_phase(
        const ReportConfig& cfg,
        const std::unordered_map<std::string, ResolutionResult>& resolution,
        const std::vector<SourceData>& data);

    // Parse reports.json and populate configs_.
    void load_config(const std::string& path);
};
