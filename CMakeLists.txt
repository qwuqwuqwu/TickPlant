cmake_minimum_required(VERSION 3.20)
project(BinanceDashboard CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Queue implementation: MUTEX (default), SPSC, or MPSC
set(QUEUE_TYPE "MUTEX" CACHE STRING "Queue implementation: MUTEX | SPSC | MPSC")
set_property(CACHE QUEUE_TYPE PROPERTY STRINGS MUTEX SPSC MPSC)

if(NOT QUEUE_TYPE MATCHES "^(MUTEX|SPSC|MPSC)$")
    message(FATAL_ERROR "Invalid QUEUE_TYPE '${QUEUE_TYPE}'. Must be MUTEX, SPSC, or MPSC.")
endif()

# Find required packages
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED COMPONENTS system thread)

# Add executable
add_executable(binance_dashboard
    src/main.cpp
    src/binance_client.cpp
    src/bybit_client.cpp
    src/coinbase_client.cpp
    src/kraken_client.cpp
    src/dashboard.cpp
    src/arbitrage_engine.cpp
)

# Include directories
target_include_directories(binance_dashboard PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${Boost_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(binance_dashboard PRIVATE
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    Boost::system
    Boost::thread
    Threads::Threads
)

# Compiler flags for performance
target_compile_options(binance_dashboard PRIVATE
    $<$<CONFIG:Release>:-O3 -march=native -DNDEBUG>
    $<$<CONFIG:Debug>:-O0 -g>
)

# Additional compiler definitions
target_compile_definitions(binance_dashboard PRIVATE
    BOOST_ASIO_HAS_STD_CHRONO
    $<$<STREQUAL:${QUEUE_TYPE},SPSC>:USE_SPSC_QUEUE>
    $<$<STREQUAL:${QUEUE_TYPE},MPSC>:USE_MPSC_QUEUE>
)

# ── Benchmarks ───────────────────────────────────────────────────────────────
# Enable with: cmake .. -DBUILD_BENCHMARKS=ON
# Requires google-benchmark in vcpkg.json (already added).
option(BUILD_BENCHMARKS "Build Google Benchmark harnesses" OFF)

if(BUILD_BENCHMARKS)
    find_package(benchmark REQUIRED)

    # Helper: create a benchmark executable with the right flags and includes.
    function(add_tickplant_benchmark target source)
        add_executable(${target} ${source})
        target_include_directories(${target} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
        )
        target_link_libraries(${target} PRIVATE
            benchmark::benchmark_main   # already pulls in benchmark::benchmark
            Threads::Threads
        )
        target_compile_options(${target} PRIVATE
            -O3 -march=native
        )
    endfunction()

    add_tickplant_benchmark(bench_mpsc_queue      benchmarks/bench_mpsc_queue.cpp)
    add_tickplant_benchmark(bench_mutex_queue     benchmarks/bench_mutex_queue.cpp)
    add_tickplant_benchmark(bench_spsc_queue      benchmarks/bench_spsc_queue.cpp)
    add_tickplant_benchmark(bench_timing_overhead benchmarks/bench_timing_overhead.cpp)
endif()

# For macOS, link system libraries
if(APPLE)
    find_library(COREFOUNDATION_LIBRARY CoreFoundation)
    find_library(SECURITY_LIBRARY Security)
    target_link_libraries(binance_dashboard PRIVATE
        ${COREFOUNDATION_LIBRARY}
        ${SECURITY_LIBRARY}
    )
endif()
