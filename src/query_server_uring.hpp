#pragma once

// query_server_uring.hpp
//
// io_uring-based query server (Linux only, requires liburing).
//
// Syscall profile per request:
//   Operations: ACCEPT, RECV, SEND all written as SQEs into shared memory.
//   One io_uring_submit() call submits the entire batch → 1 syscall per batch
//   (vs 3 separate syscalls per request for epoll).
//
// The kernel processes SQEs asynchronously and writes CQEs into the completion
// ring.  Userspace reads CQEs from shared memory — no second syscall needed.
//
// User-data encoding (64 bits):
//   bits 0–31 : file descriptor (int32)
//   bits 32–39: operation type  (uint8: ACCEPT=0, RECV=1, SEND=2)
//
// Same wire protocol and handle_line() logic as EpollQueryServer, so both
// servers can be benchmarked identically with bench_query_client.

#include "arbitrage_engine.hpp"
#include "report_pipeline.hpp"
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

class IoUringQueryServer {
public:
    explicit IoUringQueryServer(ArbitrageEngine* engine, uint16_t port = 9092);
    ~IoUringQueryServer();

    // Attach a ReportPipeline — enables REPORT and LISTREPORTS commands.
    // Call before start().  Non-owning pointer.
    void set_pipeline(ReportPipeline* pipeline) noexcept { pipeline_ = pipeline; }

    void start();
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
