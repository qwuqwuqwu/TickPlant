#include "kraken_client.hpp"
#include "thread_affinity.hpp"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <chrono>

KrakenWebSocketClient::KrakenWebSocketClient()
    : connected_(false)
    , should_stop_(false)
    , message_count_(0) {
}

KrakenWebSocketClient::~KrakenWebSocketClient() {
    disconnect();
}

bool KrakenWebSocketClient::connect(const std::vector<std::string>& symbols) {
    if (connected_) {
        std::cerr << "Already connected!" << std::endl;
        return false;
    }

    subscribed_symbols_ = symbols;

    try {
        std::string host = "ws.kraken.com";
        std::string port = "443";
        std::string target = "/v2";

        std::cout << "Connecting to Kraken: wss://" << host << target << std::endl;

        // Initialize IO context and SSL context
        ioc_ = std::make_unique<net::io_context>();
        ctx_ = std::make_unique<ssl::context>(ssl::context::tlsv12_client);

        // Load root certificates and configure SSL
        ctx_->set_default_verify_paths();
        ctx_->set_verify_mode(ssl::verify_none); // For simplicity, skip verification
        ctx_->set_options(ssl::context::default_workarounds |
                         ssl::context::no_sslv2 |
                         ssl::context::no_sslv3 |
                         ssl::context::single_dh_use);

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
        beast::error_code ec;
        ws_->next_layer().handshake(ssl::stream_base::client, ec);
        if (ec) {
            std::cerr << "Kraken SSL handshake failed: " << ec.message() << std::endl;
            return false;
        }

        // Set WebSocket options
        ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        ws_->set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(http::field::user_agent, "Mozilla/5.0");
            }));

        // Perform WebSocket handshake
        ws_->handshake(host, target, ec);
        if (ec) {
            std::cerr << "Kraken WebSocket handshake failed: " << ec.message() << std::endl;
            return false;
        }

        connected_ = true;
        std::cout << "Kraken WebSocket connected successfully!" << std::endl;

        // Send subscription message
        send_subscribe_message(symbols);

        // Start reading thread
        ws_thread_ = std::thread(&KrakenWebSocketClient::run_client, this);

        return true;

    } catch (const std::exception& e) {
        std::cerr << "Kraken connection exception: " << e.what() << std::endl;
        connected_ = false;
        return false;
    }
}

void KrakenWebSocketClient::disconnect() {
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
                std::cerr << "Kraken close error: " << ec.message() << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Kraken disconnect exception: " << e.what() << std::endl;
    }

    if (ioc_) {
        ioc_->stop();
    }

    if (ws_thread_.joinable()) {
        ws_thread_.join();
    }
}

void KrakenWebSocketClient::set_message_callback(MessageCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    message_callback_ = callback;
}

void KrakenWebSocketClient::set_depth_callback(DepthCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    depth_callback_ = callback;
}

void KrakenWebSocketClient::send_subscribe_message(const std::vector<std::string>& symbols) {
    try {
        // Convert Binance symbols to Kraken format
        std::vector<std::string> kraken_symbols;
        for (const auto& symbol : symbols) {
            kraken_symbols.push_back(binance_to_kraken_symbol(symbol));
        }

        // Subscribe to book channel — depth=10 gives top-10 levels per side.
        // Kraken v2 sends one snapshot per symbol then incremental updates.
        json subscribe_msg = {
            {"method", "subscribe"},
            {"params", {
                {"channel", "book"},
                {"symbol", kraken_symbols},
                {"depth", 10}
            }}
        };

        std::string msg_str = subscribe_msg.dump();
        std::cout << "Sending Kraken subscription: " << msg_str << std::endl;

        ws_->write(net::buffer(msg_str));

    } catch (const std::exception& e) {
        std::cerr << "Failed to send Kraken subscription: " << e.what() << std::endl;
    }
}

void KrakenWebSocketClient::run_client() {
    thread_affinity::set_thread_affinity(thread_affinity::TAG_KRAKEN_WS);

    try {
        while (!should_stop_ && connected_) {
            do_read();
        }
    } catch (const std::exception& e) {
        std::cerr << "Kraken WebSocket read error: " << e.what() << std::endl;
        connected_ = false;
    }
}

void KrakenWebSocketClient::do_read() {
    try {
        buffer_.clear();
        beast::error_code ec;
        ws_->read(buffer_, ec);

        if (ec) {
            if (ec != websocket::error::closed) {
                std::cerr << "Kraken read error: " << ec.message() << std::endl;
            }
            connected_ = false;
            return;
        }

        on_read(ec, buffer_.size());

    } catch (const std::exception& e) {
        std::cerr << "Kraken read exception: " << e.what() << std::endl;
        connected_ = false;
    }
}

void KrakenWebSocketClient::on_read(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec) {
        return;
    }

    message_count_++;

    try {
        std::string message = beast::buffers_to_string(buffer_.data());
        parse_depth_message(message);
    } catch (const std::exception& e) {
        std::cerr << "Kraken message parsing error: " << e.what() << std::endl;
    }
}

void KrakenWebSocketClient::parse_depth_message(const std::string& message) {
    try {
        auto j = json::parse(message);

        // Subscription confirmation: {"method":"subscribe","success":true,...}
        if (j.contains("method") && j["method"] == "subscribe") {
            if (j.contains("success") && j["success"] == true) {
                std::cout << "Kraken subscription confirmed" << std::endl;
            }
            return;
        }

        // Heartbeat / status messages
        if (j.contains("channel") && j["channel"] == "status") return;
        if (j.contains("method") && j["method"] == "pong")      return;

        // Must be a book channel message
        if (!j.contains("channel") || j["channel"] != "book") return;
        if (!j.contains("type") || !j.contains("data"))       return;

        // "snapshot" = full book, "update" = incremental delta (qty==0.0 → delete)
        std::string msg_type = j["type"].get<std::string>();
        bool is_snap = (msg_type == "snapshot");
        if (!is_snap && msg_type != "update") return;

        auto data_array = j["data"];
        if (data_array.empty()) return;

        auto now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        for (const auto& item : data_array) {
            if (!item.contains("symbol")) continue;
            std::string sym = item["symbol"].get<std::string>();

            // ── Build OrderBookSnapshot ──────────────────────────────────────
            // Kraken book levels: [{"price":65000.0,"qty":0.5},...] (numbers, not strings)
            // qty == 0.0 signals delete for updates.
            OrderBookSnapshot snap;
            snap.symbol       = sym;
            snap.exchange     = "Kraken";
            snap.timestamp_ms = now_ms;
            snap.is_snapshot  = is_snap;

            if (item.contains("bids")) {
                for (const auto& lvl : item["bids"]) {
                    double price = lvl["price"].get<double>();
                    double qty   = lvl["qty"].get<double>();
                    snap.bids.push_back({price, qty, 0});
                }
            }
            if (item.contains("asks")) {
                for (const auto& lvl : item["asks"]) {
                    double price = lvl["price"].get<double>();
                    double qty   = lvl["qty"].get<double>();
                    snap.asks.push_back({price, qty, 0});
                }
            }

            // Kraken sends bids descending and asks ascending in snapshots, but
            // updates may be in any order — sort to be safe.
            std::sort(snap.bids.begin(), snap.bids.end(),
                [](const PriceLevel& a, const PriceLevel& b){ return a.price > b.price; });
            std::sort(snap.asks.begin(), snap.asks.end(),
                [](const PriceLevel& a, const PriceLevel& b){ return a.price < b.price; });

            // ── Maintain client-side local book for BBO extraction ───────────
            auto it = local_books_.find(sym);
            if (it == local_books_.end()) {
                local_books_.emplace(sym, std::make_unique<OrderBook>(sym, "Kraken"));
                it = local_books_.find(sym);
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
                    ticker.symbol       = sym;
                    ticker.exchange     = "Kraken";
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
        std::cerr << "Kraken JSON parsing error: " << e.what() << std::endl;
        std::cerr << "Message: " << message << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Kraken depth parsing error: " << e.what() << std::endl;
    }
}

std::string KrakenWebSocketClient::binance_to_kraken_symbol(const std::string& symbol) {
    // Convert "BTCUSDT" to "BTC/USD" (Kraken symbol format)
    if (symbol == "BTCUSDT")   return "BTC/USD";
    if (symbol == "ETHUSDT")   return "ETH/USD";
    if (symbol == "ADAUSDT")   return "ADA/USD";
    if (symbol == "DOTUSDT")   return "DOT/USD";
    if (symbol == "SOLUSDT")   return "SOL/USD";
    if (symbol == "MATICUSDT") return "MATIC/USD";
    if (symbol == "AVAXUSDT")  return "AVAX/USD";
    if (symbol == "LTCUSDT")   return "LTC/USD";
    if (symbol == "LINKUSDT")  return "LINK/USD";
    if (symbol == "XLMUSDT")   return "XLM/USD";
    if (symbol == "XRPUSDT")   return "XRP/USD";
    if (symbol == "UNIUSDT")   return "UNI/USD";
    if (symbol == "AAVEUSDT")  return "AAVE/USD";
    if (symbol == "ATOMUSDT")  return "ATOM/USD";
    if (symbol == "ALGOUSDT")  return "ALGO/USD";

    // Generic: strip USDT, add /USD
    if (symbol.length() > 4 && symbol.substr(symbol.length() - 4) == "USDT") {
        return symbol.substr(0, symbol.length() - 4) + "/USD";
    }

    return symbol;
}
