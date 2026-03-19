#pragma once

#include "types.hpp"
#include "exchange_queue.hpp"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <functional>
#include <chrono>

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
