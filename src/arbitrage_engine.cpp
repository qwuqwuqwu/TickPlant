#include "arbitrage_engine.hpp"
#include "fix_parser.hpp"
#include "order_book.hpp"
#include "thread_affinity.hpp"
#include "metrics.hpp"
#include <iostream>
#include <algorithm>
#include <chrono>

// ─── Constructor / Destructor ─────────────────────────────────────────────────

ArbitrageEngine::ArbitrageEngine()
    : running_(false)
    , books_dirty_(false)
    , calculation_count_(0)
    , opportunity_count_(0)
    , min_profit_bps_(5.0)
    , max_reports_(0)
    , report_count_(0) {
}

ArbitrageEngine::~ArbitrageEngine() {
    stop();
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

void ArbitrageEngine::start() {
    if (running_) return;
    running_ = true;
    calculation_thread_ = std::thread(&ArbitrageEngine::calculation_loop, this);
    std::cout << "Arbitrage engine started (pure L2 book comparison mode).\n";
}

void ArbitrageEngine::stop() {
    running_ = false;
    books_dirty_ = true;    // unblock the wait
    cv_.notify_one();

    if (calculation_thread_.joinable()) {
        calculation_thread_.join();
    }

    print_latency_report();
    std::cout << "Arbitrage engine stopped.\n";
}

// ─── Producer-side API (called from WebSocket / FIX threads) ─────────────────

void ArbitrageEngine::update_order_book(const OrderBookSnapshot& snap) {
    const std::string key = snap.exchange + ":" + snap.symbol;

    {
        std::lock_guard<std::mutex> lk(ws_books_mutex_);
        auto it = ws_books_.find(key);
        if (it == ws_books_.end()) {
            ws_books_.emplace(key, std::make_unique<OrderBook>(snap.symbol, snap.exchange));
            it = ws_books_.find(key);
        }

        OrderBook& book = *it->second;

        {
            ScopedNsTimer ob_timer([&](double ns){
                Metrics::instance().record_orderbook_update(snap.exchange, ns);
            });

            if (snap.is_snapshot) {
                // Full replace — clear then repopulate.
                // Binance (@depth20@100ms always complete), and initial snapshot from
                // Bybit / Coinbase / Kraken.
                book.clear();
                for (const auto& lvl : snap.bids)
                    book.set_level(OrderBook::Side::Bid, lvl.price, lvl.quantity, lvl.order_count);
                for (const auto& lvl : snap.asks)
                    book.set_level(OrderBook::Side::Ask, lvl.price, lvl.quantity, lvl.order_count);
            } else {
                // Incremental delta — quantity == 0.0 means "delete this price level".
                for (const auto& lvl : snap.bids) {
                    if (lvl.quantity == 0.0)
                        book.delete_level(OrderBook::Side::Bid, lvl.price);
                    else
                        book.set_level(OrderBook::Side::Bid, lvl.price, lvl.quantity, lvl.order_count);
                }
                for (const auto& lvl : snap.asks) {
                    if (lvl.quantity == 0.0)
                        book.delete_level(OrderBook::Side::Ask, lvl.price);
                    else
                        book.set_level(OrderBook::Side::Ask, lvl.price, lvl.quantity, lvl.order_count);
                }
            }
        }
    }

    Metrics::instance().record_message(snap.exchange, snap.is_snapshot);

    // Wake the calculation thread
    books_dirty_ = true;
    cv_.notify_one();
}

void ArbitrageEngine::update_fix_data(const FIXMessage& msg) {
    if (!msg.is_market_data()) return;

    const std::string sym(msg.symbol);

    {
        std::lock_guard<std::mutex> lk(fix_books_mutex_);
        auto it = fix_books_.find(sym);
        if (it == fix_books_.end()) {
            fix_books_.emplace(sym, std::make_unique<OrderBook>(sym, "FIX"));
            it = fix_books_.find(sym);
        }
        bool is_snapshot = (msg.msg_type == FIXMsgType::MarketDataSnapshot);
        {
            ScopedNsTimer fix_timer([](double ns){
                Metrics::instance().record_parse_latency("fix", ns);
            });
            if (is_snapshot)
                it->second->apply_snapshot(msg);
            else
                it->second->apply_update(msg);
        }
        Metrics::instance().record_message("FIX", is_snapshot);
    }

    // Wake the calculation thread
    books_dirty_ = true;
    cv_.notify_one();
}

// ─── Configuration ────────────────────────────────────────────────────────────

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
    std::cout << "  Calculations: " << calculation_count_.load()
              << "  Opportunities found: " << opportunity_count_.load() << "\n";

    // Book inventory: how many live books per side
    size_t ws_count = 0, fix_count = 0;
    {
        std::lock_guard<std::mutex> lk(ws_books_mutex_);
        ws_count = ws_books_.size();
    }
    {
        std::lock_guard<std::mutex> lk(fix_books_mutex_);
        fix_count = fix_books_.size();
    }
    std::cout << "  L2 books: " << ws_count << " WebSocket  "
              << fix_count << " FIX\n";
}

// ─── Calculation thread ───────────────────────────────────────────────────────

void ArbitrageEngine::calculation_loop() {
    thread_affinity::set_thread_affinity(thread_affinity::TAG_ARBITRAGE_ENGINE);

    auto last_report_time = std::chrono::steady_clock::now();
    const auto report_interval = std::chrono::seconds(10);

    while (running_) {
        // Sleep until any book is updated or shutdown is requested
        {
            std::unique_lock<std::mutex> lk(cv_mutex_);
            cv_.wait(lk, [this] { return books_dirty_.load() || !running_.load(); });
            books_dirty_ = false;
        }

        if (!running_) break;

        calculate_arbitrage();

        auto now = std::chrono::steady_clock::now();
        if (now - last_report_time >= report_interval) {
            report_count_++;
            std::cout << "\n[Report " << report_count_;
            if (max_reports_ > 0) std::cout << "/" << max_reports_;
            std::cout << "]\n";
            print_latency_report();
            last_report_time = now;

            if (max_reports_ > 0 && report_count_ >= max_reports_) {
                std::cout << "\nBenchmark complete: " << report_count_
                          << " reports collected. Shutting down gracefully.\n";
                running_ = false;
                if (shutdown_callback_) shutdown_callback_();
                break;
            }
        }
    }
}

// ─── Arbitrage detection (pure L2) ───────────────────────────────────────────

void ArbitrageEngine::calculate_arbitrage() {
    calculation_count_++;

    // ── Snapshot BBO for every book ───────────────────────────────────────────
    // We hold each outer mutex only long enough to iterate the map and call
    // book->get_snapshot() (which acquires the book's internal shared_mutex).
    // Result: by_symbol[normalized_sym] = vector of (exchange, snapshot) pairs.
    using BookEntry = std::pair<std::string, OrderBookSnapshot>;
    std::unordered_map<std::string, std::vector<BookEntry>> by_symbol;

    const auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // Per-exchange staleness threshold.
    // Binance @depth20@100ms only emits when the book actually changes — on
    // low-liquidity symbols (XLM, ALGO, UNI, …) the stream can legitimately go
    // quiet for 10–20 s.  Using 500 ms would incorrectly discard those books.
    // All other feeds (Coinbase level2, Kraken book/10, Bybit orderbook.50, FIX)
    // push incremental updates continuously; 500 ms there is a reliable dead-feed
    // detector.
    auto staleness_limit_ms = [](const std::string& exchange) -> double {
        return (exchange == "Binance") ? 30'000.0 : 500.0;
    };

    {
        std::lock_guard<std::mutex> lk(ws_books_mutex_);
        for (const auto& [key, book] : ws_books_) {
            auto snap = book->get_snapshot();
            if (snap.empty()) continue;
            double staleness_ms = static_cast<double>(now_ms - snap.timestamp_ms);
            Metrics::instance().set_book_staleness(snap.exchange, snap.symbol, staleness_ms);
            Metrics::instance().set_book_depth(snap.exchange, snap.symbol, "bid",
                                               static_cast<double>(snap.bids.size()));
            Metrics::instance().set_book_depth(snap.exchange, snap.symbol, "ask",
                                               static_cast<double>(snap.asks.size()));
            if (staleness_ms > staleness_limit_ms(snap.exchange)) continue;
            std::string norm = normalize_symbol(snap.symbol);
            by_symbol[norm].emplace_back(snap.exchange, std::move(snap));
        }
    }

    {
        std::lock_guard<std::mutex> lk(fix_books_mutex_);
        for (const auto& [sym, book] : fix_books_) {
            auto snap = book->get_snapshot();
            if (snap.empty()) continue;
            double staleness_ms = static_cast<double>(now_ms - snap.timestamp_ms);
            Metrics::instance().set_book_staleness("FIX", sym, staleness_ms);
            Metrics::instance().set_book_depth("FIX", sym, "bid",
                                               static_cast<double>(snap.bids.size()));
            Metrics::instance().set_book_depth("FIX", sym, "ask",
                                               static_cast<double>(snap.asks.size()));
            if (staleness_ms > staleness_limit_ms("FIX")) continue;
            std::string norm = normalize_symbol(sym);
            by_symbol[norm].emplace_back("FIX", std::move(snap));
        }
    }

    // ── Compare all (exchange_i, exchange_j) pairs for each symbol ────────────
    // The quantity check at the best level eliminates phantom arbitrage signals
    // where a spread exists but there is zero size available to execute against.
    std::vector<ArbitrageOpportunity> new_opportunities;

    for (const auto& [norm_sym, entries] : by_symbol) {
        if (entries.size() < 2) continue;

        for (size_t i = 0; i < entries.size(); ++i) {
            for (size_t j = i + 1; j < entries.size(); ++j) {
                const auto& [ex1, snap1] = entries[i];
                const auto& [ex2, snap2] = entries[j];

                // Direction 1: Buy on exchange1 at ask, sell on exchange2 at bid
                if (!snap1.asks.empty() && !snap2.bids.empty()) {
                    double ask = snap1.best_ask();
                    double bid = snap2.best_bid();
                    if (bid > ask && ask > 0.0) {
                        double profit_bps = ((bid - ask) / ask) * 10'000.0;
                        double qty = std::min(snap1.asks.front().quantity,
                                             snap2.bids.front().quantity);
                        if (profit_bps >= min_profit_bps_ && qty > 0.0) {
                            ArbitrageOpportunity opp;
                            opp.symbol        = norm_sym;
                            opp.buy_exchange  = ex1;
                            opp.sell_exchange = ex2;
                            opp.buy_price     = ask;
                            opp.sell_price    = bid;
                            opp.profit_bps    = profit_bps;
                            opp.max_quantity  = qty;
                            opp.timestamp_ms  = now_ms;
                            new_opportunities.push_back(opp);
                            opportunity_count_++;
                            Metrics::instance().record_arb_detection(ex1, ex2, norm_sym);

                            {
                                std::lock_guard<std::mutex> cb(callback_mutex_);
                                if (opportunity_callback_) opportunity_callback_(opp);
                            }
                        }
                    }
                }

                // Direction 2: Buy on exchange2 at ask, sell on exchange1 at bid
                if (!snap2.asks.empty() && !snap1.bids.empty()) {
                    double ask = snap2.best_ask();
                    double bid = snap1.best_bid();
                    if (bid > ask && ask > 0.0) {
                        double profit_bps = ((bid - ask) / ask) * 10'000.0;
                        double qty = std::min(snap2.asks.front().quantity,
                                             snap1.bids.front().quantity);
                        if (profit_bps >= min_profit_bps_ && qty > 0.0) {
                            ArbitrageOpportunity opp;
                            opp.symbol        = norm_sym;
                            opp.buy_exchange  = ex2;
                            opp.sell_exchange = ex1;
                            opp.buy_price     = ask;
                            opp.sell_price    = bid;
                            opp.profit_bps    = profit_bps;
                            opp.max_quantity  = qty;
                            opp.timestamp_ms  = now_ms;
                            new_opportunities.push_back(opp);
                            opportunity_count_++;
                            Metrics::instance().record_arb_detection(ex2, ex1, norm_sym);

                            {
                                std::lock_guard<std::mutex> cb(callback_mutex_);
                                if (opportunity_callback_) opportunity_callback_(opp);
                            }
                        }
                    }
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> opp_lock(opportunities_mutex_);
        opportunities_ = std::move(new_opportunities);
    }
}

// ─── Symbol normalization ─────────────────────────────────────────────────────

std::string ArbitrageEngine::normalize_symbol(const std::string& symbol) const {
    std::string norm = symbol;
    std::transform(norm.begin(), norm.end(), norm.begin(), ::toupper);

    // Coinbase: "BTC-USD" → "BTC"
    if (auto pos = norm.find('-'); pos != std::string::npos) {
        norm.resize(pos);
    }
    // Kraken: "BTC/USD" → "BTC"
    else if (auto pos = norm.find('/'); pos != std::string::npos) {
        norm.resize(pos);
    }
    // Binance/Bybit: "BTCUSDT" → "BTC"
    else if (norm.size() > 4 && norm.substr(norm.size() - 4) == "USDT") {
        norm.resize(norm.size() - 4);
    }
    // FIX simulator: "BTCUSD" → "BTC"
    else if (norm.size() > 3 && norm.substr(norm.size() - 3) == "USD") {
        norm.resize(norm.size() - 3);
    }

    return norm;
}
