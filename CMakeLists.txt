cmake_minimum_required(VERSION 3.20)
project(BinanceDashboard CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Option to use SPSC lock-free queue instead of mutex-based queue
option(USE_SPSC_QUEUE "Use SPSC lock-free queue instead of mutex" OFF)

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
    $<$<BOOL:${USE_SPSC_QUEUE}>:USE_SPSC_QUEUE>
)

# For macOS, link system libraries
if(APPLE)
    find_library(COREFOUNDATION_LIBRARY CoreFoundation)
    find_library(SECURITY_LIBRARY Security)
    target_link_libraries(binance_dashboard PRIVATE
        ${COREFOUNDATION_LIBRARY}
        ${SECURITY_LIBRARY}
    )
endif()
