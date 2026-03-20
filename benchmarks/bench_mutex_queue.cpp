// bench_mutex_queue.cpp
//
// Google Benchmark harness for a bounded mutex-protected queue.
// Mirrors bench_mpsc_queue.cpp exactly so results are directly comparable.
//
// Measures:
//   - Push latency   (single producer, half-full queue): p50 / p99 / p999 in ns
//   - Pop  latency   (single consumer, half-full queue): p50 / p99 / p999 in ns
//   - Throughput     (1 / 2 / 3 / 4 producers → 1 consumer): messages / second
//
// Build:
//   cmake .. -DBUILD_BENCHMARKS=ON && make bench_mutex_queue
//   ./bench_mutex_queue --benchmark_repetitions=3 --benchmark_report_aggregates_only=true

#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "timing.hpp"

// ─── Bounded Mutex Queue ─────────────────────────────────────────────────────
// Wraps std::queue<T> + std::mutex with the same try_push / try_pop interface
// as MPSCRingBuffer so all benchmark bodies are identical to bench_mpsc_queue.
template <typename T, size_t Capacity>
class BoundedMutexQueue {
public:
    bool try_push(T val) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (q_.size() >= Capacity)
            return false;
        q_.push(std::move(val));
        return true;
    }

    bool try_pop(T& out) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (q_.empty())
            return false;
        out = std::move(q_.front());
        q_.pop();
        return true;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return q_.size();
    }

private:
    mutable std::mutex mtx_;
    std::queue<T>      q_;
};

// Same sizes as bench_mpsc_queue for a fair comparison.
static constexpr size_t kLatencyQueueSize    = 4096;
static constexpr size_t kThroughputQueueSize = 65536;

// ─── Push Latency ────────────────────────────────────────────────────────────
// Single producer, no concurrent consumer.
// Queue kept at ~50% fill so neither full nor empty fast-paths dominate.
static void BM_Mutex_PushLatency(benchmark::State& state) {
    BoundedMutexQueue<uint64_t, kLatencyQueueSize> queue;
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
BENCHMARK(BM_Mutex_PushLatency)->Iterations(1'000'000);

// ─── Pop Latency ─────────────────────────────────────────────────────────────
// Single consumer, no concurrent producer.
// Queue kept at ~50% fill.
static void BM_Mutex_PopLatency(benchmark::State& state) {
    BoundedMutexQueue<uint64_t, kLatencyQueueSize> queue;
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
BENCHMARK(BM_Mutex_PopLatency)->Iterations(1'000'000);

// ─── Throughput (N producers → 1 consumer) ───────────────────────────────────
// Identical structure to BM_MPSC_Throughput — only the queue type differs.
static void BM_Mutex_Throughput(benchmark::State& state) {
    const size_t num_producers  = static_cast<size_t>(state.range(0));
    constexpr size_t kTotalMsgs = 100'000;
    const size_t per_producer   = kTotalMsgs / num_producers;
    const size_t actual_total   = per_producer * num_producers;

    int64_t total_processed = 0;

    for (auto _ : state) {
        state.PauseTiming();

        BoundedMutexQueue<uint64_t, kThroughputQueueSize> queue;
        std::atomic<bool>   start_flag{false};
        std::atomic<size_t> ready{0};
        std::vector<std::thread> threads;
        threads.reserve(num_producers);

        for (size_t i = 0; i < num_producers; ++i) {
            threads.emplace_back([&]() {
                ready.fetch_add(1, std::memory_order_release);
                while (!start_flag.load(std::memory_order_acquire))
                    std::this_thread::yield();

                for (size_t j = 0; j < per_producer; ++j) {
                    while (!queue.try_push(j))
                        std::this_thread::yield();
                }
            });
        }

        while (ready.load(std::memory_order_acquire) < num_producers)
            std::this_thread::yield();

        state.ResumeTiming();
        start_flag.store(true, std::memory_order_release);

        uint64_t item;
        size_t   consumed = 0;
        while (consumed < actual_total) {
            if (queue.try_pop(item))
                ++consumed;
        }

        state.PauseTiming();
        for (auto& t : threads) t.join();
        total_processed += static_cast<int64_t>(actual_total);
        state.ResumeTiming();
    }

    state.SetItemsProcessed(total_processed);
}
BENCHMARK(BM_Mutex_Throughput)
    ->Arg(1)->Arg(2)->Arg(3)->Arg(4)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(3);
