#pragma once

// fix_feed_simulator.hpp
//
// Synthetic FIX 4.4 market data feed — Phase 2.3 of the TickPlant roadmap.
//
// Generates realistic 35=W (MarketDataSnapshot) and 35=X (MarketDataIncremental)
// messages at a configurable rate, using the same crypto symbols as the four
// WebSocket producers.  Serves two purposes:
//
//   1. End-to-end pipeline validation without a live FIX connection.
//   2. Controlled arbitrage signal injection: set price_offset_bps on a symbol
//      to force detectable opportunities between the FIX book and the live
//      WebSocket books.
//
// Lifetime note: the FIXMessage passed to the MessageCallback contains
// string_view fields pointing into a stack buffer that is only valid for
// the duration of the callback.  Callers must not store those string_views.
// The TickerData returned by msg.to_ticker_data() is safe to store — it owns
// its strings.
//
// Usage:
//   FIXFeedSimulator sim;
//   sim.add_symbol({"BTCUSD", 65000.0, 0.001});
//   sim.set_callback([&](const std::string& raw, const FIXMessage& msg) {
//       engine.update_fix_data(msg);
//   });
//   sim.start();

#include "fix_parser.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

// ─── SimulatedSymbol ──────────────────────────────────────────────────────────

struct SimulatedSymbol {
    std::string symbol;               // FIX symbol, e.g. "BTCUSD"
    double      mid_price;            // Starting mid price, e.g. 65000.0
    double      spread_pct  = 0.001;  // Half-spread as a fraction (0.001 = 0.1%)
    double      depth_step  = 0.0;    // Price gap between consecutive L2 levels.
                                      // 0 = auto: 0.05% of mid_price.
    int         depth_levels = 5;     // Number of bid and ask levels per side.

    // Optional: shift all generated prices by N basis points.
    // Use this to create a synthetic arb signal against the live feeds:
    //   positive value → FIX prices are higher than market (test sell-FIX arb)
    //   negative value → FIX prices are lower (test buy-FIX arb)
    double      price_offset_bps = 0.0;
};

// ─── FIXFeedSimulator ─────────────────────────────────────────────────────────

class FIXFeedSimulator {
public:
    // Callback invoked synchronously from the simulator thread per message.
    //   raw — wire-format bytes (SOH-delimited, parseable by FIXParser)
    //   msg — already-parsed FIXMessage; valid only for the callback's duration
    using MessageCallback =
        std::function<void(const std::string& raw, const FIXMessage& msg)>;

    explicit FIXFeedSimulator(std::string exchange = "FIX");
    ~FIXFeedSimulator();

    // ── Configuration (call before start()) ───────────────────────────────

    // Add a symbol to simulate.
    void add_symbol(const SimulatedSymbol& sym);

    // How often to emit a full 35=W snapshot per symbol (default: 5000 ms).
    void set_snapshot_interval_ms(int ms);

    // Incremental 35=X updates to emit per second across all symbols
    // (default: 10).  The actual per-symbol rate is this divided by the
    // number of symbols.
    void set_incremental_hz(int hz);

    // Configure periodic burst mode.  Every interval_ms milliseconds the
    // simulator raises its update rate by multiplier× for duration_ms, then
    // returns to the normal incremental_hz rate.  Pass interval_ms == 0 to
    // disable burst mode (default).  Must be called before start().
    void set_burst_params(int interval_ms, int multiplier, int duration_ms);

    // Callback invoked for every generated message.
    void set_callback(MessageCallback cb);

    // ── Lifecycle ─────────────────────────────────────────────────────────

    void start();
    void stop();

    bool               running()  const noexcept { return running_.load(); }
    const std::string& exchange() const noexcept { return exchange_; }

private:
    // Per-symbol mutable simulation state.
    struct SymbolState {
        SimulatedSymbol                        config;
        double                                 current_mid;
        int                                    seq_num = 1;
        std::chrono::steady_clock::time_point  last_snapshot{};  // epoch = not yet sent
        std::mt19937                           rng;
    };

    std::string              exchange_;
    std::vector<SymbolState> states_;
    MessageCallback          callback_;
    int                      snapshot_interval_ms_ = 5000;
    int                      incremental_hz_       = 10;
    int                      burst_interval_ms_    = 0;   // 0 = disabled
    int                      burst_multiplier_     = 10;
    int                      burst_duration_ms_    = 2000;

    std::thread       thread_;
    std::atomic<bool> running_{false};
    FIXParser         parser_;

    void simulation_loop();

    // Build and dispatch a 35=W full snapshot for one symbol.
    void emit_snapshot(SymbolState& s);

    // Build and dispatch a 35=X incremental (single level change) for one symbol.
    void emit_incremental(SymbolState& s);

    // Build depth levels centred on `mid`.
    // Returns MDEntry records: depth bids (descending) then depth asks (ascending).
    std::vector<MDEntry> make_levels(double mid, double spread_pct,
                                     double step, int depth) const;

    // Parse `raw` and invoke callback_.  Both are alive for the call's duration.
    void dispatch(const std::string& raw);
};
