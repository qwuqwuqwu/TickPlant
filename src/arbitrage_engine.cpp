#include "arbitrage_engine.hpp"
#include "fix_parser.hpp"
#include "order_book.hpp"
#include "queue_latency_tracker.hpp"
#include "thread_affinity.hpp"
#include <iostream>
#include <algorithm>

ArbitrageEngine::ArbitrageEngine()
    : running_(false)
    , calculation_count_(0)
    , opportunity_count_(0)
    , min_profit_bps_(5.0)  // 5 basis points minimum profit
    , max_reports_(0)       // 0 = unlimited
    , report_count_(0) {
}

ArbitrageEngine::~ArbitrageEngine() {
    stop();
}

void ArbitrageEngine::start() {
    if (running_) {
        return;
    }

    running_ = true;
    calculation_thread_ = std::thread(&ArbitrageEngine::calculation_loop, this);
#ifdef USE_MPSC_QUEUE
    std::cout << "Arbitrage engine started (shared MPSC lock-free queue)." << std::endl;
#else
    std::cout << "Arbitrage engine started (shared mutex queue)." << std::endl;
#endif
}

void ArbitrageEngine::stop() {
    running_ = false;
    cv_.notify_one();

    if (calculation_thread_.joinable()) {
        calculation_thread_.join();
    }

    // Print final latency report
    print_latency_report();

    std::cout << "Arbitrage engine stopped." << std::endl;
}

void ArbitrageEngine::update_market_data(const TickerData& ticker) {
    // Push to per-exchange queue (timestamp is recorded inside push())
    incoming_queues_.push(ticker);
    cv_.notify_one();
}

void ArbitrageEngine::set_opportunity_callback(ArbitrageCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    opportunity_callback_ = callback;
}

void ArbitrageEngine::set_min_profit_bps(double min_profit_bps) {
    min_profit_bps_ = min_profit_bps;
}

void ArbitrageEngine::set_max_reports(int max_reports) {
    max_reports_ = max_reports;
}

void ArbitrageEngine::set_shutdown_callback(std::function<void()> callback) {
    shutdown_callback_ = std::move(callback);
}

std::vector<ArbitrageOpportunity> ArbitrageEngine::get_opportunities() const {
    std::lock_guard<std::mutex> lock(opportunities_mutex_);
    return opportunities_;
}

void ArbitrageEngine::print_latency_report() const {
    get_queue_latency_tracker().print_report();
}

void ArbitrageEngine::calculation_loop() {
    // Pin arbitrage engine thread (hot path - most latency-sensitive)
    thread_affinity::set_thread_affinity(thread_affinity::TAG_ARBITRAGE_ENGINE);

    // Print latency report periodically
    auto last_report_time = std::chrono::steady_clock::now();
    const auto report_interval = std::chrono::seconds(10);

    while (running_) {
        {
            std::unique_lock<std::mutex> lk(cv_mutex_);
            cv_.wait(lk, [this] { return !incoming_queues_.empty() || !running_.load(); });
        }

        if (!running_) break;

        // Drain all incoming queues into market_data_
        drain_incoming_queues();

        // Calculate arbitrage
        calculate_arbitrage();

        // Print latency report every 10 seconds
        auto now = std::chrono::steady_clock::now();
        if (now - last_report_time >= report_interval) {
            report_count_++;
            std::cout << "\n[Report " << report_count_;
            if (max_reports_ > 0) {
                std::cout << "/" << max_reports_;
            }
            std::cout << "]\n";
            print_latency_report();
            last_report_time = now;

            // Auto-shutdown after max reports reached
            if (max_reports_ > 0 && report_count_ >= max_reports_) {
                std::cout << "\nBenchmark complete: " << report_count_
                          << " reports collected. Shutting down gracefully.\n";
                running_ = false;
                if (shutdown_callback_) {
                    shutdown_callback_();
                }
                break;
            }
        }

    }
}

void ArbitrageEngine::drain_incoming_queues() {
    // Drain all queues - this updates market_data_ with latest prices
    // Latency measurement happens inside try_pop() automatically
    incoming_queues_.drain_all(market_data_);
}

void ArbitrageEngine::calculate_arbitrage() {
    // No lock needed here - market_data_ is only accessed by this thread
    calculation_count_++;

    // Build a map of symbols to their tickers across exchanges
    std::unordered_map<std::string, std::vector<TickerData>> symbol_map;

    for (const auto& [key, ticker] : market_data_) {
        // Consider LIVE and SLOW data (but not STALE)
        auto status = get_data_status(ticker);
        if (status == DataStatus::LIVE || status == DataStatus::SLOW) {
            std::string normalized = normalize_symbol(ticker.symbol);
            symbol_map[normalized].push_back(ticker);
        }
    }

    // Find arbitrage opportunities
    std::vector<ArbitrageOpportunity> new_opportunities;

    for (const auto& [symbol, tickers] : symbol_map) {
        // Need at least 2 exchanges for arbitrage
        if (tickers.size() < 2) {
            continue;
        }

        // Check all pairs of exchanges for this symbol
        for (size_t i = 0; i < tickers.size(); ++i) {
            for (size_t j = i + 1; j < tickers.size(); ++j) {
                const auto& ticker1 = tickers[i];
                const auto& ticker2 = tickers[j];

                // Check data age difference to detect stale data
                auto age1_ms = ticker1.age().count();
                auto age2_ms = ticker2.age().count();
                auto age_diff_ms = std::abs(static_cast<long>(age1_ms - age2_ms));

                // Log timestamp differences for debugging (first 5 times)
                static int timestamp_debug_count = 0;
                if (timestamp_debug_count < 5 && age_diff_ms > 100) {
                    std::cout << "DEBUG: Age difference for " << symbol << ": "
                              << ticker1.exchange << "=" << age1_ms << "ms, "
                              << ticker2.exchange << "=" << age2_ms << "ms, "
                              << "diff=" << age_diff_ms << "ms" << std::endl;
                    timestamp_debug_count++;
                }

                // Skip if data age difference is too large (>500ms)
                if (age_diff_ms > 500) {
                    continue;
                }

                // Check arbitrage in both directions
                // Direction 1: Buy on exchange1, sell on exchange2
                if (ticker2.bid_price > ticker1.ask_price) {
                    double profit_bps = ((ticker2.bid_price - ticker1.ask_price) / ticker1.ask_price) * 10000.0;

                    if (profit_bps >= min_profit_bps_) {
                        ArbitrageOpportunity opp;
                        opp.symbol = symbol;
                        opp.buy_exchange = ticker1.exchange;
                        opp.sell_exchange = ticker2.exchange;
                        opp.buy_price = ticker1.ask_price;
                        opp.sell_price = ticker2.bid_price;
                        opp.profit_bps = profit_bps;
                        opp.max_quantity = std::min(ticker1.ask_quantity, ticker2.bid_quantity);

                        static int direction_debug = 0;
                        if (direction_debug < 3) {
                            std::cout << "DEBUG: Opportunity " << symbol << " Buy " << ticker1.exchange
                                      << " @ " << ticker1.ask_price << " (age=" << age1_ms << "ms) Sell "
                                      << ticker2.exchange << " @ " << ticker2.bid_price << " (age=" << age2_ms
                                      << "ms) Profit=" << profit_bps << "bp" << std::endl;
                            direction_debug++;
                        }

                        auto now = std::chrono::system_clock::now();
                        opp.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()
                        ).count();

                        new_opportunities.push_back(opp);
                        opportunity_count_++;

                        {
                            std::lock_guard<std::mutex> cb_lock(callback_mutex_);
                            if (opportunity_callback_) {
                                opportunity_callback_(opp);
                            }
                        }
                    }
                }

                // Direction 2: Buy on exchange2, sell on exchange1
                if (ticker1.bid_price > ticker2.ask_price) {
                    double profit_bps = ((ticker1.bid_price - ticker2.ask_price) / ticker2.ask_price) * 10000.0;

                    if (profit_bps >= min_profit_bps_) {
                        ArbitrageOpportunity opp;
                        opp.symbol = symbol;
                        opp.buy_exchange = ticker2.exchange;
                        opp.sell_exchange = ticker1.exchange;
                        opp.buy_price = ticker2.ask_price;
                        opp.sell_price = ticker1.bid_price;
                        opp.profit_bps = profit_bps;
                        opp.max_quantity = std::min(ticker2.ask_quantity, ticker1.bid_quantity);

                        auto now = std::chrono::system_clock::now();
                        opp.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()
                        ).count();

                        new_opportunities.push_back(opp);
                        opportunity_count_++;

                        {
                            std::lock_guard<std::mutex> cb_lock(callback_mutex_);
                            if (opportunity_callback_) {
                                opportunity_callback_(opp);
                            }
                        }
                    }
                }
            }
        }
    }

    // Update stored opportunities
    {
        std::lock_guard<std::mutex> opp_lock(opportunities_mutex_);
        opportunities_ = std::move(new_opportunities);
    }
}

void ArbitrageEngine::update_order_book(const OrderBookSnapshot& snap) {
    const std::string key = snap.exchange + ":" + snap.symbol;

    std::lock_guard<std::mutex> lk(ws_books_mutex_);
    auto it = ws_books_.find(key);
    if (it == ws_books_.end()) {
        ws_books_.emplace(key, std::make_unique<OrderBook>(snap.symbol, snap.exchange));
        it = ws_books_.find(key);
    }

    OrderBook& book = *it->second;

    if (snap.is_snapshot) {
        // Full replace — clear then repopulate from every level in the snapshot.
        // Used by Binance (@depth20@100ms always sends complete partial book) and
        // the initial snapshot from Bybit / Coinbase / Kraken.
        book.clear();
        for (const auto& level : snap.bids)
            book.set_level(OrderBook::Side::Bid, level.price, level.quantity, level.order_count);
        for (const auto& level : snap.asks)
            book.set_level(OrderBook::Side::Ask, level.price, level.quantity, level.order_count);
    } else {
        // Incremental delta — apply each changed level individually.
        // quantity == 0.0 means "delete this price level" (Bybit/Coinbase/Kraken convention).
        for (const auto& level : snap.bids) {
            if (level.quantity == 0.0)
                book.delete_level(OrderBook::Side::Bid, level.price);
            else
                book.set_level(OrderBook::Side::Bid, level.price, level.quantity, level.order_count);
        }
        for (const auto& level : snap.asks) {
            if (level.quantity == 0.0)
                book.delete_level(OrderBook::Side::Ask, level.price);
            else
                book.set_level(OrderBook::Side::Ask, level.price, level.quantity, level.order_count);
        }
    }
    // Note: does NOT call update_market_data() — BBO is already pushed by the
    // client's MessageCallback.  Two separate calls, one queue entry, no duplication.
}

void ArbitrageEngine::update_fix_data(const FIXMessage& msg) {
    if (!msg.is_market_data()) return;

    const std::string sym(msg.symbol);

    // ── (a) Update L2 OrderBook ────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lk(fix_books_mutex_);
        auto it = fix_books_.find(sym);
        if (it == fix_books_.end()) {
            fix_books_.emplace(sym, std::make_unique<OrderBook>(sym, "FIX"));
            it = fix_books_.find(sym);
        }
        if (msg.msg_type == FIXMsgType::MarketDataSnapshot) {
            it->second->apply_snapshot(msg);
        } else {
            it->second->apply_update(msg);
        }
    }

    // ── (b) Feed BBO to existing detection path ───────────────────────────
    // to_ticker_data() already sets exchange="FIX" and copies symbol to string.
    // This feeds the FIX simulator into calculate_arbitrage() alongside the
    // four WebSocket producers until Phase 2.6 migrates to pure L2 detection.
    if (auto td = msg.to_ticker_data()) {
        update_market_data(*td);  // also calls cv_.notify_one()
    }
}

std::string ArbitrageEngine::normalize_symbol(const std::string& symbol) const {
    std::string normalized = symbol;

    // Convert to uppercase
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::toupper);

    // Handle Coinbase format (BTC-USD) -> normalize to base currency
    if (normalized.find('-') != std::string::npos) {
        normalized = normalized.substr(0, normalized.find('-'));
    }
    // Handle Kraken format (BTC/USD) -> normalize to base currency
    else if (normalized.find('/') != std::string::npos) {
        normalized = normalized.substr(0, normalized.find('/'));
    }
    // Handle Binance/Bybit format (BTCUSDT) -> normalize to base currency
    else if (normalized.length() > 4 && normalized.substr(normalized.length() - 4) == "USDT") {
        normalized = normalized.substr(0, normalized.length() - 4);
    }
    else if (normalized.length() > 3 && normalized.substr(normalized.length() - 3) == "USD") {
        normalized = normalized.substr(0, normalized.length() - 3);
    }

    return normalized;
}
