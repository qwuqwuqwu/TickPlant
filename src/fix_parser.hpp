#pragma once

// fix_parser.hpp
//
// Zero-copy FIX 4.4 message parser.
//
// Design goals:
//   - Single-pass O(n) scan — no backtracking
//   - No heap allocation during parse — all string fields are string_view
//     into the caller's buffer
//   - Stateless parser — safe to share across threads without locking
//   - Supports message types needed for TickPlant:
//       35=W  MarketDataSnapshotFullRefresh  (full order book snapshot)
//       35=X  MarketDataIncrementalRefresh   (delta update)
//       35=D  NewOrderSingle                 (order submission)
//       35=8  ExecutionReport               (fill / rejection)

#include "types.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// ─── Constants ────────────────────────────────────────────────────────────────

// FIX field delimiter: Start of Heading (ASCII 0x01)
inline constexpr char FIX_SOH = '\x01';

// Maximum market data entries per message.
// 32 covers L10 depth on both sides with room to spare.
inline constexpr int FIX_MAX_MD_ENTRIES = 32;

// ─── Enumerations ─────────────────────────────────────────────────────────────

// FIX tag 35 — message type
enum class FIXMsgType : uint8_t {
    Unknown               = 0,
    MarketDataSnapshot    = 1,   // 35=W  full refresh
    MarketDataIncremental = 2,   // 35=X  delta update
    NewOrderSingle        = 3,   // 35=D
    ExecutionReport       = 4,   // 35=8
};

// FIX tag 269 — MDEntryType
enum class MDEntryType : int {
    Bid   = 0,
    Ask   = 1,
    Trade = 2,
};

// FIX tag 279 — MDUpdateAction (35=X only)
enum class MDUpdateAction : int {
    New    = 0,
    Change = 1,
    Delete = 2,
};

// ─── MDEntry ──────────────────────────────────────────────────────────────────

// One price level within a 35=W or 35=X message.
struct MDEntry {
    int    entry_type    = 0;    // tag 269: 0=Bid, 1=Ask, 2=Trade
    int    update_action = 0;    // tag 279: 0=New, 1=Change, 2=Delete  (X only)
    double price         = 0.0;  // tag 270
    double quantity      = 0.0;  // tag 271
};

// ─── FIXMessage ───────────────────────────────────────────────────────────────

// Parsed FIX 4.4 message.
//
// All string_view fields are zero-copy — they point directly into the source
// buffer passed to FIXParser::parse(). The caller must keep that buffer alive
// for as long as the FIXMessage is in use.
//
// Stack size: ~sizeof(MDEntry)*32 + a few ints/doubles ≈ 640 bytes.
// Suitable for stack allocation in the hot path.
struct FIXMessage {
    // ── Header ────────────────────────────────────────────────────────────
    FIXMsgType       msg_type  = FIXMsgType::Unknown;
    int              seq_num   = 0;         // tag 34
    std::string_view sender;                // tag 49
    std::string_view target;                // tag 56
    std::string_view send_time;             // tag 52

    // ── Common body ───────────────────────────────────────────────────────
    std::string_view symbol;                // tag 55
    std::string_view req_id;                // tag 262  MDReqID

    // ── Market data  (35=W and 35=X) ─────────────────────────────────────
    int     num_entries = 0;
    MDEntry entries[FIX_MAX_MD_ENTRIES];

    // ── Order fields (35=D and 35=8) ─────────────────────────────────────
    std::string_view cl_ord_id;             // tag 11
    std::string_view exec_id;               // tag 17
    int    side          = 0;               // tag 54: 1=Buy, 2=Sell
    int    ord_type      = 0;               // tag 40: 1=Market, 2=Limit
    int    ord_status    = 0;               // tag 39
    int    time_in_force = 0;               // tag 59
    double order_price   = 0.0;             // tag 44
    double order_qty     = 0.0;             // tag 38
    double last_qty      = 0.0;             // tag 32
    double last_px       = 0.0;             // tag 31

    // ── Helpers ───────────────────────────────────────────────────────────

    bool valid() const noexcept {
        return msg_type != FIXMsgType::Unknown;
    }

    bool is_market_data() const noexcept {
        return msg_type == FIXMsgType::MarketDataSnapshot ||
               msg_type == FIXMsgType::MarketDataIncremental;
    }

    // Extract best bid / best ask from market data entries → TickerData.
    //
    // For 35=W: picks the highest bid price and lowest ask price across all entries.
    // For 35=X: same scan, useful for partial updates that include BBO.
    //
    // Returns nullopt if the message is not market data, or has no bid/ask entry.
    // Note: timestamp_ms is set to current wall-clock time at call time.
    std::optional<TickerData> to_ticker_data() const;
};

// ─── FIXParser ────────────────────────────────────────────────────────────────

// Zero-copy, single-pass FIX 4.4 message parser.
//
// Usage:
//   FIXParser  parser;          // stateless — construct once, reuse freely
//   FIXMessage msg;
//   if (parser.parse(raw_bytes, msg)) {
//       if (msg.is_market_data()) { ... }
//   }
//
// Thread safety: parse() is const and reads no shared state — safe to call
// concurrently from multiple threads with separate FIXMessage output buffers.
class FIXParser {
public:
    // Parse a complete FIX message.
    //
    // `msg` must remain valid for the lifetime of the returned FIXMessage
    // (all string_view fields in `out` point directly into `msg`).
    //
    // Returns true  if tag 35 was recognised and the message parsed cleanly.
    // Returns false on empty input or unrecognised message type.
    bool parse(std::string_view msg, FIXMessage& out) const;

    // ── Test / simulation helpers ─────────────────────────────────────────

    // Build a syntactically valid 35=W (MarketDataSnapshot) message.
    //   use_soh=true  → ASCII 0x01 delimiters  (wire format, parseable)
    //   use_soh=false → '|' delimiters         (human-readable logging)
    static std::string build_snapshot(
        const std::string&          symbol,
        const std::vector<MDEntry>& entries,
        int  seq_num = 1,
        bool use_soh = true
    );

    // Build a syntactically valid 35=X (MarketDataIncremental) message.
    static std::string build_incremental(
        const std::string&          symbol,
        const std::vector<MDEntry>& entries,
        int  seq_num = 1,
        bool use_soh = true
    );

private:
    // Advance `cursor` past the next tag=value\x01 field.
    // On success, tag and value are filled; cursor points past the SOH.
    // Returns false when the input is exhausted or malformed.
    static bool next_field(std::string_view& cursor,
                           int&              tag,
                           std::string_view& value) noexcept;

    // Fast integer parser — no locale, no allocation, no exceptions.
    static int    parse_int   (std::string_view sv) noexcept;

    // Fast double parser — handles sign, integer, and fractional parts.
    // Avoids std::stod to eliminate locale overhead and heap allocation.
    static double parse_double(std::string_view sv) noexcept;

    // Map tag 35 value string to FIXMsgType.
    static FIXMsgType parse_msg_type(std::string_view sv) noexcept;
};

// ─── Utility ──────────────────────────────────────────────────────────────────

// Human-readable name for a FIXMsgType — useful in logs and benchmark output.
constexpr const char* to_string(FIXMsgType t) noexcept {
    switch (t) {
        case FIXMsgType::MarketDataSnapshot:    return "MarketDataSnapshot(W)";
        case FIXMsgType::MarketDataIncremental: return "MarketDataIncremental(X)";
        case FIXMsgType::NewOrderSingle:        return "NewOrderSingle(D)";
        case FIXMsgType::ExecutionReport:       return "ExecutionReport(8)";
        default:                                return "Unknown";
    }
}
