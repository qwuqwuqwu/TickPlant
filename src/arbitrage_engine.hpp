#pragma once

#include "types.hpp"
#include "exchange_queue.hpp"
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <vector>
#include <functional>
#include <chrono>

// Full includes: FIXMessage used by value in update_fix_data,
// OrderBookSnapshot used by value in update_order_book.
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

    // Update market data via per-exchange queues (thread-safe, lock-free push)
    void update_market_data(const TickerData& ticker);

    // Accept a full L2 depth snapshot from a WebSocket producer (Phase 2.4+).
    // Updates the per-exchange L2 OrderBook.  Does NOT push to the BBO queue —
    // the BBO path is handled separately by update_market_data() which the client
    // fires from the same depth message.  Both paths run in parallel until 2.6.
    // Thread-safe: called from WebSocket producer threads.
    void update_order_book(const OrderBookSnapshot& snap);

    // Accept a parsed FIX market data message from the FIX feed simulator.
    // Routes to:
    //   (a) the per-symbol L2 OrderBook for exchange "FIX"
    //   (b) the existing BBO queue via FIXMessage::to_ticker_data() so the
    //       current arbitrage detection path sees the FIX feed alongside the
    //       four WebSocket producers.
    // Thread-safe: may be called from the simulator thread concurrently with
    // update_market_data() calls from WebSocket producer threads.
    void update_fix_data(const FIXMessage& msg);

    // Set callback for when arbitrage opportunities are found
    void set_opportunity_callback(ArbitrageCallback callback);

    // Set minimum profit threshold in basis points (default: 10 bps)
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

    // Print latency report
    void print_latency_report() const;

private:
    // Shared queue for incoming data (all exchanges push to the same queue)
    SharedQueue incoming_queues_;

    // L2 order books for the FIX feed — keyed by raw FIX symbol (e.g. "BTCUSD").
    // Written by the FIX simulator thread; read by the calculation thread in 2.6.
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> fix_books_;
    mutable std::mutex                                           fix_books_mutex_;

    // L2 order books for WebSocket producers — keyed by "EXCHANGE:SYMBOL"
    // (e.g. "Binance:BTCUSDT").  Populated starting Phase 2.4 as each WebSocket
    // producer is upgraded to a depth feed.  Read by calculation thread in 2.6.
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> ws_books_;
    mutable std::mutex                                           ws_books_mutex_;

    // Market data storage (only accessed by calculation thread after draining queues)
    MarketDataMap market_data_;

    // Arbitrage opportunities
    std::vector<ArbitrageOpportunity> opportunities_;
    mutable std::mutex opportunities_mutex_;

    // Callback
    ArbitrageCallback opportunity_callback_;
    std::mutex callback_mutex_;

    // Thread management
    std::thread calculation_thread_;
    std::atomic<bool> running_;
    std::condition_variable cv_;
    std::mutex cv_mutex_;

    // Statistics
    std::atomic<uint64_t> calculation_count_;
    std::atomic<uint64_t> opportunity_count_;

    // Configuration
    double min_profit_bps_;
    int max_reports_;
    int report_count_;

    // Shutdown callback (invoked when max_reports reached)
    std::function<void()> shutdown_callback_;

    // Main calculation loop
    void calculation_loop();

    // Drain queues and update market_data_
    void drain_incoming_queues();

    // Calculate arbitrage opportunities
    void calculate_arbitrage();

    // Find arbitrage between two exchanges for a symbol
    ArbitrageOpportunity* find_arbitrage_for_symbol(
        const std::string& symbol,
        const TickerData& ticker1,
        const TickerData& ticker2
    );

    // Normalize symbol format (handle different exchange formats)
    std::string normalize_symbol(const std::string& symbol) const;
};
