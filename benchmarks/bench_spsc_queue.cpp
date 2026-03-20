// bench_spsc_queue.cpp
//
// Google Benchmark harness for SPSCRingBuffer.
// Provides a direct comparison baseline against the MPSC benchmarks.
// Measures:
//   - Push latency   (producer thread only): p50 / p99 / p999 in ns
//   - Pop  latency   (consumer thread only): p50 / p99 / p999 in ns
//   - Throughput     (1 producer → 1 consumer): messages / second
//
// Key expectation:
//   SPSC push/pop should be faster than MPSC because there is no CAS on tail_.
//   The producer owns tail_ exclusively and uses a simple store(release).
//
// Build:
//   cmake .. -DBUILD_BENCHMARKS=ON && make bench_spsc_queue
//   ./bench_spsc_queue --benchmark_repetitions=3 --benchmark_report_aggregates_only=true

#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

#include "ring_buffer.hpp"
#include "timing.hpp"

static constexpr size_t kLatencyQueueSize   = 4096;
static constexpr size_t kThroughputQueueSize = 65536;

// ─── Push Latency ────────────────────────────────────────────────────────────
// Single producer thread, no consumer running concurrently.
// Queue held at ~50% fill to avoid the full/empty edge cases.
static void BM_SPSC_PushLatency(benchmark::State& state) {
    SPSCRingBuffer<uint64_t, kLatencyQueueSize> queue;
    auto& cal = timing::get_calibrator();

    for (size_t i = 0; i < kLatencyQueueSize / 2; ++i)
        queue.try_push(i);

    std::vector<uint64_t> latencies_ns;
    latencies_ns.reserve(1'000'000);

    uint64_t push_val = 0;
    uint64_t dummy    = 0;

    for (auto _ : state) {
        queue.try_pop(dummy);
        benchmark::DoNotOptimize(dummy);

        uint64_t t0 = timing::rdtscp();
        bool ok      = queue.try_push(push_val++);
        uint64_t t1  = timing::rdtscp();
        benchmark::DoNotOptimize(ok);

        latencies_ns.push_back(cal.cycles_to_ns(t1 - t0));
    }

    if (!latencies_ns.empty()) {
        std::sort(latencies_ns.begin(), latencies_ns.end());
        const size_t n = latencies_ns.size();
        state.counters["p50_ns"]  = static_cast<double>(latencies_ns[n * 50  / 100]);
        state.counters["p99_ns"]  = static_cast<double>(latencies_ns[n * 99  / 100]);
        state.counters["p999_ns"] = static_cast<double>(latencies_ns[n * 999 / 1000]);
        state.counters["min_ns"]  = static_cast<double>(latencies_ns.front());
    }
}
BENCHMARK(BM_SPSC_PushLatency)->Iterations(1'000'000);

// ─── Pop Latency ─────────────────────────────────────────────────────────────
// Single consumer thread, no producer running concurrently.
// Queue held at ~50% fill.
static void BM_SPSC_PopLatency(benchmark::State& state) {
    SPSCRingBuffer<uint64_t, kLatencyQueueSize> queue;
    auto& cal = timing::get_calibrator();

    std::vector<uint64_t> latencies_ns;
    latencies_ns.reserve(1'000'000);

    uint64_t push_val = 0;
    uint64_t item     = 0;

    for (auto _ : state) {
        queue.try_push(push_val++);

        uint64_t t0 = timing::rdtscp();
        bool ok      = queue.try_pop(item);
        uint64_t t1  = timing::rdtscp();
        benchmark::DoNotOptimize(ok);
        benchmark::DoNotOptimize(item);

        latencies_ns.push_back(cal.cycles_to_ns(t1 - t0));
    }

    if (!latencies_ns.empty()) {
        std::sort(latencies_ns.begin(), latencies_ns.end());
        const size_t n = latencies_ns.size();
        state.counters["p50_ns"]  = static_cast<double>(latencies_ns[n * 50  / 100]);
        state.counters["p99_ns"]  = static_cast<double>(latencies_ns[n * 99  / 100]);
        state.counters["p999_ns"] = static_cast<double>(latencies_ns[n * 999 / 1000]);
        state.counters["min_ns"]  = static_cast<double>(latencies_ns.front());
    }
}
BENCHMARK(BM_SPSC_PopLatency)->Iterations(1'000'000);

// ─── Throughput (1 producer → 1 consumer pipeline) ───────────────────────────
// One producer thread pushes kTotalMsgs items; the calling thread consumes them.
// Measures end-to-end pipeline throughput: items/second.
static void BM_SPSC_Throughput(benchmark::State& state) {
    constexpr size_t kTotalMsgs = 100'000;
    int64_t total_processed     = 0;

    for (auto _ : state) {
        // ── Setup (excluded from timing) ──────────────────────────────────
        state.PauseTiming();

        SPSCRingBuffer<uint64_t, kThroughputQueueSize> queue;
        std::atomic<bool>   start_flag{false};
        std::atomic<bool>   producer_ready{false};

        std::thread producer([&]() {
            producer_ready.store(true, std::memory_order_release);
            while (!start_flag.load(std::memory_order_acquire))
                std::this_thread::yield();

            for (size_t j = 0; j < kTotalMsgs; ++j) {
                while (!queue.try_push(j))
                    std::this_thread::yield();
            }
        });

        while (!producer_ready.load(std::memory_order_acquire))
            std::this_thread::yield();

        // ── Timed section ─────────────────────────────────────────────────
        state.ResumeTiming();
        start_flag.store(true, std::memory_order_release);

        uint64_t item;
        size_t   consumed = 0;
        while (consumed < kTotalMsgs) {
            if (queue.try_pop(item))
                ++consumed;
        }

        // ── Teardown (excluded from timing) ───────────────────────────────
        state.PauseTiming();
        producer.join();
        total_processed += static_cast<int64_t>(kTotalMsgs);
        state.ResumeTiming();
    }

    state.SetItemsProcessed(total_processed);
}
BENCHMARK(BM_SPSC_Throughput)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(3);
