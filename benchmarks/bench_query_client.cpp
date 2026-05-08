// bench_query_client.cpp
//
// Standalone RTT benchmark for the TickPlant query server (Phase 4).
// No TickPlant dependencies — only POSIX sockets + C++17.
//
// Measures round-trip latency for sequential SNAPSHOT requests on a single
// persistent TCP connection: send "SNAPSHOT <sym>\n", read until '\n', record.
//
// Usage:
//   ./bench_query_client [options]
//
//   --host  HOST   server address (default: 127.0.0.1)
//   --port  PORT   server port    (default: 9092)
//   --sym   SYM    symbol to query (default: BTC)
//   --n     N      number of measured requests (default: 100000)
//   --warmup W     warmup iterations before measurement (default: 1000)
//
// Output:
//   p50  / p99 / p999 latency in microseconds
//   throughput in requests/second

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <numeric>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

// ─── Helpers ──────────────────────────────────────────────────────────────────

static int connect_to(const char* host, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    // Disable Nagle — we send small request lines; ACK delay would inflate RTT.
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "inet_pton failed for '%s'\n", host);
        ::close(fd);
        return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("connect");
        ::close(fd);
        return -1;
    }
    return fd;
}

// Send all bytes in buf; return false on error.
static bool send_all(int fd, const char* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Read from fd until '\n' is found; return false on error or EOF.
// Reads in large chunks to avoid per-byte syscall overhead.
static bool recv_line(int fd, char* buf, size_t bufsz) {
    size_t pos = 0;
    while (pos < bufsz - 1) {
        ssize_t n = ::recv(fd, buf + pos, bufsz - pos - 1, 0);
        if (n <= 0) return false;
        // Scan the newly received chunk for '\n'
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[pos + i] == '\n') {
                buf[pos + i + 1] = '\0';
                return true;
            }
        }
        pos += static_cast<size_t>(n);
    }
    return false;  // response too large for buffer
}

// ─── Percentile calculation ───────────────────────────────────────────────────

static double percentile(std::vector<uint64_t>& v, double pct) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(pct / 100.0 * static_cast<double>(v.size()));
    if (idx >= v.size()) idx = v.size() - 1;
    return static_cast<double>(v[idx]);
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    const char*  host    = "127.0.0.1";
    uint16_t     port    = 9092;
    const char*  sym     = "BTC";
    int          n_meas  = 100'000;
    int          warmup  = 1'000;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--host")   && i + 1 < argc) host   = argv[++i];
        else if (!strcmp(argv[i], "--port")   && i + 1 < argc) port   = static_cast<uint16_t>(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--sym")    && i + 1 < argc) sym    = argv[++i];
        else if (!strcmp(argv[i], "--n")      && i + 1 < argc) n_meas = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) warmup = atoi(argv[++i]);
        else { fprintf(stderr, "unknown flag: %s\n", argv[i]); return 1; }
    }

    printf("bench_query_client  host=%s port=%u sym=%s n=%d warmup=%d\n",
           host, port, sym, n_meas, warmup);

    int fd = connect_to(host, port);
    if (fd < 0) return 1;

    std::string request = "SNAPSHOT ";
    request += sym;
    request += "\n";

    char resp_buf[65536];

    // ── Warmup ────────────────────────────────────────────────────────────────
    printf("warming up (%d requests)...\n", warmup);
    for (int i = 0; i < warmup; ++i) {
        if (!send_all(fd, request.data(), request.size())) {
            fprintf(stderr, "send failed during warmup\n");
            ::close(fd);
            return 1;
        }
        if (!recv_line(fd, resp_buf, sizeof(resp_buf))) {
            fprintf(stderr, "recv failed during warmup\n");
            ::close(fd);
            return 1;
        }
    }

    // ── Measurement ───────────────────────────────────────────────────────────
    printf("measuring %d requests...\n", n_meas);
    std::vector<uint64_t> latencies_ns;
    latencies_ns.reserve(n_meas);

    auto wall_start = std::chrono::steady_clock::now();

    for (int i = 0; i < n_meas; ++i) {
        auto t0 = std::chrono::steady_clock::now();

        if (!send_all(fd, request.data(), request.size())) {
            fprintf(stderr, "send failed at iteration %d\n", i);
            break;
        }
        if (!recv_line(fd, resp_buf, sizeof(resp_buf))) {
            fprintf(stderr, "recv failed at iteration %d\n", i);
            break;
        }

        auto t1 = std::chrono::steady_clock::now();
        latencies_ns.push_back(
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }

    auto wall_end = std::chrono::steady_clock::now();
    ::close(fd);

    if (latencies_ns.empty()) { fprintf(stderr, "no measurements collected\n"); return 1; }

    // ── Report ────────────────────────────────────────────────────────────────
    double wall_s = std::chrono::duration<double>(wall_end - wall_start).count();
    double throughput = static_cast<double>(latencies_ns.size()) / wall_s;

    double p50  = percentile(latencies_ns, 50.0)  / 1000.0;  // ns → µs
    double p99  = percentile(latencies_ns, 99.0)  / 1000.0;
    double p999 = percentile(latencies_ns, 99.9)  / 1000.0;
    double mean = static_cast<double>(
        std::accumulate(latencies_ns.begin(), latencies_ns.end(), uint64_t{0}))
        / static_cast<double>(latencies_ns.size()) / 1000.0;

    printf("\n─── Results ───────────────────────────────────────\n");
    printf("  Requests measured : %zu\n",    latencies_ns.size());
    printf("  Throughput        : %.0f req/s\n", throughput);
    printf("  Mean RTT          : %.2f µs\n", mean);
    printf("  p50  RTT          : %.2f µs\n", p50);
    printf("  p99  RTT          : %.2f µs\n", p99);
    printf("  p999 RTT          : %.2f µs\n", p999);
    printf("───────────────────────────────────────────────────\n");

    return 0;
}
