// bench_fix_parser.cpp
//
// Google Benchmark harness for FIXParser.
// Measures:
//   - FIX parse latency (35=W, 6 entries):   p50 / p99 / p999 in ns
//   - FIX parse latency (35=X, 3 entries):   p50 / p99 / p999 in ns
//   - FIX parse latency (35=D):              p50 / p99 / p999 in ns
//   - FIX parse + to_ticker_data():          p50 / p99 / p999 in ns
//   - JSON parse equivalent (nlohmann):      p50 / p99 / p999 in ns  ← baseline
//   - FIX parser throughput:                 messages / second
//
// Key result to look for:
//   BM_FIXParse_Snapshot  p50 should be < 500ns (ROADMAP target)
//   BM_JSONParse_Snapshot p50 should be ~4-10µs  (nlohmann baseline)
//
// Build (Release required for meaningful numbers):
//   cmake .. -DBUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
//   make bench_fix_parser
//   ./bench_fix_parser --benchmark_repetitions=3

#include <benchmark/benchmark.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "fix_parser.hpp"
#include "timing.hpp"

using json = nlohmann::json;

// ─── Test messages ────────────────────────────────────────────────────────────
//
// Hard-coded with correct SOH (0x01) delimiters so the parser sees a
// realistic wire-format message without paying build_snapshot() overhead.

// 35=W  MarketDataSnapshot — 3 bid levels + 3 ask levels
static const std::string kFixSnapshot =
    "8=FIX.4.4\x01" "9=249\x01"   "35=W\x01"
    "34=42\x01"      "49=BLOOMBERG\x01" "56=CLIENT\x01"
    "52=20240315-10:30:00.123\x01"
    "262=REQ001\x01" "55=BTCUSD\x01"   "268=6\x01"
    "269=0\x01"      "270=101.250\x01" "271=500\x01"
    "269=0\x01"      "270=101.000\x01" "271=1200\x01"
    "269=0\x01"      "270=100.750\x01" "271=800\x01"
    "269=1\x01"      "270=101.500\x01" "271=300\x01"
    "269=1\x01"      "270=101.750\x01" "271=600\x01"
    "269=1\x01"      "270=102.000\x01" "271=400\x01"
    "10=128\x01";

// 35=X  MarketDataIncremental — 3 delta updates
static const std::string kFixIncremental =
    "8=FIX.4.4\x01" "9=161\x01"   "35=X\x01"
    "34=43\x01"      "49=BLOOMBERG\x01" "56=CLIENT\x01"
    "52=20240315-10:30:00.456\x01"
    "262=REQ001\x01" "55=BTCUSD\x01"   "268=3\x01"
    "279=0\x01"      "269=0\x01"        "270=101.300\x01" "271=750\x01"
    "279=1\x01"      "269=0\x01"        "270=101.000\x01" "271=0\x01"
    "279=0\x01"      "269=1\x01"        "270=101.450\x01" "271=200\x01"
    "10=087\x01";

// 35=D  NewOrderSingle
static const std::string kFixNewOrder =
    "8=FIX.4.4\x01" "9=148\x01"   "35=D\x01"
    "34=44\x01"      "49=CLIENT\x01"   "56=BLOOMBERG\x01"
    "52=20240315-10:30:01.000\x01"
    "11=ORD001\x01"  "55=BTCUSD\x01"  "54=1\x01"
    "38=1000000\x01" "40=2\x01"       "44=101.250\x01"
    "59=0\x01"       "10=201\x01";

// JSON equivalent of the 35=W snapshot — used as the parsing baseline
static const std::string kJsonSnapshot = R"({
    "msg_type":"W","seq_num":42,
    "sender":"BLOOMBERG","target":"CLIENT",
    "send_time":"20240315-10:30:00.123",
    "symbol":"BTCUSD","req_id":"REQ001",
    "entries":[
        {"type":0,"price":101.250,"qty":500},
        {"type":0,"price":101.000,"qty":1200},
        {"type":0,"price":100.750,"qty":800},
        {"type":1,"price":101.500,"qty":300},
        {"type":1,"price":101.750,"qty":600},
        {"type":1,"price":102.000,"qty":400}
    ]
})";

// ─── Latency helper ───────────────────────────────────────────────────────────
// Shared percentile reporting used by every latency benchmark.

static void report_percentiles(benchmark::State&            state,
                                std::vector<uint64_t>&      lats) {
    std::sort(lats.begin(), lats.end());
    const size_t n = lats.size();
    if (n == 0) return;
    state.counters["min_ns"]  = static_cast<double>(lats.front());
    state.counters["p50_ns"]  = static_cast<double>(lats[n * 50  / 100]);
    state.counters["p99_ns"]  = static_cast<double>(lats[n * 99  / 100]);
    state.counters["p999_ns"] = static_cast<double>(lats[n * 999 / 1000]);
}

// ─── BM_FIXParse_Snapshot ────────────────────────────────────────────────────
// Baseline: parse a 35=W with 6 price levels.
// This is the most common message type in a market data pipeline.

static void BM_FIXParse_Snapshot(benchmark::State& state) {
    FIXParser  parser;
    FIXMessage msg;
    auto& cal = timing::get_calibrator();

    std::vector<uint64_t> lats;
    lats.reserve(1'000'000);

    for (auto _ : state) {
        const uint64_t t0 = timing::rdtscp();
        bool ok           = parser.parse(kFixSnapshot, msg);
        const uint64_t t1 = timing::rdtscp();
        benchmark::DoNotOptimize(ok);
        benchmark::DoNotOptimize(msg);
        lats.push_back(cal.cycles_to_ns(t1 - t0));
    }

    report_percentiles(state, lats);
}
BENCHMARK(BM_FIXParse_Snapshot)->Iterations(1'000'000);

// ─── BM_FIXParse_Incremental ─────────────────────────────────────────────────
// 35=X with 3 delta updates — the most frequent message type on a live feed.

static void BM_FIXParse_Incremental(benchmark::State& state) {
    FIXParser  parser;
    FIXMessage msg;
    auto& cal = timing::get_calibrator();

    std::vector<uint64_t> lats;
    lats.reserve(1'000'000);

    for (auto _ : state) {
        const uint64_t t0 = timing::rdtscp();
        bool ok           = parser.parse(kFixIncremental, msg);
        const uint64_t t1 = timing::rdtscp();
        benchmark::DoNotOptimize(ok);
        benchmark::DoNotOptimize(msg);
        lats.push_back(cal.cycles_to_ns(t1 - t0));
    }

    report_percentiles(state, lats);
}
BENCHMARK(BM_FIXParse_Incremental)->Iterations(1'000'000);

// ─── BM_FIXParse_NewOrder ────────────────────────────────────────────────────
// 35=D — relevant for the Fixed Income Trading role (order submission path).

static void BM_FIXParse_NewOrder(benchmark::State& state) {
    FIXParser  parser;
    FIXMessage msg;
    auto& cal = timing::get_calibrator();

    std::vector<uint64_t> lats;
    lats.reserve(1'000'000);

    for (auto _ : state) {
        const uint64_t t0 = timing::rdtscp();
        bool ok           = parser.parse(kFixNewOrder, msg);
        const uint64_t t1 = timing::rdtscp();
        benchmark::DoNotOptimize(ok);
        benchmark::DoNotOptimize(msg);
        lats.push_back(cal.cycles_to_ns(t1 - t0));
    }

    report_percentiles(state, lats);
}
BENCHMARK(BM_FIXParse_NewOrder)->Iterations(1'000'000);

// ─── BM_FIXParse_ToTickerData ────────────────────────────────────────────────
// Full pipeline: parse 35=W then extract BBO into a TickerData struct.
// This is what the arbitrage engine sees end-to-end from a FIX feed.
//
// Note: to_ticker_data() calls chrono::now() to set timestamp_ms.
// That cost (~20-50ns) is intentionally included — it's real pipeline cost.

static void BM_FIXParse_ToTickerData(benchmark::State& state) {
    FIXParser  parser;
    FIXMessage msg;
    auto& cal = timing::get_calibrator();

    std::vector<uint64_t> lats;
    lats.reserve(1'000'000);

    for (auto _ : state) {
        const uint64_t t0 = timing::rdtscp();
        parser.parse(kFixSnapshot, msg);
        auto ticker       = msg.to_ticker_data();
        const uint64_t t1 = timing::rdtscp();
        benchmark::DoNotOptimize(ticker);
        lats.push_back(cal.cycles_to_ns(t1 - t0));
    }

    report_percentiles(state, lats);
}
BENCHMARK(BM_FIXParse_ToTickerData)->Iterations(1'000'000);

// ─── BM_JSONParse_Snapshot ───────────────────────────────────────────────────
// Comparison baseline: parse an equivalent JSON payload with nlohmann/json.
// Demonstrates why FIX is used instead of JSON on the hot path.

static void BM_JSONParse_Snapshot(benchmark::State& state) {
    auto& cal = timing::get_calibrator();

    std::vector<uint64_t> lats;
    lats.reserve(1'000'000);

    for (auto _ : state) {
        const uint64_t t0 = timing::rdtscp();
        auto doc          = json::parse(kJsonSnapshot);
        const uint64_t t1 = timing::rdtscp();
        benchmark::DoNotOptimize(doc);
        lats.push_back(cal.cycles_to_ns(t1 - t0));
    }

    report_percentiles(state, lats);
}
BENCHMARK(BM_JSONParse_Snapshot)->Iterations(1'000'000);

// ─── BM_FIXParse_Throughput ──────────────────────────────────────────────────
// Maximum sustained throughput: messages / second.
// Google Benchmark measures the loop natively — no manual rdtscp needed.

static void BM_FIXParse_Throughput(benchmark::State& state) {
    FIXParser  parser;
    FIXMessage msg;
    int64_t    total = 0;

    for (auto _ : state) {
        parser.parse(kFixSnapshot, msg);
        benchmark::DoNotOptimize(msg);
        ++total;
    }

    state.SetItemsProcessed(total);
}
BENCHMARK(BM_FIXParse_Throughput)->Unit(benchmark::kNanosecond);
