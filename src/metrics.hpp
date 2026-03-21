#pragma once

// metrics.hpp
//
// Singleton Prometheus metrics registry for TickPlant.
//
// All record_*() / set_*() methods are thread-safe: prometheus-cpp uses
// internal atomics for counters, gauges, and histograms.  The pre-cached
// per-exchange pointers are written once at construction and read-only
// thereafter, so no additional locking is needed on the hot path.
//
// Metrics exposed on http://localhost:9090/metrics (Prometheus pull model):
//
//   tickplant_e2e_latency_ns        Histogram  {exchange}
//   tickplant_messages_total        Counter    {exchange, type="snapshot|incremental"}
//   tickplant_parse_latency_ns      Histogram  {parser="fix|json"}
//   tickplant_orderbook_update_ns   Histogram  {exchange}
//   tickplant_book_depth            Gauge      {exchange, symbol, side="bid|ask"}
//   tickplant_book_staleness_ms     Gauge      {exchange, symbol}
//   tickplant_arb_detections_total  Counter    {buy_exchange, sell_exchange, symbol}

#include <prometheus/registry.h>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/exposer.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <functional>
#include <chrono>

// ─── Metrics singleton ────────────────────────────────────────────────────────

class Metrics {
public:
    static Metrics& instance();

    // Start the HTTP exposer.  Must be called once before the engine starts.
    void start(uint16_t port = 9090);

    // ── Hot-path recording ────────────────────────────────────────────────────

    // End-to-end latency: depth_callback_ start → update_order_book() complete.
    void record_e2e_latency(const std::string& exchange, double ns);

    // Increment message counter.  is_snapshot: true = snapshot, false = delta.
    void record_message(const std::string& exchange, bool is_snapshot);

    // Parse latency.  parser = "fix" or "json".
    //   "json" — depth_callback_ execution time (proxy for JSON parse overhead)
    //   "fix"  — apply_snapshot / apply_update execution in update_fix_data()
    void record_parse_latency(const std::string& parser, double ns);

    // Order book update latency: just the set_level()/delete_level() loop.
    void record_orderbook_update(const std::string& exchange, double ns);

    // ── Periodic gauges (updated inside calculate_arbitrage every cycle) ──────

    // Current bid or ask level count for a book.  side = "bid" | "ask".
    void set_book_depth(const std::string& exchange, const std::string& symbol,
                        const std::string& side, double depth);

    // Milliseconds since last update for a book.
    void set_book_staleness(const std::string& exchange,
                            const std::string& symbol, double ms);

    // ── Arbitrage detection ───────────────────────────────────────────────────
    void record_arb_detection(const std::string& buy_exchange,
                               const std::string& sell_exchange,
                               const std::string& symbol);

private:
    Metrics();
    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;

    // Pre-cache per-exchange metric instances for the 5 known exchanges to
    // avoid label-map construction on the hot path.
    void init_exchange(const std::string& exchange);

    // Histogram bucket boundaries (nanoseconds) from ROADMAP §3.1:
    //   100 ns … 100 µs — sub-µs JSON parse through worst-case L2 update.
    static const prometheus::Histogram::BucketBoundaries LATENCY_BUCKETS_NS;

    std::shared_ptr<prometheus::Registry> registry_;
    std::unique_ptr<prometheus::Exposer>  exposer_;

    // ── Metric families ───────────────────────────────────────────────────────
    prometheus::Family<prometheus::Histogram>* e2e_family_       = nullptr;
    prometheus::Family<prometheus::Counter>*   messages_family_  = nullptr;
    prometheus::Family<prometheus::Histogram>* parse_family_     = nullptr;
    prometheus::Family<prometheus::Histogram>* ob_update_family_ = nullptr;
    prometheus::Family<prometheus::Gauge>*     book_depth_family_= nullptr;
    prometheus::Family<prometheus::Gauge>*     book_stale_family_= nullptr;
    prometheus::Family<prometheus::Counter>*   arb_family_       = nullptr;

    // ── Pre-cached instances for known exchanges ──────────────────────────────
    std::unordered_map<std::string, prometheus::Histogram*> e2e_cache_;
    std::unordered_map<std::string, prometheus::Counter*>   snap_msg_cache_;
    std::unordered_map<std::string, prometheus::Counter*>   incr_msg_cache_;
    std::unordered_map<std::string, prometheus::Histogram*> ob_cache_;

    // parse_latency has only two fixed labels — store directly
    prometheus::Histogram* parse_fix_  = nullptr;
    prometheus::Histogram* parse_json_ = nullptr;
};

// ─── ScopedNsTimer ────────────────────────────────────────────────────────────
//
// RAII nanosecond timer.  Records elapsed time to a callback on destruction.
//
// Usage:
//   {
//       ScopedNsTimer t([](double ns){
//           Metrics::instance().record_e2e_latency("Binance", ns);
//       });
//       g_arbitrage_engine->update_order_book(snap);
//   }  // ← fires callback here

class ScopedNsTimer {
public:
    using Callback = std::function<void(double ns)>;

    explicit ScopedNsTimer(Callback cb)
        : cb_(std::move(cb))
        , t0_(std::chrono::steady_clock::now()) {}

    ~ScopedNsTimer() {
        auto t1 = std::chrono::steady_clock::now();
        double ns = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0_).count());
        cb_(ns);
    }

    // Non-copyable, non-movable
    ScopedNsTimer(const ScopedNsTimer&) = delete;
    ScopedNsTimer& operator=(const ScopedNsTimer&) = delete;

private:
    Callback cb_;
    std::chrono::steady_clock::time_point t0_;
};
