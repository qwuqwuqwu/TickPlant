#include "metrics.hpp"
#include <iostream>

// ─── Bucket boundaries ────────────────────────────────────────────────────────
// Sub-µs parse path:   100 ns → 1 µs   (FIX apply_snapshot, fast JSON paths)
// Typical e2e range:   1 µs  → 1 ms    (mutex + map lookup + book-write loop)
// Tail / contention:   1 ms  → 100 ms   (lock contention, OS scheduling jitter)
const prometheus::Histogram::BucketBoundaries Metrics::LATENCY_BUCKETS_NS = {
    100, 500,
    1'000, 2'500, 5'000,
    10'000, 25'000, 50'000, 100'000, 250'000, 500'000,
    1'000'000, 2'500'000, 5'000'000, 100'000'000
};

// ─── Singleton ────────────────────────────────────────────────────────────────

Metrics& Metrics::instance() {
    static Metrics m;
    return m;
}

// ─── Constructor ─────────────────────────────────────────────────────────────

Metrics::Metrics()
    : registry_(std::make_shared<prometheus::Registry>()) {

    using namespace prometheus;

    e2e_family_ = &BuildHistogram()
        .Name("tickplant_e2e_latency_ns")
        .Help("End-to-end latency: WebSocket frame received to order book "
              "update complete, in nanoseconds")
        .Register(*registry_);

    messages_family_ = &BuildCounter()
        .Name("tickplant_messages_total")
        .Help("Total L2 depth messages processed, labelled by exchange and "
              "message type (snapshot or incremental)")
        .Register(*registry_);

    parse_family_ = &BuildHistogram()
        .Name("tickplant_parse_latency_ns")
        .Help("Parse latency in nanoseconds: parser=fix measures "
              "apply_snapshot/apply_update; parser=json measures the full "
              "depth_callback execution time")
        .Register(*registry_);

    ob_update_family_ = &BuildHistogram()
        .Name("tickplant_orderbook_update_ns")
        .Help("Order book set_level()/delete_level() loop latency per update, "
              "in nanoseconds")
        .Register(*registry_);

    book_depth_family_ = &BuildGauge()
        .Name("tickplant_book_depth")
        .Help("Current number of price levels on each side of the order book")
        .Register(*registry_);

    book_stale_family_ = &BuildGauge()
        .Name("tickplant_book_staleness_ms")
        .Help("Milliseconds since the last update was applied to this order "
              "book (engine skips books older than 500 ms)")
        .Register(*registry_);

    arb_family_ = &BuildCounter()
        .Name("tickplant_arb_detections_total")
        .Help("Arbitrage opportunities detected across exchange pairs, "
              "labelled by buy_exchange, sell_exchange, and normalized symbol")
        .Register(*registry_);

    // Pre-cache metric instances for the 5 known exchanges so that
    // record_*() methods never allocate or build label maps on the hot path.
    for (const char* ex : {"Binance", "Bybit", "Coinbase", "Kraken", "FIX"}) {
        init_exchange(ex);
    }

    // parse_latency has only two fixed parser labels
    parse_fix_  = &parse_family_->Add({{"parser", "fix"}},  LATENCY_BUCKETS_NS);
    parse_json_ = &parse_family_->Add({{"parser", "json"}}, LATENCY_BUCKETS_NS);
}

void Metrics::init_exchange(const std::string& exchange) {
    e2e_cache_[exchange] =
        &e2e_family_->Add({{"exchange", exchange}}, LATENCY_BUCKETS_NS);

    snap_msg_cache_[exchange] =
        &messages_family_->Add({{"exchange", exchange}, {"type", "snapshot"}});

    incr_msg_cache_[exchange] =
        &messages_family_->Add({{"exchange", exchange}, {"type", "incremental"}});

    ob_cache_[exchange] =
        &ob_update_family_->Add({{"exchange", exchange}}, LATENCY_BUCKETS_NS);
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

void Metrics::start(uint16_t port) {
    const std::string addr = "0.0.0.0:" + std::to_string(port);
    exposer_ = std::make_unique<prometheus::Exposer>(addr);
    exposer_->RegisterCollectable(registry_);
    std::cout << "Prometheus metrics exposed on http://localhost:"
              << port << "/metrics\n";
}

// ─── Hot-path recording ───────────────────────────────────────────────────────

void Metrics::record_e2e_latency(const std::string& exchange, double ns) {
    auto it = e2e_cache_.find(exchange);
    if (it != e2e_cache_.end()) it->second->Observe(ns);
}

void Metrics::record_message(const std::string& exchange, bool is_snapshot) {
    if (is_snapshot) {
        auto it = snap_msg_cache_.find(exchange);
        if (it != snap_msg_cache_.end()) it->second->Increment();
    } else {
        auto it = incr_msg_cache_.find(exchange);
        if (it != incr_msg_cache_.end()) it->second->Increment();
    }
}

void Metrics::record_parse_latency(const std::string& parser, double ns) {
    if (parser == "fix")  { if (parse_fix_)  parse_fix_->Observe(ns);  return; }
    if (parser == "json") { if (parse_json_) parse_json_->Observe(ns); return; }
}

void Metrics::record_orderbook_update(const std::string& exchange, double ns) {
    auto it = ob_cache_.find(exchange);
    if (it != ob_cache_.end()) it->second->Observe(ns);
}

// ─── Periodic gauges ─────────────────────────────────────────────────────────

void Metrics::set_book_depth(const std::string& exchange,
                              const std::string& symbol,
                              const std::string& side, double depth) {
    book_depth_family_->Add({{"exchange", exchange},
                              {"symbol",   symbol},
                              {"side",     side}}).Set(depth);
}

void Metrics::set_book_staleness(const std::string& exchange,
                                  const std::string& symbol, double ms) {
    book_stale_family_->Add({{"exchange", exchange},
                              {"symbol",   symbol}}).Set(ms);
}

// ─── Arbitrage detection ─────────────────────────────────────────────────────

void Metrics::record_arb_detection(const std::string& buy_exchange,
                                    const std::string& sell_exchange,
                                    const std::string& symbol) {
    arb_family_->Add({{"buy_exchange",  buy_exchange},
                      {"sell_exchange", sell_exchange},
                      {"symbol",        symbol}}).Increment();
}
