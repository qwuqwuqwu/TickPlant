#pragma once

// tick_logger.hpp
//
// Streams BBO (best-bid/offer) tick records to a QuestDB instance via the
// InfluxDB Line Protocol (ILP) over TCP (port 9009).
//
// Design
// ──────
// Multiple WebSocket ingestion threads call log_bbo() concurrently.  Each
// call formats one ILP line and enqueues it into a mutex-protected pending
// buffer without blocking on network I/O.  A single background writer thread
// drains the buffer in batches via a double-buffer swap, reconnecting
// automatically if QuestDB is unavailable.
//
// ILP line format written to QuestDB:
//   order_book,exchange=Binance,symbol=BTC
//       bid=65432.10,ask=65433.00,mid=65432.55,spread_bps=1.37
//       1715000000000000000\n
//
// QuestDB auto-creates the table and columns on first insert.
//
// Usage (in main):
//   TickLogger logger("127.0.0.1", 9009);
//   engine.set_tick_logger(&logger);

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

class TickLogger {
public:
    explicit TickLogger(std::string host = "127.0.0.1", uint16_t port = 9009);
    ~TickLogger();

    // Non-blocking: formats one ILP line and enqueues it.
    // Safe to call concurrently from any thread.
    // timestamp_ns: nanoseconds since Unix epoch (0 = use QuestDB server time)
    void log_bbo(std::string_view exchange,
                 std::string_view symbol,
                 double bid_px,
                 double ask_px,
                 double mid_px,
                 double spread_bps,
                 uint64_t timestamp_ns);

    // Returns false if the writer thread has not yet connected (informational).
    bool is_connected() const noexcept { return connected_.load(); }

private:
    void writer_loop();
    bool ensure_connected();
    bool flush_batch(const std::vector<std::string>& lines);

    std::string  host_;
    uint16_t     port_;
    int          fd_        = -1;
    std::atomic<bool> connected_{false};

    // Double-buffer: producers append to pending_, writer swaps and drains.
    std::vector<std::string> pending_;
    std::mutex               pending_mu_;
    std::condition_variable  pending_cv_;

    std::thread       writer_thread_;
    std::atomic<bool> running_{false};
};
