#pragma once

#include <iostream>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <mach/thread_act.h>
#include <pthread.h>
#elif defined(__linux__)
#include <sched.h>
#include <pthread.h>
#endif

// Thread affinity hints for scheduling threads on separate cores.
//
// macOS  — tag is an arbitrary hint group ID. The kernel uses it as a soft
//          hint to place threads with DIFFERENT tags on different cores/L2
//          domains. The numeric value is opaque; only uniqueness matters.
//
// Linux  — tag is a LITERAL CPU core ID passed to pthread_setaffinity_np.
//          The value must correspond to a real core (0 … nproc-1).
//
//          Machine: i5-2410M — 2 physical cores, 4 logical cores (HT).
//          HT topology:
//            physical core 0  →  logical 0 + logical 2  (siblings)
//            physical core 1  →  logical 1 + logical 3  (siblings)
//
//          GRUB: isolcpus=0,2 — isolates ALL logical threads of physical
//          core 0 from the OS scheduler. This gives ArbitrageEngine the
//          entire physical core with no HT sibling contention and no OS
//          scheduling interference.
//
//          Key lesson: isolcpus must isolate BOTH HT siblings of a physical
//          core to be effective. Isolating only one sibling (e.g. isolcpus=2,3)
//          still allows the OS to run on the paired sibling (0,1), causing
//          L1/L2 cache eviction and shared execution-unit contention.
//
//          Core 2 is isolated but intentionally left unoccupied — keeping
//          the HT sibling idle eliminates all resource competition for core 0.
//          Cores 1 and 3 (physical core 1) remain shared with the OS.
namespace thread_affinity {

#ifdef __linux__
    // Linux: values are literal core IDs.
    // isolcpus=0,2 — physical core 0 fully dedicated to ArbitrageEngine.
    constexpr int TAG_ARBITRAGE_ENGINE = 0;  // isolated — hot path, full physical core
    constexpr int TAG_BINANCE_WS      = 1;  // non-isolated, physical core 1
    constexpr int TAG_COINBASE_WS     = 1;  // non-isolated, shares core 1
    constexpr int TAG_KRAKEN_WS       = 3;  // non-isolated, physical core 1 (HT sibling)
    constexpr int TAG_BYBIT_WS        = 3;  // non-isolated, shares core 3
    constexpr int TAG_DASHBOARD       = 1;  // non-isolated, lowest priority
#else
    // macOS: values are opaque hint group IDs — uniqueness = separate cores.
    constexpr int TAG_ARBITRAGE_ENGINE = 1;  // hot path
    constexpr int TAG_BINANCE_WS      = 2;
    constexpr int TAG_COINBASE_WS     = 3;
    constexpr int TAG_KRAKEN_WS       = 4;
    constexpr int TAG_BYBIT_WS        = 5;
    constexpr int TAG_DASHBOARD       = 6;  // lowest priority
#endif

    // Set thread affinity hint for the CURRENT thread.
    // Must be called from within the target thread (uses pthread_self()).
    // Returns true on success, false on failure (non-fatal).
    inline bool set_thread_affinity(int tag) {
#ifdef __APPLE__
        thread_affinity_policy_data_t policy;
        policy.affinity_tag = tag;

        kern_return_t ret = thread_policy_set(
            pthread_mach_thread_np(pthread_self()),
            THREAD_AFFINITY_POLICY,
            reinterpret_cast<thread_policy_t>(&policy),
            THREAD_AFFINITY_POLICY_COUNT
        );

        if (ret != KERN_SUCCESS) {
            std::cerr << "Warning: thread_policy_set failed for tag "
                      << tag << " (kern_return=" << ret << ")" << std::endl;
            return false;
        }
        return true;

#elif defined(__linux__)
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(tag, &cpuset);

        int ret = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
        if (ret != 0) {
            std::cerr << "Warning: pthread_setaffinity_np failed for tag "
                      << tag << " (errno=" << ret << ")" << std::endl;
            return false;
        }
        return true;

#else
        (void)tag;
        return true;
#endif
    }

} // namespace thread_affinity
