#pragma once

// simple_query_server.hpp
//
// Portable blocking TCP query server — works on macOS and Linux.
//
// Unlike EpollQueryServer / IoUringQueryServer, this uses a plain
// accept() loop with one thread per client connection.  It is NOT
// intended for benchmarking (no epoll, no io_uring).  Its purpose is
// to let a Mac-side FIX data server speak the same SNAPSHOT/HEALTH/
// REPORT wire protocol so that RemoteDataSource can connect to it.
//
// Connection model:
//   serve_loop()  — accept()s in a loop (100 ms select timeout so
//                   stop() is never stuck waiting).
//   handle_client() — blocking recv/send on the accepted fd, detached
//                   thread per connection.
//
// Thread-safety: handle_line() delegates to ArbitrageEngine (which is
// already thread-safe via shared_mutex) and ReportPipeline (stateless
// per call).

#include "arbitrage_engine.hpp"
#include "report_pipeline.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

class SimpleQueryServer {
public:
    explicit SimpleQueryServer(ArbitrageEngine* engine, uint16_t port = 9093);
    ~SimpleQueryServer();

    void set_pipeline(ReportPipeline* pipeline) noexcept { pipeline_ = pipeline; }

    void start();
    void stop();

    bool is_running() const noexcept { return running_.load(std::memory_order_relaxed); }

private:
    void        serve_loop();
    void        handle_client(int fd);       // runs on a detached thread
    std::string handle_line(std::string_view line);

    ArbitrageEngine* engine_;
    ReportPipeline*  pipeline_ = nullptr;
    uint16_t         port_;
    int              server_fd_ = -1;
    std::thread      thread_;
    std::atomic<bool> running_{false};
};
