#pragma once

#include "types.hpp"
#include "queue_latency_tracker.hpp"
#include "mpsc_ring_buffer.hpp"
#include <mutex>
#include <queue>
#include <string>
#include <iostream>

// ============================================================================
// Compile-time switch: USE_MPSC_QUEUE
// Define this to use lock-free MPSC queue instead of shared mutex queue
// ============================================================================
// #define USE_MPSC_QUEUE

// ============================================================================
// Shared mutex queue (BASELINE — real contention from 3 producers)
// All exchange WebSocket threads push to the same mutex-protected queue.
// ============================================================================
class MutexSharedQueue {
public:
    MutexSharedQueue() = default;

    // Push ticker data — 3 producers contend on the same lock
    void push(TickerData ticker) {
        // Capture exchange name BEFORE move (moved-from string is undefined)
        std::string exchange = ticker.exchange;

        uint64_t start_tsc = QueueLatencyTracker::get_enqueue_tsc();

        size_t occupancy = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            occupancy = queue_.size();
            queue_.push(std::move(ticker));
        }

        uint64_t end_tsc = QueueLatencyTracker::get_enqueue_tsc();
        get_queue_latency_tracker().record_operation(exchange, start_tsc, end_tsc, occupancy);
    }

    // Drain all items into market data map (single consumer)
    size_t drain_all(MarketDataMap& market_data) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t count = 0;

        while (!queue_.empty()) {
            TickerData ticker = std::move(queue_.front());
            queue_.pop();
            std::string key = make_ticker_key(ticker.exchange, ticker.symbol);
            market_data[key] = std::move(ticker);
            count++;
        }

        return count;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    void report_drops() const {
        // Mutex queue doesn't drop messages
    }

private:
    mutable std::mutex mutex_;
    std::queue<TickerData> queue_;
};

// ============================================================================
// Lock-free MPSC queue (OPTIMIZED — no locks, CAS-based)
// All exchange WebSocket threads push via atomic CAS operations.
// ============================================================================
class MPSCSharedQueue {
public:
    static constexpr size_t QUEUE_SIZE = 4096;  // Must be power of 2

    MPSCSharedQueue() = default;

    // Push ticker data — lock-free CAS, no mutex
    void push(TickerData ticker) {
        // Capture exchange name BEFORE move
        std::string exchange = ticker.exchange;

        size_t occupancy = queue_.size();
        uint64_t start_tsc = QueueLatencyTracker::get_enqueue_tsc();

        if (!queue_.try_push(std::move(ticker))) {
            dropped_count_.fetch_add(1, std::memory_order_relaxed);
        }

        uint64_t end_tsc = QueueLatencyTracker::get_enqueue_tsc();
        get_queue_latency_tracker().record_operation(exchange, start_tsc, end_tsc, occupancy);
    }

    // Drain all items into market data map (single consumer)
    size_t drain_all(MarketDataMap& market_data) {
        size_t count = 0;
        TickerData ticker;

        while (queue_.try_pop(ticker)) {
            std::string key = make_ticker_key(ticker.exchange, ticker.symbol);
            market_data[key] = std::move(ticker);
            count++;
        }

        return count;
    }

    bool empty() const {
        return queue_.empty();
    }

    // Report dropped messages (queue full events)
    void report_drops() const {
        uint64_t drops = dropped_count_.load(std::memory_order_relaxed);
        if (drops > 0) {
            std::cout << "MPSC Queue drops: " << drops << std::endl;
        }
    }

private:
    MPSCRingBuffer<TickerData, QUEUE_SIZE> queue_;
    std::atomic<uint64_t> dropped_count_{0};
};

// ============================================================================
// Type alias selected by compile-time switch
// ============================================================================
#ifdef USE_MPSC_QUEUE
using SharedQueue = MPSCSharedQueue;
#else
using SharedQueue = MutexSharedQueue;
#endif
