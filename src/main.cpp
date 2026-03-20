#include "binance_client.hpp"
#include "coinbase_client.hpp"
#include "kraken_client.hpp"
#include "bybit_client.hpp"
#include "dashboard.hpp"
#include "arbitrage_engine.hpp"
#include "fix_feed_simulator.hpp"
#include <iostream>
#include <signal.h>
#include <vector>
#include <string>
#include <thread>

// Global variables for signal handling
std::unique_ptr<BinanceWebSocketClient> g_binance_client;
std::unique_ptr<CoinbaseWebSocketClient> g_coinbase_client;
std::unique_ptr<KrakenWebSocketClient> g_kraken_client;
std::unique_ptr<BybitWebSocketClient> g_bybit_client;
std::unique_ptr<TerminalDashboard> g_dashboard;
std::unique_ptr<ArbitrageEngine> g_arbitrage_engine;
std::unique_ptr<FIXFeedSimulator> g_fix_simulator;
std::atomic<bool> g_shutdown(false);

void signal_handler(int signal) {
    std::cout << "\nShutdown signal received (" << signal << "). Cleaning up..." << std::endl;
    g_shutdown = true;

    if (g_arbitrage_engine) {
        g_arbitrage_engine->stop();
    }

    if (g_dashboard) {
        g_dashboard->stop();
    }

    if (g_binance_client) {
        g_binance_client->disconnect();
    }

    if (g_coinbase_client) {
        g_coinbase_client->disconnect();
    }

    if (g_kraken_client) {
        g_kraken_client->disconnect();
    }

    if (g_bybit_client) {
        g_bybit_client->disconnect();
    }
}

void setup_signal_handlers() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#ifndef _WIN32
    signal(SIGQUIT, signal_handler);
#endif
}

int main(int argc, char* argv[]) {
    // Parse command-line arguments
    int max_reports = 0;  // 0 = unlimited (default)
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--max-reports" && i + 1 < argc) {
            max_reports = std::stoi(argv[++i]);
        }
    }

    std::cout << "Multi-Exchange Crypto Arbitrage Dashboard\n";
    std::cout << "==========================================\n";
#ifdef USE_MPSC_QUEUE
    std::cout << "Queue Type: MPSC Lock-Free (shared, 4 producers)\n";
#else
    std::cout << "Queue Type: Shared Mutex (baseline contention)\n";
#endif
    std::cout << "Latency report prints every 10 seconds\n";
    if (max_reports > 0) {
        std::cout << "Benchmark mode: auto-shutdown after " << max_reports << " reports (~"
                  << max_reports * 10 << "s)\n";
    } else {
        std::cout << "Running indefinitely (use --max-reports N to auto-stop)\n";
    }
    std::cout << "\n";

    setup_signal_handlers();

    // Create dashboard, arbitrage engine, and WebSocket clients
    g_dashboard = std::make_unique<TerminalDashboard>();
    g_arbitrage_engine = std::make_unique<ArbitrageEngine>();
    g_binance_client = std::make_unique<BinanceWebSocketClient>();
    g_coinbase_client = std::make_unique<CoinbaseWebSocketClient>();
    g_kraken_client = std::make_unique<KrakenWebSocketClient>();
    g_bybit_client = std::make_unique<BybitWebSocketClient>();

    // Define symbols to monitor (Binance format)
    std::vector<std::string> symbols = {
        "BTCUSDT",   // Bitcoin
        "ETHUSDT",   // Ethereum
        "ADAUSDT",   // Cardano
        "DOTUSDT",   // Polkadot
        "SOLUSDT",   // Solana
        "MATICUSDT", // Polygon
        "AVAXUSDT",  // Avalanche
        "LTCUSDT",   // Litecoin
        "LINKUSDT",  // Chainlink
        "XLMUSDT",   // Stellar
        "XRPUSDT",   // Ripple
        "UNIUSDT",   // Uniswap
        "AAVEUSDT",  // Aave
        "ATOMUSDT",  // Cosmos
        "ALGOUSDT"   // Algorand
    };

    std::cout << "Monitoring " << symbols.size() << " cryptocurrency pairs across 5 exchanges:\n";
    std::cout << "Binance (depth20) + Coinbase (level2) + Kraken (book/10)"
                 " + Bybit (orderbook.50) + FIX Simulator\n\n";

    // Set up callbacks to update dashboard and arbitrage engine when new data arrives

    // Binance — Phase 2.4: depth feed (@depth20@100ms)
    // BBO callback: dashboard display + existing BBO detection (unchanged path)
    g_binance_client->set_message_callback([&](const TickerData& ticker) {
        g_dashboard->update_market_data(ticker);
        g_arbitrage_engine->update_market_data(ticker);
    });
    // L2 depth callback: maintain OrderBook for Binance (used by Phase 2.6 detection)
    g_binance_client->set_depth_callback([&](const OrderBookSnapshot& snap) {
        g_arbitrage_engine->update_order_book(snap);
    });

    // Coinbase — Phase 2.5: level2 feed (snapshot + incremental updates)
    g_coinbase_client->set_message_callback([&](const TickerData& ticker) {
        g_dashboard->update_market_data(ticker);
        g_arbitrage_engine->update_market_data(ticker);
    });
    g_coinbase_client->set_depth_callback([&](const OrderBookSnapshot& snap) {
        g_arbitrage_engine->update_order_book(snap);
    });

    // Kraken — Phase 2.5: book feed (depth=10, snapshot + incremental updates)
    g_kraken_client->set_message_callback([&](const TickerData& ticker) {
        g_dashboard->update_market_data(ticker);
        g_arbitrage_engine->update_market_data(ticker);
    });
    g_kraken_client->set_depth_callback([&](const OrderBookSnapshot& snap) {
        g_arbitrage_engine->update_order_book(snap);
    });

    // Bybit — Phase 2.5: orderbook.50 feed (snapshot + incremental deltas)
    g_bybit_client->set_message_callback([&](const TickerData& ticker) {
        g_dashboard->update_market_data(ticker);
        g_arbitrage_engine->update_market_data(ticker);
    });
    g_bybit_client->set_depth_callback([&](const OrderBookSnapshot& snap) {
        g_arbitrage_engine->update_order_book(snap);
    });

    // Link dashboard to arbitrage engine so it can pull opportunities
    g_dashboard->set_arbitrage_engine(g_arbitrage_engine.get());

    // ── FIX Feed Simulator (fifth producer) ───────────────────────────────
    // Generates synthetic L2 depth messages for the same crypto symbols as the
    // four WebSocket producers.  Feeds:
    //   - an OrderBook keyed "FIX" for L2 depth (Phase 2.3)
    //   - the existing BBO detection queue via to_ticker_data() so the arb
    //     engine sees FIX as a fifth exchange today (migrated in Phase 2.6)
    g_fix_simulator = std::make_unique<FIXFeedSimulator>("FIX");

    // Mirror the main symbol list in FIX format (USD suffix, no T).
    // normalize_symbol("BTCUSD") → "BTC", matching Binance "BTCUSDT" → "BTC".
    g_fix_simulator->add_symbol({"BTCUSD",  65000.0, 0.001});
    g_fix_simulator->add_symbol({"ETHUSD",   3500.0, 0.001});
    g_fix_simulator->add_symbol({"SOLUSD",    140.0, 0.002});
    g_fix_simulator->add_symbol({"LTCUSD",     85.0, 0.002});
    g_fix_simulator->add_symbol({"XRPUSD",      0.52, 0.002});
    g_fix_simulator->add_symbol({"ADAUSD",       0.45, 0.002});
    g_fix_simulator->add_symbol({"ATOMUSD",     10.0, 0.002});
    g_fix_simulator->add_symbol({"AVAXUSD",     35.0, 0.002});
    g_fix_simulator->add_symbol({"LINKUSD",     14.0, 0.002});
    g_fix_simulator->add_symbol({"UNIUSD",       8.0, 0.002});

    g_fix_simulator->set_snapshot_interval_ms(5000);
    g_fix_simulator->set_incremental_hz(20);  // 20 updates/sec across all symbols

    g_fix_simulator->set_callback([&](const std::string& /*raw*/, const FIXMessage& msg) {
        g_arbitrage_engine->update_fix_data(msg);
    });

    std::cout << "Connecting to exchanges..." << std::endl;

    // Connect to Binance WebSocket
    if (!g_binance_client->connect(symbols)) {
        std::cerr << "Failed to connect to Binance WebSocket!" << std::endl;
        return 1;
    }

    // Connect to Coinbase WebSocket
    if (!g_coinbase_client->connect(symbols)) {
        std::cerr << "Failed to connect to Coinbase WebSocket!" << std::endl;
        // Continue anyway
    }

    // Connect to Kraken WebSocket
    if (!g_kraken_client->connect(symbols)) {
        std::cerr << "Failed to connect to Kraken WebSocket!" << std::endl;
        // Continue anyway
    }

    // Connect to Bybit WebSocket
    if (!g_bybit_client->connect(symbols)) {
        std::cerr << "Failed to connect to Bybit WebSocket!" << std::endl;
        // Continue anyway
    }

    std::cout << "Connected successfully! Starting arbitrage engine and dashboard..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));  // Wait a bit for data to flow

    // Start the arbitrage engine (Thread 2)
    g_arbitrage_engine->set_min_profit_bps(0.1);  // 0.1 basis points minimum profit
    g_arbitrage_engine->set_max_reports(max_reports);
    g_arbitrage_engine->set_shutdown_callback([]() {
        g_shutdown = true;
    });
    g_arbitrage_engine->start();

    // Start FIX simulator — begins producing L2 depth messages immediately
    g_fix_simulator->start();
    std::cout << "FIX feed simulator started (10 symbols, 20 updates/sec, "
                 "snapshot every 5s)\n";

    // Start the dashboard (Thread 1 - display)
    g_dashboard->set_update_interval(std::chrono::milliseconds(500)); // Update every 500ms
    g_dashboard->start();

    // Main application loop
    while (!g_shutdown && (g_binance_client->is_connected() ||
                           g_coinbase_client->is_connected() ||
                           g_kraken_client->is_connected() ||
                           g_bybit_client->is_connected())) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "\nShutting down..." << std::endl;

    // Cleanup
    if (g_fix_simulator) {
        g_fix_simulator->stop();
    }

    if (g_arbitrage_engine) {
        g_arbitrage_engine->stop();
    }

    if (g_dashboard) {
        g_dashboard->stop();
    }

    if (g_binance_client) {
        g_binance_client->disconnect();
    }

    if (g_coinbase_client) {
        g_coinbase_client->disconnect();
    }

    if (g_kraken_client) {
        g_kraken_client->disconnect();
    }

    if (g_bybit_client) {
        g_bybit_client->disconnect();
    }

    std::cout << "Application stopped cleanly." << std::endl;
    return 0;
}
