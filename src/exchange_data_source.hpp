#pragma once

// exchange_data_source.hpp
//
// DataSource adapter for a single exchange inside ArbitrageEngine.
//
// resolve() — checks get_feed_staleness_ms() for this exchange's feed age.
// fetch()   — calls get_snapshots() for each requested canonical symbol,
//             filters to books from this exchange, returns BboData.
//
// One instance per exchange: register "Binance", "Coinbase", "Kraken",
// "Bybit", and "FIX" separately so the report can pick individual feeds.

#include "data_source.hpp"
#include "arbitrage_engine.hpp"

class ExchangeDataSource : public DataSource {
public:
    // exchange_name: "Binance" | "Coinbase" | "Kraken" | "Bybit" | "FIX"
    explicit ExchangeDataSource(ArbitrageEngine* engine,
                                std::string      exchange_name);

    std::string name() const override { return name_; }

    ResolutionResult resolve(const std::vector<std::string>& symbols,
                             uint64_t max_staleness_ms) override;

    SourceData fetch(const std::vector<std::string>& symbols) override;

private:
    ArbitrageEngine* engine_;
    std::string      name_;
};
