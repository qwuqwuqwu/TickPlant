#pragma once

#include "types.hpp"
#include "order_book.hpp"
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;

class BybitWebSocketClient {
public:
    using MessageCallback = std::function<void(const TickerData&)>;
    using DepthCallback   = std::function<void(const OrderBookSnapshot&)>;

    BybitWebSocketClient();
    ~BybitWebSocketClient();

    // Connect to Bybit WebSocket and subscribe to orderbook.50 (L2 depth)
    bool connect(const std::vector<std::string>& symbols);

    // Disconnect and cleanup
    void disconnect();

    // BBO callback — fires on every snapshot and delta with current best bid/ask.
    // Kept for backward-compat with the BBO arbitrage path (retired in Phase 2.6).
    void set_message_callback(MessageCallback callback);

    // L2 depth callback — fires on every snapshot (is_snapshot=true) and delta
    // (is_snapshot=false, qty==0.0 means delete).  Used to maintain ws_books_ in
    // ArbitrageEngine for the pure L2 detection path introduced in Phase 2.6.
    void set_depth_callback(DepthCallback callback);

    // Check if client is connected
    bool is_connected() const { return connected_; }

    // Get connection statistics
    uint64_t get_message_count() const { return message_count_; }

private:
    std::unique_ptr<net::io_context> ioc_;
    std::unique_ptr<ssl::context> ctx_;
    std::unique_ptr<websocket::stream<beast::ssl_stream<tcp::socket>>> ws_;

    std::thread ws_thread_;
    std::atomic<bool> connected_;
    std::atomic<bool> should_stop_;
    std::atomic<uint64_t> message_count_;

    MessageCallback message_callback_;
    DepthCallback   depth_callback_;
    std::mutex callback_mutex_;

    std::vector<std::string> subscribed_symbols_;
    beast::flat_buffer buffer_;

    // Client-side L2 books — one per symbol (keyed by native Bybit symbol, e.g.
    // "BTCUSDT").  Used to derive an accurate BBO after each incremental delta so
    // the existing message_callback_ path stays live until Phase 2.6 retires it.
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> local_books_;

    // WebSocket operations
    void run_client();
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);

    // Parse Bybit orderbook.50 snapshot/delta message
    void parse_depth_message(const std::string& message);

    // Return the orderbook.50 topic string for a Binance-format symbol
    std::string binance_to_bybit_topic(const std::string& symbol);

    // Send subscription message (max 10 args per request)
    void send_subscribe_message(const std::vector<std::string>& symbols);
};
