#include "simple_query_server.hpp"
#include "query_server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

// ─── Constructor / Destructor ─────────────────────────────────────────────────

SimpleQueryServer::SimpleQueryServer(ArbitrageEngine* engine, uint16_t port)
    : engine_(engine), port_(port) {}

SimpleQueryServer::~SimpleQueryServer() { stop(); }

// ─── Lifecycle ────────────────────────────────────────────────────────────────

void SimpleQueryServer::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&SimpleQueryServer::serve_loop, this);
}

void SimpleQueryServer::stop() {
    if (!running_.exchange(false)) return;
    if (server_fd_ >= 0) { ::close(server_fd_); server_fd_ = -1; }
    if (thread_.joinable()) thread_.join();
}

// ─── Request dispatch ─────────────────────────────────────────────────────────

std::string SimpleQueryServer::handle_line(std::string_view line) {
    ParsedRequest req = parse_request(line);

    if (req.type == RequestType::SNAPSHOT) {
        auto books = engine_->get_snapshots(req.symbol);
        return snapshot_response(req.symbol, books);
    }
    if (req.type == RequestType::HEALTH) {
        auto staleness = engine_->get_feed_staleness_ms();
        return health_response(staleness);
    }
    if (req.type == RequestType::REPORT) {
        if (!pipeline_)
            return error_response("reporting pipeline not configured");
        return pipeline_->run(req.report_name);
    }
    if (req.type == RequestType::LISTREPORTS) {
        if (!pipeline_)
            return error_response("reporting pipeline not configured");
        return pipeline_->list_reports();
    }
    return error_response("unknown command — use SNAPSHOT <SYM>, HEALTH, REPORT <name>, or LISTREPORTS");
}

// ─── Per-client handler (runs on detached thread) ────────────────────────────

void SimpleQueryServer::handle_client(int fd) {
    // 5 s idle timeout — client should send a request promptly.
    struct timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[512];
    int  pos = 0;

    while (true) {
        ssize_t n = ::recv(fd, buf + pos, sizeof(buf) - pos - 1, 0);
        if (n <= 0) break;
        pos += static_cast<int>(n);
        buf[pos] = '\0';

        // Look for a complete line
        char* nl = static_cast<char*>(memchr(buf, '\n', static_cast<size_t>(pos)));
        if (!nl) {
            if (pos >= static_cast<int>(sizeof(buf)) - 1) break; // line too long
            continue;
        }
        *nl = '\0';

        std::string resp = handle_line(buf);
        ::send(fd, resp.data(), resp.size(), MSG_NOSIGNAL);

        // Slide remainder to front (support pipelining)
        int consumed = static_cast<int>(nl - buf) + 1;
        pos -= consumed;
        if (pos > 0) memmove(buf, nl + 1, static_cast<size_t>(pos));
    }
    ::close(fd);
}

// ─── Accept loop ─────────────────────────────────────────────────────────────

void SimpleQueryServer::serve_loop() {
    server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        std::cerr << "[simple] socket() failed: " << strerror(errno) << '\n';
        running_ = false;
        return;
    }
    {
        int one = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        int nodelay = 1;
        setsockopt(server_fd_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port_);

    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[simple] bind() failed on port " << port_
                  << ": " << strerror(errno) << '\n';
        ::close(server_fd_); server_fd_ = -1; running_ = false; return;
    }
    if (::listen(server_fd_, SOMAXCONN) < 0) {
        std::cerr << "[simple] listen() failed: " << strerror(errno) << '\n';
        ::close(server_fd_); server_fd_ = -1; running_ = false; return;
    }
    std::cout << "[simple] FIX query server listening on :" << port_ << '\n';

    while (running_) {
        // Use select() with a 100 ms timeout so stop() is never stuck.
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server_fd_, &fds);
        struct timeval tv{0, 100'000};   // 100 ms
        int n = ::select(server_fd_ + 1, &fds, nullptr, nullptr, &tv);
        if (n <= 0) continue;            // timeout or EINTR — check running_

        int cfd = ::accept(server_fd_, nullptr, nullptr);
        if (cfd < 0) continue;

        // Detach a thread for each client — request rate is low on this server.
        std::thread([this, cfd]() { handle_client(cfd); }).detach();
    }

    if (server_fd_ >= 0) { ::close(server_fd_); server_fd_ = -1; }
    std::cout << "[simple] FIX query server stopped\n";
}
