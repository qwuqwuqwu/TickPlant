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
#include <mutex>
#include <memory>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;

class BinanceWebSocketClient {
public:
    // BBO callback — fires on every depth update with best bid/ask extracted.
    // Kept for backward compatibility with dashboard and existing BBO detection.
    using MessageCallback = std::function<void(const TickerData&)>;

    // L2 depth callback — fires on every depth update with full book snapshot.
    // Use this to maintain per-symbol OrderBooks in the ArbitrageEngine.
    using DepthCallback = std::function<void(const OrderBookSnapshot&)>;

    BinanceWebSocketClient();
    ~BinanceWebSocketClient();

    // Connect to Binance WebSocket and subscribe to @depth20@100ms streams
    bool connect(const std::vector<std::string>& symbols);

    // Disconnect and cleanup
    void disconnect();

    // BBO callback — called for every depth update (backward compat)
    void set_message_callback(MessageCallback callback);

    // L2 depth callback — called for every depth update with full snapshot
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

    // WebSocket operations
    void run_client();
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);

    // Parse Binance @depth20@100ms message.
    // Fires depth_callback_ with full OrderBookSnapshot, then fires
    // message_callback_ with BBO extracted from the snapshot.
    void parse_depth_message(const std::string& message);

    // Convert symbol to Binance @depth20@100ms stream name.
    // e.g. "BTCUSDT" -> "btcusdt@depth20@100ms"
    std::string symbol_to_stream(const std::string& symbol);
};
