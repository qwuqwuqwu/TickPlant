# TickPlant — Low-Latency Market Data Platform

A high-performance market data platform written in C++20. Aggregates live
L2 order books from four cryptocurrency exchanges and a FIX feed simulator,
detects cross-exchange arbitrage opportunities in real time, and exposes the
data via a microsecond-latency TCP query server backed by either **epoll** or
**io_uring**.

## Features

- **Real-time L2 Order Books**: Live WebSocket feeds from 4 exchanges
  - Binance (depth20, 100 ms)
  - Coinbase Advanced Trade (level2 full book)
  - Kraken v2 (BBO event-triggered)
  - Bybit (orderbook.50)

- **FIX Feed Simulator**: Synthetic burst feed over TCP using a hand-rolled
  FIX 4.2 parser — no third-party FIX library

- **Multi-Symbol Monitoring**: 9 cryptocurrency pairs simultaneously
  - BTC, ETH, ADA, DOT, SOL, MATIC, AVAX, LTC, LINK

- **Cross-Exchange Arbitrage Detection**: Real-time L2 spread comparison
  - Configurable minimum profit threshold (default: 5 basis points)
  - Quantity-gated: phantom arbitrage from zero-size quotes filtered out

- **Phase 4 — TCP Query Server**: epoll or io_uring backend, newline-delimited
  protocol, `SNAPSHOT <SYM>` and `HEALTH` commands

- **Prometheus Metrics**: per-feed staleness, book depth, arbitrage alerts
  (pull model, port 9090)

## Prerequisites

### macOS (Your Setup)
```bash
# Install vcpkg (package manager)
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh
export VCPKG_ROOT=$(pwd)
export PATH=$VCPKG_ROOT:$PATH

# Install dependencies
./vcpkg install websocketpp nlohmann-json openssl boost-system boost-thread boost-chrono boost-random

# Install CMake if not already installed
brew install cmake
```

### Required Dependencies
- **C++20 compatible compiler** (Clang 12+ or GCC 10+)
- **CMake 3.20+**
- **vcpkg** (for package management)
- **OpenSSL** (for TLS connections)
- **WebSocket++** (for WebSocket client)
- **nlohmann/json** (for JSON parsing)
- **Boost libraries** (system, thread, chrono, random)

## Build Instructions

```bash
# Clone and navigate to project directory
cd binance_dashboard

# Create build directory
mkdir build && cd build

# Configure with vcpkg integration
cmake .. -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

# Build the project
make -j$(nproc)

# Or for release build with optimizations
cmake .. -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

## Usage

```bash
# Run with epoll query server (Linux)
./binance_dashboard --query-server epoll --query-port 9092

# Run with io_uring query server (Linux, requires liburing)
./binance_dashboard --query-server uring --query-port 9092

# Query a live L2 snapshot
echo "SNAPSHOT BTC" | nc -q 1 127.0.0.1 9092

# Check feed health
echo "HEALTH" | nc -q 1 127.0.0.1 9092

# Run the RTT benchmark (standalone, no deps)
./bench_query_client --sym BTC --n 10000 --warmup 500

# Benchmark with CPU pinning (Linux, after isolcpus=2,3 in grub)
taskset -c 3 ./binance_dashboard --query-server epoll --query-port 9092 &
taskset -c 2 ./bench_query_client --sym BTC --n 10000 --warmup 500
```

## Architecture

### Low-Latency Design
- **`shared_mutex` on order books** — query readers and arbitrage detection
  run concurrently; only WebSocket update callbacks hold exclusive locks
- **Edge-triggered epoll (EPOLLET)** — one syscall drains all pending data;
  no redundant wake-ups
- **io_uring ring buffer** — RECV/SEND submitted via shared SQ; completions
  harvested from CQ without per-operation kernel crossings
- **TCP_NODELAY on all connections** — disables Nagle; eliminates 40 ms
  ACK-delay on small request/response messages
- **CPU affinity + `isolcpus`** — query server thread pinned to a fully
  isolated core (no OS scheduler ticks, no competing threads)

### Components
- `binance_client`: WebSocket client — Binance depth20 @ 100 ms
- `coinbase_client`: WebSocket client — Coinbase Advanced Trade level2
- `kraken_client`: WebSocket client — Kraken v2 BBO event-triggered
- `bybit_client`: WebSocket client — Bybit orderbook.50
- `fix_feed_simulator`: Synthetic TCP FIX 4.2 burst feed
- `fix_parser`: Hand-rolled FIX 4.2 tag parser (no third-party lib)
- `order_book`: Thread-safe L2 order book with `shared_mutex`
- `arbitrage_engine`: Real-time cross-exchange arbitrage detection
- `query_server_epoll`: Edge-triggered epoll query server (Linux)
- `query_server_uring`: io_uring query server via liburing (Linux)
- `metrics`: Prometheus pull-model metrics (port 9090)
- `dashboard`: Terminal UI with color-coded live display

## Monitored Symbols

The dashboard tracks these cryptocurrency pairs:
- **BTCUSDT** - Bitcoin
- **ETHUSDT** - Ethereum
- **ADAUSDT** - Cardano
- **DOTUSDT** - Polkadot
- **SOLUSDT** - Solana
- **MATICUSDT** - Polygon
- **AVAXUSDT** - Avalanche
- **LTCUSDT** - Litecoin
- **LINKUSDT** - Chainlink

## Dashboard Features

### Real-time Display
- **Current Prices**: Live bid/ask prices
- **Spreads**: Real-time spread in basis points
- **Volume**: Available liquidity at best prices
- **Status**: Connection health (LIVE/SLOW/STALE)
- **Statistics**: Update counts and average spreads

### Color Coding
- **🟢 Green**: Price increases, active connections
- **🔴 Red**: Price decreases, connection issues
- **🟡 Yellow**: Neutral/warning states
- **🔵 Blue**: Information display
- **🔷 Cyan**: Headers and borders

### Controls
- **Ctrl+C**: Graceful shutdown
- **Auto-refresh**: Every 500ms
- **Auto-reconnect**: On connection failures

## Phase 4 — Low-Latency Query Server

A TCP query server exposes live L2 order-book snapshots over a simple
newline-delimited text protocol.  Two I/O backends are implemented and
benchmarked head-to-head.

### Wire Protocol

```
# Request (client → server)
SNAPSHOT BTC\n
HEALTH\n

# Response (server → client) — compact JSON + newline
{"status":"ok","symbol":"BTC","books":[{"exchange":"Binance","bids":[[65432.10,0.42],...],"asks":[[65433.00,0.18],...]},...]}
{"status":"ok","feeds":{"Binance":{"staleness_ms":12,"live":true},...}}
```

### I/O Backends

| Backend | Mechanism | Syscalls per request |
|---------|-----------|----------------------|
| **epoll** | `epoll_wait` → `recv` → `send` (edge-triggered) | ~3 |
| **io_uring** | `io_uring_enter` batches RECV+SEND via shared ring buffer | ~1 |

Launch with `--query-server epoll` or `--query-server uring` (default port 9092).

### Benchmark Results

**Hardware**: Intel Core i5-2410M (Sandy Bridge, 2× physical cores, 4× logical, 2.3 GHz)  
**OS**: Ubuntu 22.04, kernel 6.8, `isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3`  
**Method**: server pinned to isolated CPU 3 (`taskset -c 3`), client pinned to isolated
CPU 2 (`taskset -c 2`); single persistent TCP connection; 10,000 measured requests,
500 warmup; loopback interface

```
bench_query_client --sym BTC --n 10000 --warmup 500
```

| Metric | epoll | io_uring |
|--------|------:|--------:|
| Throughput | ~600–620 req/s | ~490–650 req/s |
| p50 RTT | ~1.6 ms | ~1.6 ms |
| p99 RTT | ~2.0 ms | ~1.7–8.0 ms |
| p999 RTT | ~2–8 ms | ~2–14 ms |

**Both backends achieve ~1.6 ms median on this hardware.**  Tail latency
is dominated by L3 cache pressure from the four concurrent WebSocket
ingestion threads (Binance, Coinbase, Kraken, Bybit) which share the
same 3 MB L3 cache as the isolated query-server core.  Run-to-run
variance is high enough that per-run p999 numbers are not directly
comparable.

The meaningful comparison is **architectural**, not numeric:

| Property | epoll | io_uring |
|----------|-------|----------|
| Syscalls per request | ~3 (`epoll_wait` + `recv` + `send`) | ~1 (`io_uring_enter` batches via ring) |
| Kernel boundary crossings | Per-event | Per-batch |
| Memory model | FD-based event table | Shared SQ/CQ ring buffers |
| Best fit | Predictable single-connection latency | High-concurrency throughput |

On production hardware with dedicated isolated cores and no shared-cache
neighbours, io_uring's syscall reduction translates directly to lower
median and tail latency at high connection counts.

### Key Engineering Decisions

- **`shared_mutex` for order-book reads** — the arbitrage calculation loop
  originally held an exclusive write lock on `ws_books_mutex_` while scanning
  all books and updating Prometheus metrics, blocking every concurrent query
  reader for milliseconds.  Downgrading to `shared_lock` in all read-only paths
  (`get_snapshots`, `list_symbols`, `calculate_arbitrage`) dropped median RTT
  from ~14 ms to ~1.6 ms.

- **Edge-triggered epoll (EPOLLET)** — avoids redundant `epoll_wait` wake-ups;
  the server drains all available bytes per event.

- **TCP_NODELAY on both sides** — disables Nagle's algorithm; prevents 40 ms
  ACK-delay inflation on small request lines.

- **1 ms poll timeout** — both servers check `running_` every 1 ms rather than
  50 ms, bounding worst-case shutdown latency and eliminating timeout-induced
  tail spikes in the io_uring CQE wait loop.

## Performance Characteristics

### Typical Performance (on Intel i5 MacBook Pro)
- **Latency**: 1-5ms from WebSocket to display
- **Throughput**: 1000-5000 messages/second
- **Memory Usage**: ~50MB steady state
- **CPU Usage**: 5-15% on 4-core system

### Network Requirements
- **Bandwidth**: ~10-50 KB/s depending on market activity
- **Internet**: Stable connection to stream.binance.com
- **Firewall**: Allow HTTPS/WSS connections on port 9443

## Architecture Overview

```
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│                 │    │                  │    │                 │
│  Binance API    │───▶│  WebSocket       │───▶│  Terminal       │
│  (WebSocket)    │    │  Client          │    │  Dashboard      │
│                 │    │                  │    │                 │
└─────────────────┘    └──────────────────┘    └─────────────────┘
                              │
                              ▼
                       ┌──────────────────┐
                       │                  │
                       │  JSON Parser     │
                       │  Message Queue   │
                       │                  │
                       └──────────────────┘
```

### Key Components

1. **BinanceWebSocketClient**: Manages WebSocket connection and data parsing
2. **TerminalDashboard**: Handles display logic and real-time updates  
3. **TickerData**: Data structure for market information
4. **Message Threading**: Separate threads for networking and display

## Troubleshooting

### Build Issues
```bash
# If CMake can't find vcpkg packages:
export VCPKG_ROOT=/path/to/vcpkg
cmake .. -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

# If missing dependencies:
cd $VCPKG_ROOT
./vcpkg install websocketpp nlohmann-json openssl boost-system boost-thread
```

### Runtime Issues
```bash
# Connection timeout:
# - Check internet connection
# - Verify Binance API accessibility
# - Try running with verbose output

# Display issues:
# - Ensure terminal supports UTF-8 and ANSI colors
# - Try resizing terminal window
# - Use modern terminal (Terminal.app, iTerm2)
```

### Network Issues
- **Firewall**: Ensure WSS connections are allowed
- **Proxy**: Set HTTP_PROXY/HTTPS_PROXY if needed
- **DNS**: Verify stream.binance.com resolves correctly

## Development Notes

### Performance Optimizations
- Zero-copy JSON parsing where possible
- Lock-free data structures for message passing
- Efficient string formatting and display updates
- Memory pool allocation for high-frequency objects

### Code Quality
- Modern C++20 features and best practices
- RAII for resource management
- Thread-safe design with minimal locking
- Comprehensive error handling and logging

### Extension Points
- Easy to add new exchanges (inherit from base client)
- Configurable symbol lists
- Pluggable display formats (JSON, CSV, database)
- Arbitrage detection algorithms (next phase)

## Exchange-Specific Notes

### Binance.US
- Limited symbol availability compared to Binance.com
- Wider spreads due to lower liquidity
- Some pairs (MATIC, LINK) not available on WebSocket

### Coinbase
- Excellent liquidity and tight spreads
- Full symbol coverage
- Advanced Trade WebSocket API

### Kraken
- BBO (Best Bid/Offer) event triggers for faster updates
- Competitive spreads
- Full symbol coverage
- V2 WebSocket API

## Future Enhancements

- Execution capabilities (place actual trades)
- Historical arbitrage opportunity logging
- Performance metrics and analytics
- Additional exchange support
- Web-based dashboard
- Alert notifications

## License

This project is for educational and demonstration purposes. Not intended for production trading.

---

**Note**: This application connects to live market data but does not perform any trading operations. It's designed for market analysis and system development purposes.
