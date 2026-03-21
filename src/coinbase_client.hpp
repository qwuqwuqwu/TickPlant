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

class CoinbaseWebSocketClient {
public:
    using MessageCallback = std::function<void(const TickerData&)>;
    using DepthCallback   = std::function<void(const OrderBookSnapshot&)>;

    CoinbaseWebSocketClient();
    ~CoinbaseWebSocketClient();

    // Connect to Coinbase Advanced Trade WebSocket and subscribe to level2 channel
    bool connect(const std::vector<std::string>& symbols);

    // Disconnect and cleanup
    void disconnect();

    // BBO callback — fires on every snapshot and update with current best bid/ask.
    void set_message_callback(MessageCallback callback);

    // L2 depth callback — fires on every snapshot (is_snapshot=true) and update
    // (is_snapshot=false, qty==0.0 means delete).
    void set_depth_callback(DepthCallback callback);

    // Set CDP Secret API Key credentials.  Call before connect(), or let
    // connect() auto-load from COINBASE_API_KEY_NAME / COINBASE_API_PRIVATE_KEY
    // environment variables (populated from .env by load_dotenv()).
    void set_credentials(const std::string& key_name,
                         const std::string& private_key_pem);

    bool     is_connected()     const { return connected_; }
    uint64_t get_message_count() const { return message_count_; }

private:
    std::unique_ptr<net::io_context> ioc_;
    std::unique_ptr<ssl::context>    ctx_;
    std::unique_ptr<websocket::stream<beast::ssl_stream<tcp::socket>>> ws_;

    std::thread            ws_thread_;
    std::atomic<bool>      connected_;
    std::atomic<bool>      should_stop_;
    std::atomic<uint64_t>  message_count_;

    MessageCallback message_callback_;
    DepthCallback   depth_callback_;
    std::mutex      callback_mutex_;

    std::vector<std::string> subscribed_symbols_;
    beast::flat_buffer       buffer_;

    // CDP Secret API Key credentials
    std::string api_key_name_;     // e.g. "c27bde3a-7452-4f46-b9be-f020d37fa3d9"
    std::string api_private_key_;  // EC private key — PEM or raw base64

    // Client-side L2 books keyed by product_id (e.g. "BTC-USD")
    std::unordered_map<std::string, std::unique_ptr<OrderBook>> local_books_;

    // WebSocket I/O
    void run_client();
    void do_read();
    void on_read(beast::error_code ec, std::size_t bytes_transferred);

    // Message handling
    void parse_depth_message(const std::string& message);
    void process_event(const json& event, uint64_t now_ms);
    void fire_callbacks(const std::string& product_id,
                        const OrderBookSnapshot& snap,
                        OrderBook& book,
                        uint64_t now_ms);

    // Subscription (attaches JWT when credentials are available)
    void send_subscribe_message(const std::vector<std::string>& symbols);

    // JWT ES256 — Coinbase CDP authentication
    // Returns a signed JWT valid for 120 s, or "" if credentials are missing.
    std::string make_jwt() const;

    // base64url helpers (RFC 4648 §5 — no padding, '-' and '_')
    static std::string              base64url_encode(const unsigned char* data, size_t len);
    static std::vector<unsigned char> base64url_decode(const std::string& encoded);

    // Symbol conversion ("BTCUSDT" → "BTC-USD")
    static std::string binance_to_coinbase_symbol(const std::string& symbol);
};
