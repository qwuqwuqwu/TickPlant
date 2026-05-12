#pragma once

// query_server_epoll.hpp
//
// epoll-based query server (Linux only).
//
// Syscall profile per request:
//   1. epoll_wait()    — wait for readable event
//   2. recv()          — read the request line
//   3. send()          — write the JSON response
//   Total: 3 syscalls (each crosses the user→kernel boundary independently)
//
// This is the baseline for comparison against the io_uring server (Phase 4.3),
// which batches all I/O into shared-memory rings and costs 1 syscall per batch.
//
// Usage:
//   EpollQueryServer srv(engine, 9092);
//   srv.start();          // spawns a server thread pinned to TAG_QUERY_SERVER
//   ...
//   srv.stop();           // drains the loop and joins the thread

#include "arbitrage_engine.hpp"
#include "report_pipeline.hpp"
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

class EpollQueryServer {
public:
    explicit EpollQueryServer(ArbitrageEngine* engine, uint16_t port = 9092);
    ~EpollQueryServer();

    // Attach a ReportPipeline — enables REPORT and LISTREPORTS commands.
    // Call before start().  Non-owning pointer.
    void set_pipeline(ReportPipeline* pipeline) noexcept { pipeline_ = pipeline; }

    // Start the server thread.  Returns immediately; server accepts connections
    // asynchronously on its own thread.
    void start();

    // Stop the server.  Closes the server socket and joins the thread.
    void stop();

    bool is_running() const noexcept { return running_.load(std::memory_order_relaxed); }

private:
    void        serve_loop();
    std::string handle_line(std::string_view line);

    ArbitrageEngine* engine_;
    ReportPipeline*  pipeline_ = nullptr;
    uint16_t         port_;
    int              server_fd_ = -1;
    std::thread      thread_;
    std::atomic<bool> running_{false};
};
