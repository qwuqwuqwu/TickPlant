#include "report_pipeline.hpp"

#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>

#include <nlohmann/json.hpp>
using json = nlohmann::json;

// ─── Constructor ──────────────────────────────────────────────────────────────

ReportPipeline::ReportPipeline(DataSourceRegistry* registry,
                               const std::string&  config_path)
    : registry_(registry)
{
    load_config(config_path);
}

// ─── Config loading ───────────────────────────────────────────────────────────

void ReportPipeline::load_config(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "[pipeline] WARNING: cannot open '" << path
                  << "' — no reports configured\n";
        return;
    }
    try {
        auto j = json::parse(f);
        for (const auto& rj : j["reports"]) {
            ReportConfig cfg;
            cfg.name = rj.value("name", "");
            if (cfg.name.empty()) continue;

            for (const auto& sym : rj["symbols"])
                cfg.symbols.push_back(sym.get<std::string>());

            for (const auto& sj : rj["sources"]) {
                SourceConfig sc;
                sc.name              = sj.value("name", "");
                sc.required          = sj.value("required", false);
                sc.max_staleness_ms  = sj.value("max_staleness_ms", uint64_t(5000));
                if (!sc.name.empty()) cfg.sources.push_back(std::move(sc));
            }
            configs_[cfg.name] = std::move(cfg);
        }
        std::cout << "[pipeline] loaded " << configs_.size()
                  << " report(s) from '" << path << "'\n";
    } catch (const std::exception& e) {
        std::cerr << "[pipeline] ERROR parsing '" << path << "': " << e.what() << '\n';
    }
}

// ─── list_reports ─────────────────────────────────────────────────────────────

std::string ReportPipeline::list_reports() const {
    std::string s;
    s += "{\"status\":\"ok\",\"reports\":[";
    bool first = true;
    for (const auto& [n, _] : configs_) {
        if (!first) s += ',';
        first = false;
        s += '"';
        s += n;
        s += '"';
    }
    s += "]}\n";
    return s;
}

// ─── run ─────────────────────────────────────────────────────────────────────

std::string ReportPipeline::run(const std::string& report_name) {
    auto it = configs_.find(report_name);
    if (it == configs_.end()) {
        return "{\"status\":\"error\",\"message\":\"unknown report: "
               + report_name + "\"}\n";
    }
    const ReportConfig& cfg = it->second;

    // Phase 1 — concurrent resolution
    auto resolution = resolve_phase(cfg);

    // Check if any required source failed to resolve
    for (const auto& sc : cfg.sources) {
        if (!sc.required) continue;
        auto rit = resolution.find(sc.name);
        if (rit == resolution.end() ||
            rit->second.status != ResolutionStatus::OK) {
            std::string msg = "required source '" + sc.name + "' unavailable";
            if (rit != resolution.end() && !rit->second.message.empty())
                msg += ": " + rit->second.message;
            return "{\"status\":\"error\",\"message\":\"" + msg + "\"}\n";
        }
    }

    // Phase 2 — concurrent fetch
    auto data = pipeline_phase(cfg, resolution);

    // Phase 3 — generate JSON
    return generate_phase(cfg, resolution, data);
}

// ─── Phase 1: resolution ─────────────────────────────────────────────────────

std::unordered_map<std::string, ResolutionResult>
ReportPipeline::resolve_phase(const ReportConfig& cfg) {
    // Launch one async task per source.
    std::vector<std::pair<std::string, std::future<ResolutionResult>>> futures;
    futures.reserve(cfg.sources.size());

    for (const auto& sc : cfg.sources) {
        DataSource* src = registry_->get(sc.name);
        if (!src) {
            // Not registered — treat as UNREACHABLE immediately.
            std::promise<ResolutionResult> p;
            p.set_value({ResolutionStatus::UNREACHABLE,
                         sc.name + ": not registered"});
            futures.emplace_back(sc.name, p.get_future());
            continue;
        }
        // Capture by pointer — safe because registry owns src and outlives this call.
        uint64_t ms = sc.max_staleness_ms;
        auto syms   = cfg.symbols;   // copy for capture
        futures.emplace_back(sc.name,
            std::async(std::launch::async,
                [src, syms = std::move(syms), ms]() mutable {
                    return src->resolve(syms, ms);
                }));
    }

    // Collect results.
    std::unordered_map<std::string, ResolutionResult> results;
    results.reserve(futures.size());
    for (auto& [n, f] : futures) {
        results[n] = f.get();
    }
    return results;
}

// ─── Phase 2: pipeline (fetch) ────────────────────────────────────────────────

std::vector<SourceData>
ReportPipeline::pipeline_phase(
        const ReportConfig& cfg,
        const std::unordered_map<std::string, ResolutionResult>& res)
{
    std::vector<std::pair<std::string, std::future<SourceData>>> futures;

    for (const auto& sc : cfg.sources) {
        auto rit = res.find(sc.name);
        if (rit == res.end() || rit->second.status != ResolutionStatus::OK)
            continue;   // skip non-OK sources

        DataSource* src = registry_->get(sc.name);
        if (!src) continue;

        auto syms = cfg.symbols;
        futures.emplace_back(sc.name,
            std::async(std::launch::async,
                [src, syms = std::move(syms)]() mutable {
                    return src->fetch(syms);
                }));
    }

    std::vector<SourceData> out;
    out.reserve(futures.size());
    for (auto& [n, f] : futures) {
        out.push_back(f.get());
    }
    return out;
}

// ─── Phase 3: generation (JSON rendering) ─────────────────────────────────────

static std::string status_str(ResolutionStatus s) {
    switch (s) {
        case ResolutionStatus::OK:          return "ok";
        case ResolutionStatus::STALE:       return "stale";
        case ResolutionStatus::UNREACHABLE: return "unreachable";
    }
    return "unknown";
}

static std::string fmt_dbl(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.8g", v);
    return buf;
}

std::string ReportPipeline::generate_phase(
        const ReportConfig& cfg,
        const std::unordered_map<std::string, ResolutionResult>& resolution,
        const std::vector<SourceData>& data)
{
    using namespace std::chrono;
    uint64_t now_ms = static_cast<uint64_t>(
        duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count());

    std::string s;
    s.reserve(4096);

    s += "{\"status\":\"ok\"";
    s += ",\"report\":\"" + cfg.name + "\"";
    s += ",\"generated_at_ms\":" + std::to_string(now_ms);

    // ── Resolution summary ────────────────────────────────────────────────────
    s += ",\"resolution\":{";
    bool first = true;
    for (const auto& sc : cfg.sources) {
        if (!first) s += ',';
        first = false;
        s += "\"" + sc.name + "\":{\"status\":\"";
        auto rit = resolution.find(sc.name);
        if (rit != resolution.end()) {
            s += status_str(rit->second.status) + "\"";
            if (!rit->second.message.empty()) {
                s += ",\"message\":\"";
                for (char c : rit->second.message)
                    s += (c == '"') ? '\'' : c;
                s += "\"";
            }
        } else {
            s += "unreachable\"";
        }
        s += "}";
    }
    s += "}";

    // ── Symbols section ───────────────────────────────────────────────────────
    s += ",\"symbols\":{";
    bool sym_first = true;
    for (const auto& sym : cfg.symbols) {
        if (!sym_first) s += ',';
        sym_first = false;
        s += "\"" + sym + "\":{";

        // Collect live BBO across all sources for this symbol
        s += "\"live\":{";
        bool ex_first = true;
        for (const auto& sd : data) {
            auto sit = sd.live.find(sym);
            if (sit == sd.live.end()) continue;
            for (const auto& [ex, bbo] : sit->second) {
                if (!ex_first) s += ',';
                ex_first = false;
                s += "\"" + ex + "\":{";
                s += "\"bid\":"        + fmt_dbl(bbo.bid);
                s += ",\"ask\":"       + fmt_dbl(bbo.ask);
                s += ",\"mid\":"       + fmt_dbl(bbo.mid);
                s += ",\"spread_bps\":" + fmt_dbl(bbo.spread_bps);
                s += ",\"timestamp_ms\":" + std::to_string(bbo.timestamp_ms);
                s += "}";
            }
        }
        s += "}";

        // Collect historical stats (from QuestDB source)
        bool has_hist = false;
        for (const auto& sd : data) {
            auto hit = sd.historical.find(sym);
            if (hit == sd.historical.end()) continue;
            if (!has_hist) {
                s += ",\"historical\":{";
                has_hist = true;
            }
            const auto& hs = hit->second;
            s += "\"avg_spread_bps\":" + fmt_dbl(hs.avg_spread_bps);
            s += ",\"min_bid\":"       + fmt_dbl(hs.min_bid);
            s += ",\"max_ask\":"       + fmt_dbl(hs.max_ask);
            s += ",\"sample_count\":"  + std::to_string(hs.sample_count);
            break;   // only one QuestDB source expected
        }
        if (has_hist) s += "}";

        s += "}";
    }
    s += "}";

    s += "}\n";
    return s;
}
