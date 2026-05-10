#include "tick_logger.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

// ─── Constructor / Destructor ─────────────────────────────────────────────────

TickLogger::TickLogger(std::string host, uint16_t port)
    : host_(std::move(host)), port_(port)
{
    running_ = true;
    writer_thread_ = std::thread(&TickLogger::writer_loop, this);
}

TickLogger::~TickLogger() {
    running_ = false;
    pending_cv_.notify_all();
    if (writer_thread_.joinable()) writer_thread_.join();
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

// ─── Hot path: enqueue one ILP line ──────────────────────────────────────────

void TickLogger::log_bbo(std::string_view exchange,
                         std::string_view symbol,
                         double bid_px,
                         double ask_px,
                         double mid_px,
                         double spread_bps,
                         uint64_t timestamp_ns)
{
    // Format the ILP line.
    // order_book,exchange=<ex>,symbol=<sym> bid=<x>,ask=<x>,mid=<x>,spread_bps=<x> <ts>\n
    char buf[256];
    int n = std::snprintf(buf, sizeof(buf),
        "order_book,exchange=%.*s,symbol=%.*s "
        "bid=%g,ask=%g,mid=%g,spread_bps=%g "
        "%llu\n",
        static_cast<int>(exchange.size()), exchange.data(),
        static_cast<int>(symbol.size()),   symbol.data(),
        bid_px, ask_px, mid_px, spread_bps,
        static_cast<unsigned long long>(timestamp_ns));

    if (n <= 0 || n >= static_cast<int>(sizeof(buf))) return;

    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_.emplace_back(buf, static_cast<size_t>(n));
    }
    pending_cv_.notify_one();
}

// ─── Writer thread ────────────────────────────────────────────────────────────

void TickLogger::writer_loop() {
    std::vector<std::string> batch;
    batch.reserve(256);

    while (running_) {
        // Wait until there is something to send (or shutdown).
        {
            std::unique_lock<std::mutex> lk(pending_mu_);
            pending_cv_.wait_for(lk, std::chrono::milliseconds(200),
                [this]{ return !pending_.empty() || !running_; });

            if (pending_.empty()) continue;

            // Swap out the pending buffer — producers can keep enqueuing
            // while we flush without holding the lock.
            batch.swap(pending_);
        }

        // Ensure we have a live connection to QuestDB.
        if (!ensure_connected()) {
            // QuestDB not reachable — drop batch silently and retry later.
            batch.clear();
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        if (!flush_batch(batch)) {
            // Send failed — close socket so ensure_connected() reconnects.
            ::close(fd_);
            fd_ = -1;
            connected_ = false;
        }

        batch.clear();
    }
}

// ─── Connection helpers ───────────────────────────────────────────────────────

bool TickLogger::ensure_connected() {
    if (fd_ >= 0) return true;

    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return false;

    // TCP_NODELAY: ILP lines are small; don't let Nagle buffer them.
    int one = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port_);
    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
        ::close(fd_); fd_ = -1; return false;
    }
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd_); fd_ = -1; return false;
    }

    connected_ = true;
    std::cout << "[tick_logger] connected to QuestDB at "
              << host_ << ':' << port_ << '\n';
    return true;
}

bool TickLogger::flush_batch(const std::vector<std::string>& lines) {
    // Concatenate all lines into one send buffer to minimise syscalls.
    std::string buf;
    buf.reserve(lines.size() * 128);
    for (const auto& l : lines) buf += l;

    size_t sent = 0;
    while (sent < buf.size()) {
        ssize_t n = ::send(fd_, buf.data() + sent, buf.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}
