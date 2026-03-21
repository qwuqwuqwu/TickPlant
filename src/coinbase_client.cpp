#include "coinbase_client.hpp"
#include "thread_affinity.hpp"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <random>

// ─── Constructor / Destructor ─────────────────────────────────────────────────

CoinbaseWebSocketClient::CoinbaseWebSocketClient()
    : connected_(false)
    , should_stop_(false)
    , message_count_(0) {
}

CoinbaseWebSocketClient::~CoinbaseWebSocketClient() {
    disconnect();
}

// ─── Credentials ─────────────────────────────────────────────────────────────

void CoinbaseWebSocketClient::set_credentials(const std::string& key_name,
                                               const std::string& private_key_pem) {
    api_key_name_    = key_name;
    api_private_key_ = private_key_pem;
}

// ─── Connect ─────────────────────────────────────────────────────────────────

bool CoinbaseWebSocketClient::connect(const std::vector<std::string>& symbols) {
    if (connected_) {
        std::cerr << "Coinbase: already connected\n";
        return false;
    }

    // Auto-load credentials from environment (populated by load_dotenv())
    if (api_key_name_.empty()) {
        const char* k = std::getenv("COINBASE_API_KEY_NAME");
        const char* p = std::getenv("COINBASE_API_PRIVATE_KEY");
        if (k) api_key_name_    = k;
        if (p) api_private_key_ = p;
    }

    subscribed_symbols_ = symbols;

    try {
        // Coinbase Advanced Trade WebSocket — requires CDP Secret API Key JWT auth
        std::string host   = "advanced-trade-ws.coinbase.com";
        std::string port   = "443";
        std::string target = "/";

        std::cout << "Connecting to Coinbase: wss://" << host << target << "\n";

        ioc_ = std::make_unique<net::io_context>();
        ctx_ = std::make_unique<ssl::context>(ssl::context::tlsv12_client);
        ctx_->set_default_verify_paths();
        ctx_->set_verify_mode(ssl::verify_none);

        tcp::resolver resolver(*ioc_);
        ws_ = std::make_unique<websocket::stream<beast::ssl_stream<tcp::socket>>>(*ioc_, *ctx_);

        if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host.c_str())) {
            beast::error_code ec{static_cast<int>(::ERR_get_error()),
                                 net::error::get_ssl_category()};
            std::cerr << "Coinbase SSL SNI error: " << ec.message() << "\n";
            return false;
        }

        auto const results = resolver.resolve(host, port);
        net::connect(beast::get_lowest_layer(*ws_), results);
        ws_->next_layer().handshake(ssl::stream_base::client);

        ws_->set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(http::field::user_agent, "TickPlant/1.0");
            }));

        ws_->handshake(host, target);
        connected_ = true;
        std::cout << "Coinbase WebSocket connected successfully!\n";

        send_subscribe_message(symbols);

        ws_thread_ = std::thread(&CoinbaseWebSocketClient::run_client, this);
        return true;

    } catch (const std::exception& e) {
        std::cerr << "Coinbase connection error: " << e.what() << "\n";
        connected_ = false;
        return false;
    }
}

// ─── Disconnect ───────────────────────────────────────────────────────────────

void CoinbaseWebSocketClient::disconnect() {
    if (!connected_ && !ws_thread_.joinable()) return;

    should_stop_ = true;
    connected_   = false;

    try {
        if (ws_ && ws_->is_open()) {
            beast::error_code ec;
            ws_->close(websocket::close_code::normal, ec);
        }
    } catch (...) {}

    if (ioc_) ioc_->stop();
    if (ws_thread_.joinable()) ws_thread_.join();
}

// ─── Callbacks ────────────────────────────────────────────────────────────────

void CoinbaseWebSocketClient::set_message_callback(MessageCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    message_callback_ = callback;
}

void CoinbaseWebSocketClient::set_depth_callback(DepthCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    depth_callback_ = callback;
}

// ─── Subscription ─────────────────────────────────────────────────────────────

void CoinbaseWebSocketClient::send_subscribe_message(const std::vector<std::string>& symbols) {
    try {
        std::vector<std::string> coinbase_symbols;
        for (const auto& s : symbols)
            coinbase_symbols.push_back(binance_to_coinbase_symbol(s));

        // Advanced Trade format: "channel" is a singular string, not an array
        json msg = {
            {"type",        "subscribe"},
            {"product_ids", coinbase_symbols},
            {"channel",     "level2"}
        };

        std::string jwt = make_jwt();
        if (!jwt.empty()) {
            msg["jwt"] = jwt;
            std::cout << "Coinbase: JWT attached (key=" << api_key_name_ << ")\n";
        } else {
            std::cout << "Coinbase: no credentials — level2 will be rejected.\n"
                      << "          Fill COINBASE_API_KEY_NAME and COINBASE_API_PRIVATE_KEY in .env\n";
        }

        std::string s = msg.dump();
        std::cout << "Sending Coinbase subscription: " << s << "\n";
        ws_->write(net::buffer(s));

    } catch (const std::exception& e) {
        std::cerr << "Coinbase subscribe error: " << e.what() << "\n";
    }
}

// ─── Read loop ────────────────────────────────────────────────────────────────

void CoinbaseWebSocketClient::run_client() {
    thread_affinity::set_thread_affinity(thread_affinity::TAG_COINBASE_WS);
    try {
        while (!should_stop_ && connected_) do_read();
    } catch (const std::exception& e) {
        std::cerr << "Coinbase read error: " << e.what() << "\n";
        connected_ = false;
    }
}

void CoinbaseWebSocketClient::do_read() {
    try {
        buffer_.clear();
        beast::error_code ec;
        ws_->read(buffer_, ec);
        if (ec) {
            if (ec != websocket::error::closed)
                std::cerr << "Coinbase read: " << ec.message() << "\n";
            connected_ = false;
            return;
        }
        on_read(ec, buffer_.size());
    } catch (const std::exception& e) {
        std::cerr << "Coinbase do_read exception: " << e.what() << "\n";
        connected_ = false;
    }
}

void CoinbaseWebSocketClient::on_read(beast::error_code ec, std::size_t) {
    if (ec) return;
    message_count_++;
    try {
        parse_depth_message(beast::buffers_to_string(buffer_.data()));
    } catch (const std::exception& e) {
        std::cerr << "Coinbase parse error: " << e.what() << "\n";
    }
}

// ─── Message parser (Advanced Trade wire format) ─────────────────────────────
//
// Top-level envelope:
//   {"channel":"l2_data", "events":[...], "timestamp":"...", "sequence_num":N}
//
// Event (inside "events" array):
//   {"type":"snapshot"|"update", "product_id":"BTC-USD",
//    "updates":[{"side":"bid"|"offer","price_level":"65000","new_quantity":"1.5"},...]}
//
// Subscription confirmation:
//   {"channel":"subscriptions", ...}

void CoinbaseWebSocketClient::parse_depth_message(const std::string& message) {
    auto j = json::parse(message);

    // Log any top-level error frames (no "channel" field) so they are visible.
    if (!j.contains("channel")) {
        std::cerr << "Coinbase server msg: " << message.substr(0, 300) << "\n";
        return;
    }
    std::string channel = j["channel"].get<std::string>();

    if (channel == "subscriptions") {
        std::cout << "Coinbase level2 subscription confirmed\n";
        return;
    }

    // Heartbeat sent by the server — no book data
    if (channel == "heartbeats") return;

    // Error frame
    if (channel == "error" || (j.contains("type") && j["type"] == "error")) {
        std::cerr << "Coinbase error: " << j.dump() << "\n";
        return;
    }

    if (channel != "l2_data") return;
    if (!j.contains("events")) return;

    auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    for (const auto& event : j["events"])
        process_event(event, now_ms);
}

void CoinbaseWebSocketClient::process_event(const json& event, uint64_t now_ms) {
    if (!event.contains("type") || !event.contains("product_id")) return;

    std::string event_type = event["type"].get<std::string>();
    bool is_snap = (event_type == "snapshot");
    if (!is_snap && event_type != "update") return;

    if (!event.contains("updates")) return;
    std::string product_id = event["product_id"].get<std::string>();

    // ── Build OrderBookSnapshot ───────────────────────────────────────────────
    // Each update: {"side":"bid"|"offer","price_level":"65000","new_quantity":"1.5"}
    // new_quantity "0" means delete the level.
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

        if (side == "bid")
            snap.bids.push_back({price, qty, 0});
        else if (side == "offer")
            snap.asks.push_back({price, qty, 0});
    }

    // Sort bids desc, asks asc (Advanced Trade sends unordered)
    std::sort(snap.bids.begin(), snap.bids.end(),
        [](const PriceLevel& a, const PriceLevel& b){ return a.price > b.price; });
    std::sort(snap.asks.begin(), snap.asks.end(),
        [](const PriceLevel& a, const PriceLevel& b){ return a.price < b.price; });

    // ── Maintain local book for BBO extraction ────────────────────────────────
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
        // local_books_ entries are only created during snapshot processing,
        // so if we reached here the book already has a valid baseline.
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

    fire_callbacks(product_id, snap, book, now_ms);
}

void CoinbaseWebSocketClient::fire_callbacks(const std::string& product_id,
                                              const OrderBookSnapshot& snap,
                                              OrderBook& book,
                                              uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(callback_mutex_);

    if (depth_callback_ && !snap.empty())
        depth_callback_(snap);

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

// ─── Symbol conversion ────────────────────────────────────────────────────────

std::string CoinbaseWebSocketClient::binance_to_coinbase_symbol(const std::string& symbol) {
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
    if (symbol.size() > 4 && symbol.substr(symbol.size() - 4) == "USDT")
        return symbol.substr(0, symbol.size() - 4) + "-USD";
    return symbol;
}

// ─── JWT ES256 ────────────────────────────────────────────────────────────────

// base64url: RFC 4648 §5 — no padding, '+' → '-', '/' → '_'
std::string CoinbaseWebSocketClient::base64url_encode(const unsigned char* data, size_t len) {
    // Use EVP_EncodeBlock then transform
    size_t b64_len = ((len + 2) / 3) * 4 + 1;
    std::string b64(b64_len, '\0');
    int written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(b64.data()), data, static_cast<int>(len));
    b64.resize(static_cast<size_t>(written));

    // Strip padding and convert to URL-safe alphabet
    while (!b64.empty() && b64.back() == '=') b64.pop_back();
    for (char& c : b64) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    return b64;
}

std::vector<unsigned char> CoinbaseWebSocketClient::base64url_decode(const std::string& in) {
    // Convert URL-safe alphabet back to standard base64 and add padding
    std::string b64 = in;
    for (char& c : b64) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (b64.size() % 4 != 0) b64 += '=';

    std::vector<unsigned char> out((b64.size() / 4) * 3 + 3);
    int decoded = EVP_DecodeBlock(
        out.data(),
        reinterpret_cast<const unsigned char*>(b64.data()),
        static_cast<int>(b64.size()));
    if (decoded < 0) return {};

    // Remove padding bytes
    size_t padding = 0;
    if (b64.size() >= 1 && b64[b64.size()-1] == '=') padding++;
    if (b64.size() >= 2 && b64[b64.size()-2] == '=') padding++;
    out.resize(static_cast<size_t>(decoded) - padding);
    return out;
}

// make_jwt: builds a signed JWT valid for 120 s.
//
// Header:  {"alg":"EdDSA","kid":"<key_name>"}
// Payload: {"sub":"<key_name>","iss":"cdp","nbf":<now>,"exp":<now+120>,
//           "aud":["public_websocket_api"]}
// Signing: Ed25519 (RFC 8037).  Signature is always 64 raw bytes — no DER
//          conversion needed unlike ECDSA.
//
// Key formats accepted:
//   • PKCS#8 PEM  (-----BEGIN PRIVATE KEY-----)
//   • Raw base64/base64url — 32-byte seed alone, OR 64-byte seed||pubkey
//     (CDP portal concatenates seed + public key into one base64 blob)
std::string CoinbaseWebSocketClient::make_jwt() const {
    if (api_key_name_.empty() || api_private_key_.empty()) return "";

    // ── 1. Build header and payload ───────────────────────────────────────────
    // Generate a random nonce (hex string) — required by the Coinbase server
    // to prevent JWT replay detection from rejecting repeated connections.
    std::string nonce;
    {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<uint64_t> dis;
        uint64_t n1 = dis(gen), n2 = dis(gen);
        char buf[33];
        std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                      static_cast<unsigned long long>(n1),
                      static_cast<unsigned long long>(n2));
        nonce = buf;
    }

    json header_j  = {{"alg", "EdDSA"}, {"kid", api_key_name_}, {"nonce", nonce}};
    std::time_t now = std::time(nullptr);
    json payload_j = {
        {"sub", api_key_name_},
        {"iss", "cdp"},
        {"nbf", static_cast<long>(now)},
        {"exp", static_cast<long>(now + 120)},
        {"aud", json::array({"public_websocket_api"})}
    };

    std::string header_str  = header_j.dump();
    std::string payload_str = payload_j.dump();

    std::string h = base64url_encode(
        reinterpret_cast<const unsigned char*>(header_str.data()),  header_str.size());
    std::string p = base64url_encode(
        reinterpret_cast<const unsigned char*>(payload_str.data()), payload_str.size());

    std::string signing_input = h + "." + p;

    // ── 2. Load Ed25519 private key ───────────────────────────────────────────
    EVP_PKEY* pkey = nullptr;

    // Normalise literal "\n" (two chars: backslash + n) to real newlines.
    // .env files typically store multi-line PEM keys as a single line with
    // escaped newlines, e.g.  "-----BEGIN PRIVATE KEY-----\nMEE...\n-----END..."
    // OpenSSL's PEM parser requires actual newline characters.
    std::string key_str = api_private_key_;
    {
        std::string normalised;
        normalised.reserve(key_str.size());
        for (size_t i = 0; i < key_str.size(); ++i) {
            if (key_str[i] == '\\' && i + 1 < key_str.size() && key_str[i+1] == 'n') {
                normalised += '\n';
                ++i;
            } else {
                normalised += key_str[i];
            }
        }
        key_str = std::move(normalised);
    }

    // Path A: PEM (PKCS#8 "-----BEGIN PRIVATE KEY-----" or "-----BEGIN EC PRIVATE KEY-----")
    {
        BIO* bio = BIO_new_mem_buf(key_str.data(),
                                   static_cast<int>(key_str.size()));
        pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
    }

    // Path B: raw base64 / base64url blob.
    // CDP portal gives seed(32) || pubkey(32) = 64 bytes base64-encoded.
    // EVP_PKEY_new_raw_private_key needs only the 32-byte seed.
    if (!pkey) {
        // Normalise to standard base64 with padding, then decode via BIO
        // (BIO is more reliable than EVP_DecodeBlock across OpenSSL versions).
        // If the normalised string looks like PEM, strip the headers first so
        // only the raw base64 body remains.
        std::string b64 = key_str;
        if (b64.find("-----BEGIN") != std::string::npos) {
            // Strip everything up to and including the first "-----\n" line
            // and everything from the "-----END" line onward.
            std::string body;
            bool in_body = false;
            std::istringstream ss(b64);
            std::string ln;
            while (std::getline(ss, ln)) {
                if (!in_body) {
                    if (ln.find("-----BEGIN") != std::string::npos) in_body = true;
                    continue;
                }
                if (ln.find("-----END") != std::string::npos) break;
                // Strip carriage return in case of \r\n
                if (!ln.empty() && ln.back() == '\r') ln.pop_back();
                body += ln;
            }
            b64 = body;
        }
        for (char& c : b64) {          // base64url → standard base64
            if (c == '-') c = '+';
            else if (c == '_') c = '/';
        }
        while (b64.size() % 4 != 0) b64 += '=';

        std::vector<unsigned char> raw(b64.size());   // upper bound
        BIO* b64_bio = BIO_new(BIO_f_base64());
        BIO* mem_bio = BIO_new_mem_buf(b64.data(), static_cast<int>(b64.size()));
        BIO_push(b64_bio, mem_bio);
        BIO_set_flags(b64_bio, BIO_FLAGS_BASE64_NO_NL);
        int decoded = BIO_read(b64_bio, raw.data(), static_cast<int>(raw.size()));
        BIO_free_all(b64_bio);

        if (decoded >= 32) {
            raw.resize(static_cast<size_t>(decoded));
            // Use only the first 32 bytes — the Ed25519 private seed.
            // CDP bundles seed(32)||pubkey(32) together; we only need the seed.
            pkey = EVP_PKEY_new_raw_private_key(
                       EVP_PKEY_ED25519, nullptr, raw.data(), 32);
            if (!pkey)
                std::cerr << "Coinbase JWT: EVP_PKEY_new_raw_private_key failed\n";
        } else {
            std::cerr << "Coinbase JWT: decoded only " << decoded
                      << " bytes — key is too short (need ≥32)\n";
        }
    }

    if (!pkey) {
        std::cerr << "Coinbase JWT: failed to load Ed25519 private key — "
                     "check COINBASE_API_PRIVATE_KEY in .env\n";
        return "";
    }

    // ── 3. Sign with Ed25519 ──────────────────────────────────────────────────
    // Ed25519 uses nullptr for the digest (hashing is internal to the algorithm).
    // The signature is always exactly 64 bytes — no DER conversion required.
    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        std::cerr << "Coinbase JWT: EVP_MD_CTX_new failed\n";
        EVP_PKEY_free(pkey);
        return "";
    }

    if (EVP_DigestSignInit(md_ctx, nullptr, /*md=*/nullptr, nullptr, pkey) != 1) {
        std::cerr << "Coinbase JWT: EVP_DigestSignInit failed\n";
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        return "";
    }

    size_t sig_len = 64;
    unsigned char sig[64];
    if (EVP_DigestSign(md_ctx,
                       sig, &sig_len,
                       reinterpret_cast<const unsigned char*>(signing_input.data()),
                       signing_input.size()) != 1) {
        std::cerr << "Coinbase JWT: EVP_DigestSign failed\n";
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        return "";
    }

    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);

    return signing_input + "." + base64url_encode(sig, sig_len);
}
