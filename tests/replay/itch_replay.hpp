#pragma once

// itch_replay.hpp
//
// NASDAQ ITCH 5.0 binary message parser and L2 reconstruction engine.
//
// Purpose:
//   Provides deterministic regression testing for the OrderBook engine by
//   replaying historical (or synthetic) ITCH 5.0 data and verifying the
//   resulting L2 book state.
//
// ITCH 5.0 overview:
//   - Binary, big-endian, fixed-length messages
//   - L3 feed: every individual order event is reported
//   - Each message is preceded by a 2-byte big-endian length in file format
//   - Price field: uint32, divide by 10'000 to get dollars (e.g. 1012500 = $101.25)
//   - Timestamp: 6-byte big-endian nanoseconds since midnight
//
// L2 reconstruction:
//   ITCH is an L3 feed — it gives individual orders, not aggregated price levels.
//   This class maintains an order map (ref_num → Order) and derives L2 price
//   level aggregates, emitting an L2Event callback whenever a level changes.
//
// Relevant message types for L2 reconstruction:
//   'A' Add Order              — new resting order enters the book
//   'F' Add Order with MPID   — same, includes market participant ID
//   'E' Order Executed        — order (partially) filled at its limit price
//   'C' Order Executed w/Price — order filled at a different price
//   'X' Order Cancel          — order quantity reduced (partial cancel)
//   'D' Order Delete           — order fully removed
//   'U' Order Replace          — order price/qty changed (delete + re-add)
//
// Reference: https://www.nasdaqtrader.com/content/technicalsupport/specifications/
//            dataproducts/NQTVITCHspecification.pdf

#include "order_book.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>

namespace itch {

// ─── L2Event ──────────────────────────────────────────────────────────────────

// A single L2 price-level change derived from ITCH L3 events.
// Passed to the user callback after every order event that changes a level.
struct L2Event {
    enum class Type { Set, Delete };

    Type             type;
    OrderBook::Side  side;
    double           price;
    double           quantity;      // total quantity at this level after change
    int              order_count;   // number of resting orders at this level
    std::string      stock;         // trimmed stock symbol (e.g. "AAPL")
    uint64_t         timestamp_ns;  // nanoseconds since midnight
};

// ─── ITCHReplay ───────────────────────────────────────────────────────────────

class ITCHReplay {
public:
    // Called once per L2 level change.
    using L2Callback = std::function<void(const L2Event&)>;

    void set_callback(L2Callback cb)            { callback_ = std::move(cb); }

    // Only emit events for this stock symbol (e.g. "AAPL").
    // Empty string (default) means emit for all symbols.
    void set_symbol_filter(const std::string& s) { filter_ = s; }

    // ── Parsing ──────────────────────────────────────────────────────────

    // Parse and process a single ITCH message from a raw byte buffer.
    // `buf` points to the first byte of the message type field (NOT the
    // 2-byte length prefix — callers strip that before calling here).
    // `len` is the message body length (as reported by the prefix).
    // Returns true if the message type was recognised and handled.
    bool process_message(const uint8_t* buf, size_t len);

    // Replay all messages from an ITCH 5.0 binary file.
    // File format: repeated [2-byte big-endian length][message body].
    // Returns the number of messages successfully processed.
    // Throws std::runtime_error if the file cannot be opened.
    size_t replay_file(const std::string& path);

    // ── Statistics ────────────────────────────────────────────────────────

    uint64_t add_count()     const noexcept { return add_count_;     }
    uint64_t execute_count() const noexcept { return execute_count_; }
    uint64_t cancel_count()  const noexcept { return cancel_count_;  }
    uint64_t delete_count()  const noexcept { return delete_count_;  }
    uint64_t replace_count() const noexcept { return replace_count_; }
    uint64_t total_messages() const noexcept {
        return add_count_ + execute_count_ + cancel_count_ +
               delete_count_ + replace_count_;
    }

    // ── Synthetic message builders (for unit tests) ───────────────────────

    // Build a binary 'A' (Add Order) message ready for process_message().
    static std::vector<uint8_t> make_add_order(
        uint64_t    ref_num,
        char        side,           // 'B' or 'S'
        uint32_t    shares,
        const char* stock,          // 8-char space-padded
        uint32_t    price_raw,      // price × 10000
        uint64_t    timestamp_ns = 0
    );

    // Build a binary 'E' (Order Executed) message.
    static std::vector<uint8_t> make_order_executed(
        uint64_t ref_num,
        uint32_t executed_shares,
        uint64_t timestamp_ns = 0
    );

    // Build a binary 'X' (Order Cancel) message.
    static std::vector<uint8_t> make_order_cancel(
        uint64_t ref_num,
        uint32_t cancelled_shares,
        uint64_t timestamp_ns = 0
    );

    // Build a binary 'D' (Order Delete) message.
    static std::vector<uint8_t> make_order_delete(
        uint64_t ref_num,
        uint64_t timestamp_ns = 0
    );

    // Build a binary 'U' (Order Replace) message.
    static std::vector<uint8_t> make_order_replace(
        uint64_t old_ref_num,
        uint64_t new_ref_num,
        uint32_t new_shares,
        uint32_t new_price_raw,
        uint64_t timestamp_ns = 0
    );

private:
    // ── Internal order state ──────────────────────────────────────────────

    struct Order {
        uint64_t ref_num;
        char     side;           // 'B' or 'S'
        uint32_t remaining;      // remaining shares
        double   price;          // in dollars
        std::string stock;
    };

    // ref_num → active order
    std::unordered_map<uint64_t, Order> orders_;

    // L2 aggregates per stock:
    //   stock → { bids: price→(qty,count), asks: price→(qty,count) }
    struct LevelInfo { double qty = 0.0; int count = 0; };
    struct BookSides {
        std::map<double, LevelInfo, std::greater<double>> bids;
        std::map<double, LevelInfo>                       asks;
    };
    std::unordered_map<std::string, BookSides> l2_;

    L2Callback  callback_;
    std::string filter_;

    uint64_t add_count_     = 0;
    uint64_t execute_count_ = 0;
    uint64_t cancel_count_  = 0;
    uint64_t delete_count_  = 0;
    uint64_t replace_count_ = 0;

    // ── Message handlers ─────────────────────────────────────────────────

    void handle_add_order         (const uint8_t* buf, size_t len, uint64_t ts);
    void handle_order_executed    (const uint8_t* buf, size_t len, uint64_t ts);
    void handle_order_cancel      (const uint8_t* buf, size_t len, uint64_t ts);
    void handle_order_delete      (const uint8_t* buf, size_t len, uint64_t ts);
    void handle_order_replace     (const uint8_t* buf, size_t len, uint64_t ts);

    // Reduce an order's remaining quantity; remove if depleted.
    // Emits L2Event if the corresponding price level changes.
    void reduce_order(uint64_t ref_num, uint32_t by_shares, uint64_t ts);

    // Remove an order entirely, updating the L2 aggregate.
    void remove_order(uint64_t ref_num, uint64_t ts);

    // Update the L2 aggregate after an order change and emit a callback.
    void update_l2(const Order& order, double old_qty, uint64_t ts);

    // Emit callback if the symbol passes the filter.
    void emit(const L2Event& ev);

    // ── Big-endian binary readers ─────────────────────────────────────────

    static uint16_t ru16(const uint8_t* p) noexcept;
    static uint32_t ru32(const uint8_t* p) noexcept;
    static uint64_t ru64(const uint8_t* p) noexcept;
    static uint64_t ru48(const uint8_t* p) noexcept;  // 6-byte timestamp
    static std::string read_stock(const uint8_t* p) noexcept;

    // ── Big-endian binary writers (for synthetic message builders) ────────

    static void wu16(uint8_t* p, uint16_t v) noexcept;
    static void wu32(uint8_t* p, uint32_t v) noexcept;
    static void wu64(uint8_t* p, uint64_t v) noexcept;
    static void wu48(uint8_t* p, uint64_t v) noexcept;
};

} // namespace itch
