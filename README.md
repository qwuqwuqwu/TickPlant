# TickPlant — Low-Latency Market Data Platform

A high-performance market data platform written in C++20.  
Aggregates real-time L2 order books from five exchanges, persists BBO ticks
to QuestDB, and exposes a config-driven reporting pipeline over a custom TCP
protocol — deployable across two machines with two cooperating processes on
the client side.

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Run Modes](#run-modes)
3. [Wire Protocol](#wire-protocol)
4. [Reporting Pipeline](#reporting-pipeline)
5. [Build](#build)
6. [Running the Distributed Setup](#running-the-distributed-setup)
7. [Benchmark Results](#benchmark-results)
8. [Component Reference](#component-reference)

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                  Linux Server (192.168.88.9)             │
│                                                          │
│  Binance ──┐                                             │
│  Coinbase ─┤                                             │
│  Bybit ────┼──▶ ArbitrageEngine ──▶ TickLogger ──▶ QuestDB :9000
│  Kraken ───┤         │                                   │
│  FIX sim ──┘         ▼                                   │
│                 OrderBook[]                              │
│                      │                                   │
│                      ▼                                   │
│             QueryServer (epoll / io_uring)  :9092        │
└─────────────────────────────────────────────────────────┘
                        ▲  SNAPSHOT / HEALTH / REPORT
                        │
┌─────────────────────────────────────────────────────────┐
│                  Mac Client                              │
│                                                          │
│  Process 1: fix-server                                   │
│    FIX simulator ──▶ ArbitrageEngine                     │
│    SimpleQueryServer  :9093                              │
│                                                          │
│  Process 2: report-client  ◀── user REPL                 │
│    RemoteDataSource  ──▶ Linux  :9092                    │
│    RemoteDataSource  ──▶ Mac-FIX :9093                   │
│    QuestDbDataSource ──▶ QuestDB :9000                   │
│    ReportPipeline    ──▶ JSON report                     │
└─────────────────────────────────────────────────────────┘
```

**Data flow summary:**

| Layer | What happens |
|---|---|
| Ingestion | WebSocket clients push `OrderBookSnapshot` deltas into `ArbitrageEngine` |
| Storage | `TickLogger` writes BBO ticks to QuestDB via ILP over TCP (nanosecond timestamps) |
| Query | `QueryServer` serves `SNAPSHOT` / `HEALTH` / `REPORT` over a newline-delimited TCP protocol |
| Reporting | `ReportPipeline` fans out to all registered `DataSource`s concurrently, resolves staleness, and generates a structured JSON report |

---

## Run Modes

The binary is started with `--mode <mode>`. Three modes are supported:

### `--mode full` (Linux)

Runs everything on one machine: all five exchange feeds, QuestDB persistence,
and the full query server.

```bash
./binance_dashboard \
  --mode full \
  --query-server epoll \   # or: uring
  --query-port 9092
```

### `--mode fix-server` (Mac — Process 1)

Runs the FIX 4.4 simulator and a portable `select()`-based query server.
No WebSocket clients — this process is the local FIX data node.

```bash
./binance_dashboard \
  --mode fix-server \
  --fix-port 9093
```

### `--mode report-client` (Mac — Process 2)

Interactive REPL that aggregates data from remote nodes and QuestDB.
Accepts `--add-remote` (repeatable) and `--questdb-host/port`.

```bash
./binance_dashboard \
  --mode report-client \
  --add-remote "Mac-FIX:127.0.0.1:9093" \
  --add-remote "Linux:192.168.88.9:9092" \
  --questdb-host 192.168.88.9 \
  --questdb-port 9000 \
  --pretty
```

`--pretty` pretty-prints JSON output (uses `jq`-style 2-space indent).

### CLI flags summary

| Flag | Modes | Description |
|---|---|---|
| `--mode <full\|fix-server\|report-client>` | all | Selects run mode |
| `--query-server <epoll\|uring>` | full | I/O backend for query server |
| `--query-port <port>` | full | Query server listen port (default 9092) |
| `--fix-port <port>` | fix-server | SimpleQueryServer listen port (default 9093) |
| `--add-remote "label:host:port"` | report-client | Register a remote data source (repeatable) |
| `--questdb-host <host>` | report-client | QuestDB host (default 127.0.0.1) |
| `--questdb-port <port>` | report-client | QuestDB REST port (default 9000) |
| `--pretty` | report-client | Pretty-print JSON output |

---

## Wire Protocol

All servers (epoll, io_uring, SimpleQueryServer) speak the same
newline-delimited text protocol over TCP.

### Commands

```
SNAPSHOT <SYM>      — L2 order book snapshot for one symbol
HEALTH              — feed staleness for all connected exchanges
REPORT <name>       — run a named report from reports.json
LISTREPORTS         — list all configured report names
```

### Example session

```
$ nc 192.168.88.9 9092

HEALTH
{"status":"ok","feeds":{"Binance":{"staleness_ms":18,"live":true},"Kraken":{"staleness_ms":34,"live":true},...}}

SNAPSHOT BTC
{"status":"ok","symbol":"BTC","books":[{"exchange":"Binance","bids":[[80801.49,0.42],...],"asks":[[80813.41,0.18],...]},...]}

LISTREPORTS
{"status":"ok","reports":["bbo_summary","cross_venue","fix_vs_ws","remote_vs_local"]}

REPORT remote_vs_local
{"status":"ok","report":"remote_vs_local","resolution":{...},"symbols":{...}}
```

---

## Reporting Pipeline

Reports are defined in `reports.json` (copied to the build directory at
cmake time). Each report names the symbols and data sources it needs, and
marks each source as required or optional.

```json
{
  "reports": [
    {
      "name": "remote_vs_local",
      "symbols": ["BTC", "ETH", "SOL"],
      "sources": [
        {"name": "Linux",   "required": true,  "max_staleness_ms": 5000},
        {"name": "Mac-FIX", "required": true,  "max_staleness_ms": 5000},
        {"name": "QuestDB", "required": false, "max_staleness_ms": 60000}
      ]
    }
  ]
}
```

### Three-phase execution

```
Resolution phase  ──  concurrent std::async per source
  └── checks each source: HEALTH / QuestDB probe / staleness threshold
  └── result: OK | STALE | UNREACHABLE

Fetch phase  ──  concurrent std::async for all OK sources
  └── SNAPSHOT per symbol, or QuestDB 1-hour aggregate query
  └── required sources: abort report if UNREACHABLE
  └── optional sources: included when available, skipped silently otherwise

Generation phase  ──  single thread
  └── merges live BBO + historical stats per symbol into JSON
```

### Report output structure

```json
{
  "status": "ok",
  "report": "remote_vs_local",
  "generated_at_ms": 1778596023317,
  "resolution": {
    "Linux":   {"status": "ok"},
    "Mac-FIX": {"status": "ok"},
    "QuestDB": {"status": "ok"}
  },
  "symbols": {
    "BTC": {
      "live": {
        "Binance": {"bid": 80801.49, "ask": 80813.41, "spread_bps": 1.47, "timestamp_ms": ...},
        "Kraken":  {"bid": 80742.60, "ask": 80742.70, "spread_bps": 0.01, "timestamp_ms": ...}
      },
      "historical": {
        "avg_spread_bps": 0.087,
        "min_bid": 80742.6,
        "max_ask": 80820.1,
        "sample_count": 1562
      }
    }
  }
}
```

### DataSource abstraction

Every data source implements a three-method interface:

```cpp
class DataSource {
public:
    virtual std::string      name()    const = 0;
    virtual ResolutionResult resolve(symbols, max_staleness_ms) = 0;
    virtual SourceData       fetch(symbols) = 0;
};
```

Built-in implementations:

| Class | Backed by | Protocol |
|---|---|---|
| `ExchangeDataSource` | Local `ArbitrageEngine` | In-process |
| `RemoteDataSource` | Remote TickPlant node | TCP `SNAPSHOT`/`HEALTH` |
| `QuestDbDataSource` | QuestDB REST API | HTTP + chunked transfer-encoding decode |

New sources register themselves in `DataSourceRegistry` at startup — zero
changes to pipeline logic required.

---

## Build

### Dependencies

| Library | Purpose |
|---|---|
| Boost (system, thread) | Boost.Asio WebSocket TLS layer |
| OpenSSL | TLS for WebSocket connections |
| nlohmann/json | JSON parsing throughout |
| prometheus-cpp | Prometheus pull-model metrics |
| liburing *(Linux, optional)* | io_uring query server backend |

### macOS (vcpkg)

```bash
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg && ./bootstrap-vcpkg.sh
export VCPKG_ROOT=$(pwd)

vcpkg install nlohmann-json openssl boost-system boost-thread prometheus-cpp

cd /path/to/TickPlant
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
make -j$(sysctl -n hw.logicalcpu)
```

### Linux (apt + vcpkg)

```bash
sudo apt install liburing-dev   # enables io_uring backend

mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
         -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Optional build flags

```bash
# Choose queue implementation (default: MUTEX)
cmake .. -DQUEUE_TYPE=SPSC    # or MPSC

# Build Google Benchmark harnesses
cmake .. -DBUILD_BENCHMARKS=ON

# Build OrderBook ITCH replay test
cmake .. -DBUILD_TESTS=ON
```

---

## Running the Distributed Setup

### Step 1 — Linux: start QuestDB

```bash
cd ~/questdb && ./bin/questdb.sh start
# REST API: http://192.168.88.9:9000
# ILP:      192.168.88.9:9009
```

### Step 2 — Linux: start the full node

```bash
./binance_dashboard \
  --mode full \
  --query-server epoll \
  --query-port 9092
```

### Step 3 — Mac: start the FIX server (Process 1)

```bash
./binance_dashboard \
  --mode fix-server \
  --fix-port 9093
```

### Step 4 — Mac: start the report client (Process 2)

```bash
./binance_dashboard \
  --mode report-client \
  --add-remote "Mac-FIX:127.0.0.1:9093" \
  --add-remote "Linux:192.168.88.9:9092" \
  --questdb-host 192.168.88.9 \
  --pretty
```

### Step 5 — Query from the report client REPL

```
> LISTREPORTS
> REPORT remote_vs_local
> REPORT bbo_summary
> quit
```

---

## Benchmark Results

**Hardware**: Intel Core i5-2410M (Sandy Bridge, 4 logical cores, 2.3 GHz)  
**OS**: Ubuntu 22.04, kernel 6.8, `isolcpus=2,3 nohz_full=2,3`  
**Method**: server pinned to CPU 3, client pinned to CPU 2; 10,000 measured
requests, 500 warmup; loopback interface

```bash
./bench_query_client --sym BTC --n 10000 --warmup 500
```

| Metric | epoll | io_uring |
|---|---:|---:|
| p50 RTT | ~1.6 ms | ~1.6 ms |
| p99 RTT | ~2.0 ms | ~1.7–8.0 ms |
| p999 RTT | ~2–8 ms | ~2–14 ms |
| Syscalls / request | ~3 | ~1 |

Tail latency is dominated by L3 cache pressure from the four concurrent
WebSocket ingestion threads sharing the same 3 MB L3 cache. On isolated
production hardware, io_uring's reduced syscall count translates to lower
tail latency at high connection counts.

### End-to-end latency (WebSocket → query response)

| Percentile | Latency |
|---|---|
| p50 | 1.71 µs |
| p99 | 37.5 µs |
| p999 | 127 µs |

---

## Component Reference

| Component | File(s) | Description |
|---|---|---|
| `BinanceClient` | `binance_client.*` | WebSocket depth20 @ 100 ms |
| `CoinbaseClient` | `coinbase_client.*` | WebSocket level2 full book |
| `KrakenClient` | `kraken_client.*` | WebSocket v2 BBO event-triggered |
| `BybitClient` | `bybit_client.*` | WebSocket orderbook.50 |
| `FIXFeedSimulator` | `fix_feed_simulator.*` | Synthetic burst FIX 4.4 TCP feed |
| `FIXParser` | `fix_parser.*` | Hand-rolled FIX 4.4 tag parser |
| `OrderBook` | `order_book.*` | Thread-safe L2 book (`shared_mutex`); `prune_crossed()` removes transiently crossed bid levels |
| `ArbitrageEngine` | `arbitrage_engine.*` | Cross-exchange BBO comparison, delta application |
| `TickLogger` | `tick_logger.*` | QuestDB ILP writer over TCP (nanosecond timestamps) |
| `QueryServerEpoll` | `query_server_epoll.*` | Edge-triggered epoll query server (Linux) |
| `QueryServerUring` | `query_server_uring.*` | io_uring query server via liburing (Linux) |
| `SimpleQueryServer` | `simple_query_server.*` | `select()`-based portable query server (macOS) |
| `DataSourceRegistry` | `data_source_registry.*` | Owns and looks up `DataSource` instances by name |
| `ExchangeDataSource` | `exchange_data_source.*` | Wraps `ArbitrageEngine` as a `DataSource` |
| `RemoteDataSource` | `remote_data_source.*` | TCP client to a remote TickPlant node |
| `QuestDbDataSource` | `questdb_data_source.*` | HTTP client to QuestDB REST API; decodes chunked transfer-encoding |
| `ReportPipeline` | `report_pipeline.*` | Three-phase concurrent pipeline: resolve → fetch → generate |
| `Metrics` | `metrics.*` | Prometheus pull-model metrics (port 9090) |
| `Dashboard` | `dashboard.*` | Terminal UI with color-coded live display |

---

*This project is for educational and demonstration purposes and does not perform any trading operations.*
