#pragma once

#include "types.hpp"
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <vector>
#include <functional>
#include <chrono>

// Full includes: FIXMessage and OrderBookSnapshot are used by value in the
// public API — forward declarations are not sufficient.
#include "order_book.hpp"   // also pulls in fix_parser.hpp

// Callback for arbitrage opportunities
using ArbitrageCallback = std::function<void(const ArbitrageOpportunity&)>;

class ArbitrageEngine {
public:
    ArbitrageEngine();
    ~ArbitrageEngine();

    // Start the arbitrage calculation thread
    void start();

    // Stop the engine
    void stop();

    // Accept a full L2 depth snapshot or incremental delta from a WebSocket
    // producer (Phase 2.4+).  Updates the per-exchange L2 OrderBook in ws_books_
    // and wakes the calculation thread.
    // Thread-safe: called from WebSocket producer threads.
    void update_order_book(const OrderBookSnapshot& snap);

    // Accept a parsed FIX market data message from the FIX feed simulator.
    // Routes to the per-symbol L2 OrderBook for exchange "FIX" in fix_books_
    // and wakes the calculation thread.
    // Thread-safe: may be called from the simulator thread concurrently with
    // update_order_book() calls from WebSocket producer threads.
    void update_fix_data(const FIXMessage& msg);

    // Set callback for when arbitrage opportunities are found
    void set_opportunity_callback(ArbitrageCallback callback);

    // Set minimum profit threshold in basis points (default: 0.1 bps)
    void set_min_profit_bps(double min_profit_bps);

    // Set max number of latency reports before auto-shutdown (0 = unlimited)
    void set_max_reports(int max_reports);

    // Set callback invoked when benchmark is complete (max reports reached)
    void set_shutdown_callback(std::function<void()> callback);

    // Get current arbitrage opportunities
    std::vector<ArbitrageOpportunity> get_opportunities() const;

    // Get statistics
    uint64_t get_calculation_count() const { return calculation_count_; }
    uint64_t get_opportunity_count() const { return opportunity_count_; }

    // Print periodic status report
    void print_latency_report() const;

private:
    // L2 order books for WebSocket producers — keyed by "EXCHANGE:SYMBOL"
    // (e.g. "Binance:BTCUSDT").  Updated by update_order_book() from producer
    // threads.  Read by the calculation thread in calculate_arbitrage().
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> ws_books_;
    mutable std::mutex                                           ws_books_mutex_;

    // L2 order books for the FIX feed — keyed by raw FIX symbol (e.g. "BTCUSD").
    // Updated by update_fix_data() from the simulator thread.
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> fix_books_;
    mutable std::mutex                                           fix_books_mutex_;

    // Arbitrage opportunities (result of the last calculate_arbitrage() call)
    std::vector<ArbitrageOpportunity> opportunities_;
    mutable std::mutex opportunities_mutex_;

    // Opportunity callback
    ArbitrageCallback opportunity_callback_;
    std::mutex callback_mutex_;

    // Thread management
    std::thread calculation_thread_;
    std::atomic<bool> running_;

    // Dirty flag + condvar: producers set books_dirty_ and notify cv_ whenever
    // any book is updated.  The calculation thread sleeps here until woken.
    std::atomic<bool>       books_dirty_;
    std::condition_variable cv_;
    std::mutex              cv_mutex_;

    // Statistics
    std::atomic<uint64_t> calculation_count_;
    std::atomic<uint64_t> opportunity_count_;

    // Configuration
    double min_profit_bps_;
    int max_reports_;
    int report_count_;

    // Shutdown callback (invoked when max_reports reached)
    std::function<void()> shutdown_callback_;

    // Main calculation loop (runs on calculation_thread_)
    void calculation_loop();

    // Scan all L2 books for cross-exchange arbitrage and populate opportunities_
    void calculate_arbitrage();

    // Normalize symbol format across exchanges to a canonical base currency
    // (e.g. "BTCUSDT" → "BTC", "BTC-USD" → "BTC", "BTC/USD" → "BTC")
    std::string normalize_symbol(const std::string& symbol) const;
};
