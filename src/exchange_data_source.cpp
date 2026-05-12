#include "exchange_data_source.hpp"

#include <climits>

ExchangeDataSource::ExchangeDataSource(ArbitrageEngine* engine,
                                       std::string      exchange_name)
    : engine_(engine), name_(std::move(exchange_name)) {}

// ─── resolve ─────────────────────────────────────────────────────────────────

ResolutionResult ExchangeDataSource::resolve(
        const std::vector<std::string>& /*symbols*/,
        uint64_t max_staleness_ms)
{
    auto staleness = engine_->get_feed_staleness_ms();

    auto it = staleness.find(name_);
    if (it == staleness.end() || it->second == UINT64_MAX) {
        return {ResolutionStatus::UNREACHABLE,
                name_ + ": no data received yet"};
    }
    if (it->second > max_staleness_ms) {
        return {ResolutionStatus::STALE,
                name_ + ": staleness " + std::to_string(it->second)
                    + "ms > threshold " + std::to_string(max_staleness_ms) + "ms"};
    }
    return {ResolutionStatus::OK, ""};
}

// ─── fetch ────────────────────────────────────────────────────────────────────

SourceData ExchangeDataSource::fetch(const std::vector<std::string>& symbols) {
    SourceData sd;
    sd.source_name = name_;

    for (const auto& sym : symbols) {
        auto snaps = engine_->get_snapshots(sym);   // thread-safe shared_lock
        for (const auto& snap : snaps) {
            if (snap.exchange != name_) continue;   // only our exchange

            BboData bbo;
            bbo.bid          = snap.best_bid();
            bbo.ask          = snap.best_ask();
            bbo.mid          = snap.mid_price();
            bbo.spread_bps   = snap.spread_bps();
            bbo.timestamp_ms = snap.timestamp_ms;
            sd.live[sym][snap.exchange] = bbo;
        }
    }
    return sd;
}
