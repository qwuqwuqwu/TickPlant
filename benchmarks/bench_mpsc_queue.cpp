// bench_mpsc_queue.cpp
//
// Google Benchmark harness for MPSCRingBuffer.
// Measures:
//   - Push latency   (single producer, half-full queue): p50 / p99 / p999 in ns
//   - Pop  latency   (single consumer, half-full queue): p50 / p99 / p999 in ns
//   - Throughput     (1 / 2 / 3 / 4 producers → 1 consumer): messages / second
//
// Build:
//   cmake .. -DBUILD_BENCHMARKS=ON && make bench_mpsc_queue
//   ./bench_mpsc_queue --benchmark_repetitions=3 --benchmark_report_aggregates_only=true

#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

#include "mpsc_ring_buffer.hpp"
#include "timing.hpp"

// Capacity used for latency benchmarks — fits in L2 cache on most CPUs.
static constexpr size_t kLatencyQueueSize = 4096;

// Larger capacity for throughput benchmarks to avoid artificial back-pressure.
static constexpr size_t kThroughputQueueSize = 65536;

// ─── Push Latency ────────────────────────────────────────────────────────────
// Single producer, no concurrent consumer.
// Queue is kept at ~50% fill so neither the full nor empty fast-paths dominate.
// Reports p50 / p99 / p999 / min in nanoseconds via rdtscp.
static void BM_MPSC_PushLatency(benchmark::State& state) {
    MPSCRingBuffer<uint64_t, kLatencyQueueSize> queue;
    auto& cal = timing::get_calibrator();  // triggers 100ms calibration once

    // Pre-fill to half capacity so the first iteration hits a warm, half-full queue.
    for (size_t i = 0; i < kLatencyQueueSize / 2; ++i)
        queue.try_push(i);

    std::vector<uint64_t> latencies_ns;
    latencies_ns.reserve(1'000'000);

    uint64_t push_val = 0;
    uint64_t dummy    = 0;

    for (auto _ : state) {
        // Pop one slot before pushing one to maintain ~50% occupancy.
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
BENCHMARK(BM_MPSC_PushLatency)->Iterations(1'000'000);

// ─── Pop Latency ─────────────────────────────────────────────────────────────
// Single consumer, no concurrent producer.
// Queue is kept at ~50% fill.
static void BM_MPSC_PopLatency(benchmark::State& state) {
    MPSCRingBuffer<uint64_t, kLatencyQueueSize> queue;
    auto& cal = timing::get_calibrator();

    std::vector<uint64_t> latencies_ns;
    latencies_ns.reserve(1'000'000);

    uint64_t push_val = 0;
    uint64_t item     = 0;

    for (auto _ : state) {
        // Push one slot before popping one to maintain ~50% occupancy.
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
BENCHMARK(BM_MPSC_PopLatency)->Iterations(1'000'000);

// ─── Throughput (N producers → 1 consumer) ───────────────────────────────────
// Spawns N producer threads that each push (kTotalMsgs / N) items.
// The calling thread acts as the single consumer.
// Measures wall-clock throughput: items/second.
//
// Parameterised via Arg(N):
//   Arg(1) → 1 producer  (baseline, no CAS contention)
//   Arg(2) → 2 producers
//   Arg(3) → 3 producers
//   Arg(4) → 4 producers (matches live system: Binance/Bybit/Coinbase/Kraken)
static void BM_MPSC_Throughput(benchmark::State& state) {
    const size_t num_producers  = static_cast<size_t>(state.range(0));
    constexpr size_t kTotalMsgs = 100'000;
    const size_t per_producer   = kTotalMsgs / num_producers;
    // Use actual total to avoid integer division truncation causing consumer to
    // spin forever waiting for messages that were never pushed (e.g. 3 producers:
    // 100'000/3=33'333, 33'333×3=99'999 ≠ 100'000).
    const size_t actual_total   = per_producer * num_producers;

    int64_t total_processed = 0;

    for (auto _ : state) {
        // ── Setup (excluded from timing) ──────────────────────────────────
        state.PauseTiming();

        MPSCRingBuffer<uint64_t, kThroughputQueueSize> queue;
        std::atomic<bool>   start_flag{false};
        std::atomic<size_t> ready{0};
        std::vector<std::thread> threads;
        threads.reserve(num_producers);

        for (size_t i = 0; i < num_producers; ++i) {
            threads.emplace_back([&]() {
                // Signal readiness, then spin until the consumer fires start_flag.
                ready.fetch_add(1, std::memory_order_release);
                while (!start_flag.load(std::memory_order_acquire))
                    std::this_thread::yield();

                for (size_t j = 0; j < per_producer; ++j) {
                    while (!queue.try_push(j))
                        std::this_thread::yield();
                }
            });
        }

        // Wait until every producer thread is spin-waiting at the start gate.
        while (ready.load(std::memory_order_acquire) < num_producers)
            std::this_thread::yield();

        // ── Timed section ─────────────────────────────────────────────────
        state.ResumeTiming();
        start_flag.store(true, std::memory_order_release);

        uint64_t item;
        size_t   consumed = 0;
        while (consumed < actual_total) {
            if (queue.try_pop(item))
                ++consumed;
        }

        // ── Teardown (excluded from timing) ───────────────────────────────
        state.PauseTiming();
        for (auto& t : threads) t.join();
        total_processed += static_cast<int64_t>(actual_total);
        state.ResumeTiming();
    }

    state.SetItemsProcessed(total_processed);
}
BENCHMARK(BM_MPSC_Throughput)
    ->Arg(1)->Arg(2)->Arg(3)->Arg(4)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(3);
