#include "query_server_epoll.hpp"
#include "query_server.hpp"
#include "thread_affinity.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <unordered_map>

#ifdef __linux__
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// ─── Per-connection state ─────────────────────────────────────────────────────

#ifdef __linux__
namespace {

static constexpr int MAX_EVENTS  = 64;
static constexpr int INBUF_SIZE  = 512;   // enough for longest request line

struct Conn {
    int  fd    = -1;
    enum class State { READING, WRITING } state = State::READING;

    // Input: accumulate bytes until we find '\n'
    char inbuf[INBUF_SIZE] = {};
    int  inlen = 0;

    // Output: buffered response, sent in chunks
    std::string outbuf;
    size_t      outpos = 0;
};

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Disable Nagle algorithm — we send complete responses in one chunk, so
// TCP_NODELAY prevents a 40 ms delay on the ACK path.
static void set_tcp_nodelay(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

} // namespace
#endif // __linux__

// ─── EpollQueryServer ─────────────────────────────────────────────────────────

EpollQueryServer::EpollQueryServer(ArbitrageEngine* engine, uint16_t port)
    : engine_(engine), port_(port) {}

EpollQueryServer::~EpollQueryServer() {
    stop();
}

void EpollQueryServer::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&EpollQueryServer::serve_loop, this);
}

void EpollQueryServer::stop() {
    if (!running_.exchange(false)) return;

#ifdef __linux__
    // Closing server_fd_ unblocks epoll_wait; the loop checks running_ and exits.
    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
    }
#endif

    if (thread_.joinable()) thread_.join();
}

// ─── Request handler ─────────────────────────────────────────────────────────

std::string EpollQueryServer::handle_line(std::string_view line) {
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

// ─── Server loop (Linux-only) ─────────────────────────────────────────────────

#ifdef __linux__

void EpollQueryServer::serve_loop() {
    thread_affinity::set_thread_affinity(thread_affinity::TAG_QUERY_SERVER);

    // ── Create and bind TCP server socket ────────────────────────────────────
    server_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server_fd_ < 0) {
        std::cerr << "[epoll] socket() failed: " << strerror(errno) << '\n';
        running_ = false;
        return;
    }
    {
        int one = 1;
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        setsockopt(server_fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port_);

    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[epoll] bind() failed on port " << port_
                  << ": " << strerror(errno) << '\n';
        ::close(server_fd_);
        server_fd_ = -1;
        running_ = false;
        return;
    }
    if (::listen(server_fd_, SOMAXCONN) < 0) {
        std::cerr << "[epoll] listen() failed: " << strerror(errno) << '\n';
        ::close(server_fd_);
        server_fd_ = -1;
        running_ = false;
        return;
    }


    // ── Create epoll fd ───────────────────────────────────────────────────────
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        std::cerr << "[epoll] epoll_create1() failed: " << strerror(errno) << '\n';
        ::close(server_fd_);
        server_fd_ = -1;
        running_ = false;
        return;
    }

    // Register server socket for incoming connections
    epoll_event ev{};
    ev.events   = EPOLLIN;
    ev.data.fd  = server_fd_;
    epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd_, &ev);

    std::cout << "[epoll] query server listening on :" << port_ << '\n';

    // ── Per-connection state ──────────────────────────────────────────────────
    std::unordered_map<int, Conn> conns;
    epoll_event events[MAX_EVENTS];

    auto close_conn = [&](int fd) {
        epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
        conns.erase(fd);
    };

    // ── Event loop ────────────────────────────────────────────────────────────
    while (running_) {
        // 1 ms timeout so we can check running_ periodically
        int n = epoll_wait(epfd, events, MAX_EVENTS, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; ++i) {
            int  fd  = events[i].data.fd;
            auto evt = events[i].events;

            // ── New connection ────────────────────────────────────────────────
            if (fd == server_fd_) {
                while (true) {
                    int cfd = ::accept4(server_fd_, nullptr, nullptr,
                                        SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) break;   // EAGAIN: no more pending connections
                    set_tcp_nodelay(cfd);

                    Conn& c = conns[cfd];
                    c.fd    = cfd;

                    epoll_event cev{};
                    cev.events   = EPOLLIN | EPOLLET;
                    cev.data.fd  = cfd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev);
                }
                continue;
            }

            // ── Existing connection: error or hangup ──────────────────────────
            if (evt & (EPOLLHUP | EPOLLERR)) {
                close_conn(fd);
                continue;
            }

            auto it = conns.find(fd);
            if (it == conns.end()) { ::close(fd); continue; }
            Conn& c = it->second;

            // ── READING: recv bytes until we find '\n' ────────────────────────
            if (c.state == Conn::State::READING && (evt & EPOLLIN)) {
                while (true) {
                    int room = INBUF_SIZE - c.inlen - 1;
                    if (room <= 0) { close_conn(fd); goto next_event; }

                    ssize_t r = ::recv(fd, c.inbuf + c.inlen, room, 0);
                    if (r < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        close_conn(fd); goto next_event;
                    }
                    if (r == 0) { close_conn(fd); goto next_event; }   // EOF
                    c.inlen += static_cast<int>(r);

                    // Look for '\n' — one request per newline
                    char* nl = static_cast<char*>(memchr(c.inbuf, '\n', c.inlen));
                    if (!nl) continue;   // more data needed

                    *nl = '\0';
                    c.outbuf = handle_line(c.inbuf);
                    c.outpos = 0;

                    // Slide any remaining bytes to the front
                    int consumed = static_cast<int>(nl - c.inbuf) + 1;
                    c.inlen -= consumed;
                    if (c.inlen > 0) memmove(c.inbuf, nl + 1, c.inlen);

                    // Switch to writing mode
                    c.state = Conn::State::WRITING;
                    epoll_event wev{};
                    wev.events  = EPOLLOUT | EPOLLET;
                    wev.data.fd = fd;
                    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &wev);
                    break;
                }
            }

            // ── WRITING: send the buffered response ───────────────────────────
            if (c.state == Conn::State::WRITING && (evt & EPOLLOUT)) {
                while (c.outpos < c.outbuf.size()) {
                    ssize_t w = ::send(fd,
                        c.outbuf.data() + c.outpos,
                        c.outbuf.size() - c.outpos,
                        MSG_NOSIGNAL);
                    if (w < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        close_conn(fd); goto next_event;
                    }
                    c.outpos += static_cast<size_t>(w);
                }

                if (c.outpos >= c.outbuf.size()) {
                    // Response fully sent — go back to reading
                    c.outbuf.clear();
                    c.outpos = 0;
                    c.state  = Conn::State::READING;
                    epoll_event rev{};
                    rev.events  = EPOLLIN | EPOLLET;
                    rev.data.fd = fd;
                    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &rev);
                }
            }

            next_event:;
        }
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    for (auto& [fd, _] : conns) ::close(fd);
    ::close(epfd);
    if (server_fd_ >= 0) { ::close(server_fd_); server_fd_ = -1; }
    std::cout << "[epoll] query server stopped\n";
}

#else // !__linux__

void EpollQueryServer::serve_loop() {
    std::cout << "[epoll] query server not available on this platform (Linux only)\n";
    running_ = false;
}

#endif // __linux__
