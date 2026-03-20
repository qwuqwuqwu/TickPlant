#include "coinbase_client.hpp"
#include "thread_affinity.hpp"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <chrono>

CoinbaseWebSocketClient::CoinbaseWebSocketClient()
    : connected_(false)
    , should_stop_(false)
    , message_count_(0) {
}

CoinbaseWebSocketClient::~CoinbaseWebSocketClient() {
    disconnect();
}

bool CoinbaseWebSocketClient::connect(const std::vector<std::string>& symbols) {
    if (connected_) {
        std::cerr << "Already connected!" << std::endl;
        return false;
    }

    subscribed_symbols_ = symbols;

    try {
        std::string host = "advanced-trade-ws.coinbase.com";
        std::string port = "443";
        std::string target = "/";

        std::cout << "Connecting to Coinbase: wss://" << host << target << std::endl;

        // Initialize IO context and SSL context
        ioc_ = std::make_unique<net::io_context>();
        ctx_ = std::make_unique<ssl::context>(ssl::context::tlsv12_client);

        // Load root certificates
        ctx_->set_default_verify_paths();
        ctx_->set_verify_mode(ssl::verify_none); // For simplicity, skip verification

        // Create resolver and WebSocket stream
        tcp::resolver resolver(*ioc_);
        ws_ = std::make_unique<websocket::stream<beast::ssl_stream<tcp::socket>>>(*ioc_, *ctx_);

        // Set SNI Hostname
        if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host.c_str())) {
            beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
            std::cerr << "SSL SNI error: " << ec.message() << std::endl;
            return false;
        }

        // Look up the domain name
        auto const results = resolver.resolve(host, port);

        // Make the connection
        auto ep = net::connect(beast::get_lowest_layer(*ws_), results);

        // Perform SSL handshake
        ws_->next_layer().handshake(ssl::stream_base::client);

        // Set WebSocket options
        ws_->set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(http::field::user_agent, "TickPlant/1.0");
            }));

        // Perform WebSocket handshake
        ws_->handshake(host, target);

        connected_ = true;
        std::cout << "Coinbase WebSocket connected successfully!" << std::endl;

        // Send subscription message
        send_subscribe_message(symbols);

        // Start reading thread
        ws_thread_ = std::thread(&CoinbaseWebSocketClient::run_client, this);

        return true;

    } catch (const std::exception& e) {
        std::cerr << "Coinbase connection exception: " << e.what() << std::endl;
        connected_ = false;
        return false;
    }
}

void CoinbaseWebSocketClient::disconnect() {
    if (!connected_ && !ws_thread_.joinable()) {
        return;
    }

    should_stop_ = true;
    connected_ = false;

    try {
        if (ws_ && ws_->is_open()) {
            beast::error_code ec;
            ws_->close(websocket::close_code::normal, ec);
            if (ec) {
                std::cerr << "Coinbase close error: " << ec.message() << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Coinbase disconnect exception: " << e.what() << std::endl;
    }

    if (ioc_) {
        ioc_->stop();
    }

    if (ws_thread_.joinable()) {
        ws_thread_.join();
    }
}

void CoinbaseWebSocketClient::set_message_callback(MessageCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    message_callback_ = callback;
}

void CoinbaseWebSocketClient::set_depth_callback(DepthCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    depth_callback_ = callback;
}

void CoinbaseWebSocketClient::send_subscribe_message(const std::vector<std::string>& symbols) {
    try {
        // Convert Binance symbols to Coinbase product_id format
        std::vector<std::string> coinbase_symbols;
        for (const auto& symbol : symbols) {
            coinbase_symbols.push_back(binance_to_coinbase_symbol(symbol));
        }

        // Subscribe to level2 channel for full L2 depth (snapshot + incremental updates)
        json subscribe_msg = {
            {"type", "subscribe"},
            {"product_ids", coinbase_symbols},
            {"channel", "level2"}
        };

        std::string msg_str = subscribe_msg.dump();
        std::cout << "Sending Coinbase subscription: " << msg_str << std::endl;

        ws_->write(net::buffer(msg_str));

    } catch (const std::exception& e) {
        std::cerr << "Failed to send Coinbase subscription: " << e.what() << std::endl;
    }
}

void CoinbaseWebSocketClient::run_client() {
    thread_affinity::set_thread_affinity(thread_affinity::TAG_COINBASE_WS);

    try {
        while (!should_stop_ && connected_) {
            do_read();
        }
    } catch (const std::exception& e) {
        std::cerr << "Coinbase WebSocket read error: " << e.what() << std::endl;
        connected_ = false;
    }
}

void CoinbaseWebSocketClient::do_read() {
    try {
        buffer_.clear();
        beast::error_code ec;
        ws_->read(buffer_, ec);

        if (ec) {
            if (ec != websocket::error::closed) {
                std::cerr << "Coinbase read error: " << ec.message() << std::endl;
            }
            connected_ = false;
            return;
        }

        on_read(ec, buffer_.size());

    } catch (const std::exception& e) {
        std::cerr << "Coinbase read exception: " << e.what() << std::endl;
        connected_ = false;
    }
}

void CoinbaseWebSocketClient::on_read(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec) {
        return;
    }

    message_count_++;

    try {
        std::string message = beast::buffers_to_string(buffer_.data());
        parse_depth_message(message);
    } catch (const std::exception& e) {
        std::cerr << "Coinbase message parsing error: " << e.what() << std::endl;
    }
}

void CoinbaseWebSocketClient::parse_depth_message(const std::string& message) {
    try {
        auto j = json::parse(message);

        // Subscription confirmation: {"channel":"subscriptions",...} or
        // events[0]["type"] == "subscribed"
        if (j.contains("channel") && j["channel"] == "subscriptions") {
            std::cout << "Coinbase subscription confirmed" << std::endl;
            return;
        }

        // level2 data arrives as channel "l2_data" (Coinbase renames on the wire)
        if (!j.contains("events")) return;

        auto events = j["events"];
        if (events.empty()) return;

        auto now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        for (const auto& event : events) {
            if (!event.contains("type")) continue;
            std::string event_type = event["type"].get<std::string>();

            // Subscription confirmation embedded in events array
            if (event_type == "subscribed") {
                std::cout << "Coinbase level2 subscription confirmed" << std::endl;
                continue;
            }

            // "snapshot" = full book replace, "update" = incremental delta
            bool is_snap = (event_type == "snapshot");
            if (!is_snap && event_type != "update") continue;

            if (!event.contains("product_id") || !event.contains("updates")) continue;

            std::string product_id = event["product_id"].get<std::string>();

            // ── Build OrderBookSnapshot from the updates array ───────────────
            // Each update: {"side":"bid"|"offer","price_level":"65000","new_quantity":"0.5"}
            // new_quantity "0" means delete.
            OrderBookSnapshot snap;
            snap.symbol       = product_id;
            snap.exchange     = "Coinbase";
            snap.timestamp_ms = now_ms;
            snap.is_snapshot  = is_snap;

            for (const auto& upd : event["updates"]) {
                if (!upd.contains("side") || !upd.contains("price_level") ||
                    !upd.contains("new_quantity")) continue;

                double price = std::stod(upd["price_level"].get<std::string>());
                double qty   = std::stod(upd["new_quantity"].get<std::string>());
                std::string side = upd["side"].get<std::string>();

                if (side == "bid") {
                    snap.bids.push_back({price, qty, 0});
                } else if (side == "offer") {
                    snap.asks.push_back({price, qty, 0});
                }
            }

            // Sort bids descending, asks ascending (Coinbase updates are unordered)
            std::sort(snap.bids.begin(), snap.bids.end(),
                [](const PriceLevel& a, const PriceLevel& b){ return a.price > b.price; });
            std::sort(snap.asks.begin(), snap.asks.end(),
                [](const PriceLevel& a, const PriceLevel& b){ return a.price < b.price; });

            // ── Maintain client-side local book for BBO extraction ───────────
            auto it = local_books_.find(product_id);
            if (it == local_books_.end()) {
                local_books_.emplace(product_id,
                    std::make_unique<OrderBook>(product_id, "Coinbase"));
                it = local_books_.find(product_id);
            }
            OrderBook& book = *it->second;

            if (is_snap) {
                book.clear();
                for (const auto& lvl : snap.bids)
                    if (lvl.quantity > 0.0)
                        book.set_level(OrderBook::Side::Bid, lvl.price, lvl.quantity, lvl.order_count);
                for (const auto& lvl : snap.asks)
                    if (lvl.quantity > 0.0)
                        book.set_level(OrderBook::Side::Ask, lvl.price, lvl.quantity, lvl.order_count);
            } else {
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

            // ── Fire callbacks ───────────────────────────────────────────────
            std::lock_guard<std::mutex> lock(callback_mutex_);

            // L2 depth callback — feeds ws_books_ in ArbitrageEngine
            if (depth_callback_ && !snap.empty()) {
                depth_callback_(snap);
            }

            // BBO callback — backward-compat path (retired in Phase 2.6)
            if (message_callback_) {
                auto bbo = book.get_snapshot();
                if (!bbo.empty()) {
                    TickerData ticker;
                    ticker.symbol       = product_id;
                    ticker.exchange     = "Coinbase";
                    ticker.timestamp_ms = now_ms;
                    ticker.bid_price    = bbo.best_bid();
                    ticker.ask_price    = bbo.best_ask();
                    ticker.bid_quantity = bbo.bids.empty() ? 0.0 : bbo.bids.front().quantity;
                    ticker.ask_quantity = bbo.asks.empty() ? 0.0 : bbo.asks.front().quantity;
                    message_callback_(ticker);
                }
            }
        }

    } catch (const json::exception& e) {
        std::cerr << "Coinbase JSON parsing error: " << e.what() << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Coinbase depth parsing error: " << e.what() << std::endl;
    }
}

std::string CoinbaseWebSocketClient::binance_to_coinbase_symbol(const std::string& symbol) {
    // Convert "BTCUSDT" to "BTC-USD" (Coinbase product_id format)
    if (symbol == "BTCUSDT")   return "BTC-USD";
    if (symbol == "ETHUSDT")   return "ETH-USD";
    if (symbol == "BNBUSDT")   return "BNB-USD";
    if (symbol == "ADAUSDT")   return "ADA-USD";
    if (symbol == "DOTUSDT")   return "DOT-USD";
    if (symbol == "SOLUSDT")   return "SOL-USD";
    if (symbol == "MATICUSDT") return "MATIC-USD";
    if (symbol == "AVAXUSDT")  return "AVAX-USD";
    if (symbol == "LTCUSDT")   return "LTC-USD";
    if (symbol == "LINKUSDT")  return "LINK-USD";
    if (symbol == "XLMUSDT")   return "XLM-USD";
    if (symbol == "XRPUSDT")   return "XRP-USD";
    if (symbol == "UNIUSDT")   return "UNI-USD";
    if (symbol == "AAVEUSDT")  return "AAVE-USD";
    if (symbol == "ATOMUSDT")  return "ATOM-USD";
    if (symbol == "ALGOUSDT")  return "ALGO-USD";

    // Generic: strip USDT, add -USD
    if (symbol.length() > 4 && symbol.substr(symbol.length() - 4) == "USDT") {
        return symbol.substr(0, symbol.length() - 4) + "-USD";
    }

    return symbol;
}
