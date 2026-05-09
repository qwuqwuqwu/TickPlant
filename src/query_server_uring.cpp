#include "query_server_uring.hpp"
#include "query_server.hpp"
#include "thread_affinity.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <unordered_map>

// io_uring is Linux-only and requires liburing.
// CMakeLists.txt sets HAS_LIBURING when the library is found.
#if defined(__linux__) && defined(HAS_LIBURING)
#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#define URING_AVAILABLE 1
#endif

// ─── User-data encoding ───────────────────────────────────────────────────────

#ifdef URING_AVAILABLE
namespace {

static constexpr int    RING_DEPTH  = 256;  // SQ/CQ depth — power of 2
static constexpr int    INBUF_SIZE  = 512;

enum class Op : uint8_t { ACCEPT = 0, RECV = 1, SEND = 2 };

// Pack fd + op into 64-bit user_data attached to each SQE.
static inline uint64_t encode_ud(int fd, Op op) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(fd))) |
           (static_cast<uint64_t>(static_cast<uint8_t>(op)) << 32);
}

static inline void decode_ud(uint64_t ud, int& fd, Op& op) {
    fd = static_cast<int>(static_cast<uint32_t>(ud & 0xFFFFFFFFull));
    op = static_cast<Op>((ud >> 32) & 0xFF);
}

// Per-connection buffers.
struct Conn {
    char   inbuf[INBUF_SIZE] = {};
    int    inlen  = 0;
    std::string outbuf;
    size_t outpos = 0;
};

static void set_tcp_nodelay(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

} // namespace
#endif // URING_AVAILABLE

// ─── IoUringQueryServer ───────────────────────────────────────────────────────

IoUringQueryServer::IoUringQueryServer(ArbitrageEngine* engine, uint16_t port)
    : engine_(engine), port_(port) {}

IoUringQueryServer::~IoUringQueryServer() {
    stop();
}

void IoUringQueryServer::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&IoUringQueryServer::serve_loop, this);
}

void IoUringQueryServer::stop() {
    if (!running_.exchange(false)) return;
#ifdef URING_AVAILABLE
    if (server_fd_ >= 0) { ::close(server_fd_); server_fd_ = -1; }
#endif
    if (thread_.joinable()) thread_.join();
}

std::string IoUringQueryServer::handle_line(std::string_view line) {
    ParsedRequest req = parse_request(line);

    if (req.type == RequestType::SNAPSHOT) {
        auto books = engine_->get_snapshots(req.symbol);
        return snapshot_response(req.symbol, books);
    }
    if (req.type == RequestType::HEALTH) {
        auto staleness = engine_->get_feed_staleness_ms();
        return health_response(staleness);
    }
    return error_response("unknown command — use SNAPSHOT <SYM> or HEALTH");
}

// ─── Server loop ─────────────────────────────────────────────────────────────

#ifdef URING_AVAILABLE

void IoUringQueryServer::serve_loop() {
    thread_affinity::set_thread_affinity(thread_affinity::TAG_QUERY_SERVER);

    // ── Server socket ─────────────────────────────────────────────────────────
    server_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server_fd_ < 0) {
        std::cerr << "[uring] socket() failed: " << strerror(errno) << '\n';
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
        std::cerr << "[uring] bind() failed: " << strerror(errno) << '\n';
        ::close(server_fd_); server_fd_ = -1; running_ = false; return;
    }
    if (::listen(server_fd_, SOMAXCONN) < 0) {
        std::cerr << "[uring] listen() failed: " << strerror(errno) << '\n';
        ::close(server_fd_); server_fd_ = -1; running_ = false; return;
    }

    // ── io_uring setup ────────────────────────────────────────────────────────
    io_uring ring{};
    if (io_uring_queue_init(RING_DEPTH, &ring, 0) < 0) {
        std::cerr << "[uring] io_uring_queue_init() failed: " << strerror(errno) << '\n';
        ::close(server_fd_); server_fd_ = -1; running_ = false; return;
    }

    std::cout << "[uring] query server listening on :" << port_ << '\n';

    // ── Per-connection state ──────────────────────────────────────────────────
    std::unordered_map<int, Conn> conns;

    // Shared accept address storage (reused for every accept SQE).
    sockaddr_in  peer_addr{};
    socklen_t    peer_addrlen = sizeof(peer_addr);

    // Helper: submit an ACCEPT SQE so the kernel queues up the next connection.
    auto submit_accept = [&]() {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        io_uring_prep_accept(sqe, server_fd_,
                             reinterpret_cast<sockaddr*>(&peer_addr),
                             &peer_addrlen, SOCK_CLOEXEC);
        io_uring_sqe_set_data64(sqe, encode_ud(server_fd_, Op::ACCEPT));
    };

    // Helper: submit a RECV SQE for an existing connection.
    auto submit_recv = [&](int cfd) {
        Conn& c = conns[cfd];
        io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        io_uring_prep_recv(sqe, cfd,
                           c.inbuf + c.inlen,
                           INBUF_SIZE - c.inlen - 1, 0);
        io_uring_sqe_set_data64(sqe, encode_ud(cfd, Op::RECV));
    };

    // Helper: submit a SEND SQE for the pending outbuf.
    auto submit_send = [&](int cfd) {
        Conn& c = conns[cfd];
        io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        io_uring_prep_send(sqe, cfd,
                           c.outbuf.data() + c.outpos,
                           c.outbuf.size() - c.outpos,
                           MSG_NOSIGNAL);
        io_uring_sqe_set_data64(sqe, encode_ud(cfd, Op::SEND));
    };

    // Seed the ring with the first ACCEPT.
    submit_accept();
    io_uring_submit(&ring);

    // ── CQE processing loop ───────────────────────────────────────────────────
    while (running_) {
        io_uring_cqe* cqe = nullptr;

        // Wait up to 1 ms so we can re-check running_.
        // Short timeout bounds tail-latency spikes caused by missed CQEs.
        __kernel_timespec ts{0, 1'000'000};    // 1 ms
        int ret = io_uring_wait_cqe_timeout(&ring, &cqe, &ts);
        if (ret == -ETIME) continue;   // timeout, loop back and check running_
        if (ret < 0) {
            if (ret == -EINTR) continue;
            std::cerr << "[uring] wait_cqe_timeout: " << strerror(-ret) << '\n';
            break;
        }

        int fd; Op op;
        decode_ud(io_uring_cqe_get_data64(cqe), fd, op);
        int res = cqe->res;
        io_uring_cqe_seen(&ring, cqe);

        // ── ACCEPT completion ─────────────────────────────────────────────────
        if (op == Op::ACCEPT) {
            // Re-arm accept for the next incoming connection immediately.
            // We do this before handling the new connection so the kernel can
            // queue up the next accept in parallel.
            submit_accept();

            if (res < 0) {
                // ECONNABORTED or similar — just keep going
                io_uring_submit(&ring);
                continue;
            }

            int cfd = res;
            set_tcp_nodelay(cfd);
            conns.emplace(cfd, Conn{});
            submit_recv(cfd);   // ask kernel to read the first request
            io_uring_submit(&ring);
            continue;
        }

        // ── RECV completion ───────────────────────────────────────────────────
        if (op == Op::RECV) {
            if (res <= 0) {
                // EOF or error: close and clean up
                ::close(fd);
                conns.erase(fd);
                io_uring_submit(&ring);
                continue;
            }

            Conn& c = conns[fd];
            c.inlen += res;

            // Look for '\n' (end of request line)
            char* nl = static_cast<char*>(memchr(c.inbuf, '\n', c.inlen));
            if (!nl) {
                // Incomplete line — wait for more data
                if (c.inlen >= INBUF_SIZE - 1) {
                    // Buffer full with no newline: protocol error, close
                    ::close(fd); conns.erase(fd);
                } else {
                    submit_recv(fd);
                }
                io_uring_submit(&ring);
                continue;
            }

            *nl = '\0';
            c.outbuf = handle_line(c.inbuf);
            c.outpos = 0;

            // Slide leftover bytes to front of inbuf
            int consumed = static_cast<int>(nl - c.inbuf) + 1;
            c.inlen -= consumed;
            if (c.inlen > 0) memmove(c.inbuf, nl + 1, c.inlen);

            submit_send(fd);
            io_uring_submit(&ring);
            continue;
        }

        // ── SEND completion ───────────────────────────────────────────────────
        if (op == Op::SEND) {
            if (res <= 0) {
                ::close(fd); conns.erase(fd);
                io_uring_submit(&ring);
                continue;
            }

            Conn& c = conns[fd];
            c.outpos += static_cast<size_t>(res);

            if (c.outpos < c.outbuf.size()) {
                // Partial send: re-submit for remaining bytes
                submit_send(fd);
            } else {
                // Response fully sent: wait for next request
                c.outbuf.clear();
                c.outpos = 0;
                submit_recv(fd);
            }
            io_uring_submit(&ring);
            continue;
        }
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    for (auto& [cfd, _] : conns) ::close(cfd);
    io_uring_queue_exit(&ring);
    if (server_fd_ >= 0) { ::close(server_fd_); server_fd_ = -1; }
    std::cout << "[uring] query server stopped\n";
}

#else // !URING_AVAILABLE

void IoUringQueryServer::serve_loop() {
#ifdef __linux__
    std::cout << "[uring] liburing not found — rebuild with liburing installed\n";
#else
    std::cout << "[uring] query server not available on this platform (Linux only)\n";
#endif
    running_ = false;
}

#endif // URING_AVAILABLE
