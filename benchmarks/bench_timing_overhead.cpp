// bench_timing_overhead.cpp
//
// Google Benchmark harness for timing infrastructure overhead.
// Measures the cost of each timestamp primitive so you know exactly
// how much instrumentation budget each rdtsc call consumes.
//
// Benchmarks:
//   BM_RDTSC              — cost of a single rdtsc() call (non-serializing)
//   BM_RDTSCp             — cost of a single rdtscp() call (serializing fence)
//   BM_NowNS              — cost of now_ns(): rdtsc + cycles_to_ns conversion
//   BM_ScopedTimer        — cost of entering + exiting a ScopedTimer RAII guard
//                           (two rdtscp calls + cycles_to_ns)
//   BM_ChronoSteadyClock  — baseline: std::chrono::steady_clock::now()
//                           (syscall-based; shows how much rdtsc saves)
//
// Expected results on a modern x86_64 at 3–4 GHz:
//   rdtsc        ~4–8  ns   (~12–20 cycles)
//   rdtscp       ~8–15 ns   (~25–40 cycles, serializing fence has higher cost)
//   now_ns       ~10–20 ns  (rdtsc + integer multiply + divide)
//   ScopedTimer  ~20–35 ns  (two rdtscp + cycles_to_ns)
//   chrono       ~25–50 ns  (vDSO clock_gettime on Linux, slightly more on macOS)
//
// Build:
//   cmake .. -DBUILD_BENCHMARKS=ON && make bench_timing_overhead
//   ./bench_timing_overhead

#include <benchmark/benchmark.h>

#include <chrono>

#include "timing.hpp"

// ─── rdtsc (non-serializing) ─────────────────────────────────────────────────
// Cheapest timestamp: no fence, the CPU may reorder instructions around it.
// Suitable for wrapping a loop body where a few cycles of reorder slack is fine.
static void BM_RDTSC(benchmark::State& state) {
    for (auto _ : state) {
        uint64_t t = timing::rdtsc();
        benchmark::DoNotOptimize(t);
    }
}
BENCHMARK(BM_RDTSC);

// ─── rdtscp (serializing) ────────────────────────────────────────────────────
// Includes a partial serializing fence (CPUID-equivalent effect on out-of-order
// execution). Used in production timing paths where instruction reordering
// would corrupt the measurement.
static void BM_RDTSCp(benchmark::State& state) {
    for (auto _ : state) {
        uint64_t t = timing::rdtscp();
        benchmark::DoNotOptimize(t);
    }
}
BENCHMARK(BM_RDTSCp);

// ─── now_ns ──────────────────────────────────────────────────────────────────
// timing::now_ns() = rdtsc() + cycles_to_ns()
// cycles_to_ns() is: (cycles * 1_000_000_000) / tsc_frequency  — two integer ops.
static void BM_NowNS(benchmark::State& state) {
    for (auto _ : state) {
        uint64_t t = timing::now_ns();
        benchmark::DoNotOptimize(t);
    }
}
BENCHMARK(BM_NowNS);

// ─── ScopedTimer ─────────────────────────────────────────────────────────────
// Constructor: one rdtsc().
// Destructor:  one rdtsc() + cycles_to_ns() + store to result.
// This is the overhead added to every measurement site that uses ScopedTimer.
static void BM_ScopedTimer(benchmark::State& state) {
    uint64_t result = 0;

    for (auto _ : state) {
        {
            timing::ScopedTimer t(result);
            benchmark::ClobberMemory();  // prevent the body from being optimised away
        }
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_ScopedTimer);

// ─── std::chrono::steady_clock (baseline comparison) ─────────────────────────
// On Linux:  vDSO clock_gettime(CLOCK_MONOTONIC) — no syscall, but slower than rdtsc.
// On macOS:  mach_absolute_time() — similar cost.
// Use this number to quantify how much rdtsc saves vs the standard library clock.
static void BM_ChronoSteadyClock(benchmark::State& state) {
    for (auto _ : state) {
        auto t = std::chrono::steady_clock::now();
        benchmark::DoNotOptimize(t);
    }
}
BENCHMARK(BM_ChronoSteadyClock);
