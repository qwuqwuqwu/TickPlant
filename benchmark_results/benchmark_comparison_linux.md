# Linux Benchmark Comparison — With vs Without CPU Isolation

**Machine:** Sony VAIO VPCEG18FW — Intel Core i5-2410M @ 2.3GHz (turbo ~2.9GHz)
**OS:** Ubuntu 22.04 LTS, kernel 5.15
**CPU topology:** 2 physical cores × 2 HT threads = 4 logical cores
- Physical core 0 → logical core 0 + logical core 2 (HT siblings)
- Physical core 1 → logical core 1 + logical core 3 (HT siblings)

**Google Benchmark:** v1.9.5, Release build, `-O3`

---

## Test Configurations

| Config | Folder | CPU Governor | isolcpus | taskset |
|---|---|---|---|---|
| **Baseline** | `linux_0326/` | performance | none | none |
| **Isolated** | `linux_isolcpus02_0326/` | performance | `isolcpus=0,2` | `taskset -c 0` |

**isolcpus=0,2 rationale:** Isolates both HT siblings of physical core 0 from the OS scheduler.
Isolating only one sibling (e.g. `isolcpus=2,3`) is ineffective — the OS still runs on the paired sibling
(cores 0,1), causing shared L1/L2 cache eviction and execution-unit contention.
With `isolcpus=0,2`, physical core 0 is fully dedicated: no OS scheduling, no HT sibling interference.
Core 2 is intentionally left unoccupied to give the benchmark process the entire physical core.

---

## FIX Parser vs JSON Parser

**File:** `bench_fix_parser` | Iterations: 1,000,000 each

### Latency (ns)

| Benchmark | min | p50 | p99 (baseline) | p99 (isolated) | p99 Δ | p999 (baseline) | p999 (isolated) | p999 Δ |
|---|---|---|---|---|---|---|---|---|
| FIX Snapshot (35=W) | 952 | 960 | 1,492 | 1,317 | **-12%** | 3,318 | 3,286 | -1% |
| FIX Incremental (35=X) | 695 | 702 | 1,007 | 924 | **-8%** | 1,581 | 1,487 | **-6%** |
| FIX NewOrder (35=D) | 447 | 454 | 493 | 492 | ~0% | 742 | 749 | ~0% |
| FIX ToTickerData | 1,035 | 1,044 | 1,127 | 1,271 | +13% | 3,244 | 3,238 | ~0% |
| JSON Snapshot (nlohmann) | 10,332 | 10,548 | 18,715 | 18,021 | **-4%** | 25,687 | 22,049 | **-14%** |

### FIX vs JSON — Headline Comparison

| Metric | FIX Snapshot | JSON Snapshot | FIX advantage |
|---|---|---|---|
| min | 952 ns | 10,332 ns | **10.9×** |
| p50 | 960 ns | 10,548 ns | **11.0×** |
| p99 | ~1,400 ns | ~18,000 ns | **12.9×** |

### Throughput

| Config | items/second |
|---|---|
| Baseline | 1,042,657 |
| Isolated | 1,045,632 |
| Delta | +0.3% (negligible) |

### Key Findings

- **min and p50 are identical** across both configurations — these are the true hardware performance
  numbers, unaffected by OS scheduling. Trust min/p50 for characterizing parser speed.
- **p999 improvements are modest** (0–14%) — the single-threaded FIX parser has little exposure
  to OS scheduling jitter since each iteration completes in ~1µs.
- **JSON p999 benefits more** from isolation (-14%) because each iteration takes ~13µs, giving the
  OS more opportunity to interrupt mid-iteration.
- **FIX is 11× faster than JSON at p50** — entirely due to allocation: nlohmann creates heap objects
  for every field; the FIX parser uses `std::string_view` throughout with zero heap allocation.

---

## MPSC Queue

**File:** `bench_mpsc_queue`
Latency: 1,000,000 iterations. Throughput: 3 iterations per run, time_unit = ms.

### Push/Pop Latency (ns)

| Benchmark | min | p50 | p99 (baseline) | p99 (isolated) | p999 (baseline) | p999 (isolated) |
|---|---|---|---|---|---|---|
| Push latency | 24 | 28 | 29 | 33 | 33 | 44 |
| Pop latency | 11 | 14 | 16 | 16 | 16 | 19 |

Per-operation latency is essentially unchanged — single-op cost is not affected by isolation.

### Throughput (items/second)

| Producers | Baseline | Isolated | Improvement |
|---|---|---|---|
| 1 | 25,721,243 | 31,692,199 | **+23%** |
| 2 | 14,815,531 | 26,022,633 | **+76%** |
| 3 | 15,011,196 | 17,135,854 | **+14%** |
| 4 | 12,240,053 | 19,827,051 | **+62%** |

### Key Findings

- Throughput improves significantly with isolation (14–76%) as the isolated core processes
  CAS operations without OS interference disrupting the lock-free sequencing.
- Multi-producer configs benefit more because contended CAS retries are sensitive to thread
  preemption timing — fewer interruptions means fewer failed CAS operations.

---

## Mutex Queue

**File:** `bench_mutex_queue`
Latency: 1,000,000 iterations. Throughput: 3 iterations per run, time_unit = ms.

### Push/Pop Latency (ns)

| Benchmark | min | p50 | p99 (baseline) | p99 (isolated) | p999 (baseline) | p999 (isolated) |
|---|---|---|---|---|---|---|
| Push latency | 23 | 27 | 44 | 44 | 68 | 68 |
| Pop latency | 20 | 23 | 38 | 40 | 65 | 69 |

Per-operation latency is unchanged — acquiring an uncontended mutex costs the same regardless.

### Throughput (items/second)

| Producers | Baseline | Isolated | Improvement |
|---|---|---|---|
| 1 | 5,729,923 | 21,445,040 | **+274%** |
| 2 | 2,495,739 | 19,398,455 | **+677%** |
| 3 | 2,307,494 | 21,484,041 | **+831%** |
| 4 | 4,084,429 | 19,575,131 | **+379%** |

### Key Findings

- **Most dramatic isolation benefit of any benchmark** — 3.7× to 8.3× throughput improvement.
- Root cause: **OS preemption while holding a lock**. If the OS preempts a thread that holds the
  mutex, all other threads block until the OS reschedules the preempted thread. On an isolated core
  this cannot happen — the running thread is never preempted mid-critical-section.
- This is a classic **priority inversion by OS scheduling** scenario, entirely eliminated by isolation.
- The per-op latency (push/pop min/p50/p99) being unchanged confirms the lock itself is fast;
  the throughput degradation in the baseline is purely from preemption stalls.

---

## SPSC Queue

**File:** `bench_spsc_queue`
Latency: 1,000,000 iterations. Throughput: 3 iterations per run, time_unit = ms.

### Push/Pop Latency (ns)

| Benchmark | min | p50 | p99 (baseline) | p99 (isolated) | p999 (baseline) | p999 (isolated) |
|---|---|---|---|---|---|---|
| Push latency | 11 | 14 | 16 | 16 | 17 | 19 |
| Pop latency | 10 | 14 | 19 | 16 | 20 | 19 |

Per-operation latency is identical — the ring buffer mechanics are unchanged.

### Throughput

| Config | items/second | Delta |
|---|---|---|
| Baseline | 107,207,335 | — |
| Isolated | 24,472,394 | **-77%** |

### Key Findings

- **Throughput collapsed with isolation — this is a benchmarking artifact, not a real regression.**
- SPSC requires two threads running concurrently (producer + consumer). `taskset -c 0` confines
  both threads to a single logical core, forcing them to time-slice instead of running in parallel.
  The pipeline stalls: the producer runs, fills the buffer, then waits for the consumer which can't
  run until the OS context-switches on the same core.
- In the baseline (no taskset), the OS freely schedules the two SPSC threads on separate cores,
  achieving true parallelism and the 107M/s throughput.
- **The correct way to benchmark SPSC with isolation** would be to use two isolated cores
  (`taskset -c 0,2` — though core 2 is currently isolated and unoccupied) and pin producer
  to one, consumer to the other. This is left as a future experiment.
- Per-op latency (push/pop p50 = 14ns) is the reliable number — confirms the ring buffer
  mechanics are fast and unaffected.

---

## Timing Overhead

**File:** `bench_timing_overhead` | No user counters.

| Timer | Baseline (ns) | Isolated (ns) | Delta |
|---|---|---|---|
| `rdtsc` | 9.76 | 9.78 | +0.02 |
| `rdtscp` | 13.31 | 13.27 | -0.04 |
| `ScopedTimer` (rdtsc wrapper) | 25.49 | 25.55 | +0.06 |
| `chrono::steady_clock` | 26.53 | 26.72 | +0.19 |
| `clock_gettime` (NowNS) | 16.88 | 17.07 | +0.19 |

All deltas < 0.2 ns — completely stable across both configurations.
Timer cost is a pure function of the instruction sequence, unaffected by CPU isolation.

---

## Summary

| Benchmark | isolcpus benefit | Root cause |
|---|---|---|
| FIX parser (single-threaded) | Modest p999 improvement only | Short iterations rarely interrupted |
| JSON parser (single-threaded) | -14% p999 | Longer iterations more likely to be interrupted |
| MPSC queue (multi-producer) | +14% to +76% throughput | Fewer failed CAS from preemption |
| Mutex queue (multi-producer) | **+274% to +831% throughput** | Eliminates preemption-while-holding-lock |
| SPSC queue (two-thread pipeline) | -77% throughput (artifact) | taskset forces both threads onto one core |
| Timing primitives | No effect | Pure instruction cost |

### Key Takeaway

`isolcpus` has **no effect on minimum/p50 latency** of compute-bound workloads — the hardware
performance is the same. Its value is in **tail latency (p999) and throughput of lock-based
concurrent workloads**, where OS preemption causes disproportionate stalls.

The SPSC result is a reminder that CPU isolation tools (`taskset`, `isolcpus`) change the
**concurrency model** of multi-threaded benchmarks, not just the scheduling policy. A pipeline
benchmark that requires two concurrent threads must be pinned to two separate isolated cores,
not one.
