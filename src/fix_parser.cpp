// fix_parser.cpp
//
// Implementation of FIXParser and FIXMessage::to_ticker_data().
//
// Hot-path design notes:
//   - next_field() does a single linear scan per field: O(1) amortised
//   - parse_int / parse_double avoid std::stoi / std::stod to eliminate
//     locale lookup and heap allocation
//   - parse() processes every byte exactly once: O(n) in message length
//   - build_snapshot / build_incremental are test/simulation utilities only —
//     they use std::ostringstream and are not on the hot path

#include "fix_parser.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>

// ─── Private helpers ──────────────────────────────────────────────────────────

bool FIXParser::next_field(std::string_view& cursor,
                           int&              tag,
                           std::string_view& value) noexcept {
    if (cursor.empty()) return false;

    // Locate '=' separating tag number from value
    const auto eq = cursor.find('=');
    if (eq == std::string_view::npos) return false;

    tag = parse_int(cursor.substr(0, eq));
    cursor.remove_prefix(eq + 1);

    // Locate SOH delimiter at end of value
    const auto soh = cursor.find(FIX_SOH);
    if (soh == std::string_view::npos) {
        value  = cursor;    // last field — no trailing SOH
        cursor = {};
    } else {
        value  = cursor.substr(0, soh);
        cursor.remove_prefix(soh + 1);
    }

    return true;
}

int FIXParser::parse_int(std::string_view sv) noexcept {
    int    result   = 0;
    bool   negative = false;
    size_t i        = 0;

    if (!sv.empty() && sv[0] == '-') { negative = true; ++i; }

    for (; i < sv.size(); ++i) {
        const char c = sv[i];
        if (c < '0' || c > '9') break;
        result = result * 10 + (c - '0');
    }
    return negative ? -result : result;
}

double FIXParser::parse_double(std::string_view sv) noexcept {
    double result   = 0.0;
    bool   negative = false;
    size_t i        = 0;

    if (!sv.empty() && sv[0] == '-') { negative = true; ++i; }

    // Integer part
    for (; i < sv.size() && sv[i] != '.'; ++i) {
        if (sv[i] < '0' || sv[i] > '9') break;
        result = result * 10.0 + static_cast<double>(sv[i] - '0');
    }

    // Fractional part
    if (i < sv.size() && sv[i] == '.') {
        ++i;
        double factor = 0.1;
        for (; i < sv.size(); ++i) {
            if (sv[i] < '0' || sv[i] > '9') break;
            result += static_cast<double>(sv[i] - '0') * factor;
            factor *= 0.1;
        }
    }

    return negative ? -result : result;
}

FIXMsgType FIXParser::parse_msg_type(std::string_view sv) noexcept {
    if (sv.size() != 1) return FIXMsgType::Unknown;
    switch (sv[0]) {
        case 'W': return FIXMsgType::MarketDataSnapshot;
        case 'X': return FIXMsgType::MarketDataIncremental;
        case 'D': return FIXMsgType::NewOrderSingle;
        case '8': return FIXMsgType::ExecutionReport;
        default:  return FIXMsgType::Unknown;
    }
}

// ─── FIXParser::parse ─────────────────────────────────────────────────────────

bool FIXParser::parse(std::string_view msg, FIXMessage& out) const {
    out = FIXMessage{};   // zero-initialise all fields

    std::string_view cursor        = msg;
    int              tag           = 0;
    std::string_view value;
    int              current_entry = -1;

    while (next_field(cursor, tag, value)) {
        switch (tag) {

            // ── Header ──────────────────────────────────────────────────────
            case 8:  /* BeginString — accept any version */             break;
            case 9:  /* BodyLength  — skip, trust the stream */         break;
            case 35: out.msg_type  = parse_msg_type(value);            break;
            case 34: out.seq_num   = parse_int(value);                 break;
            case 49: out.sender    = value;                            break;
            case 56: out.target    = value;                            break;
            case 52: out.send_time = value;                            break;

            // ── Common body ─────────────────────────────────────────────────
            case 55:  out.symbol = value;                              break;
            case 262: out.req_id = value;                              break;

            // ── Market data repeating group ──────────────────────────────────
            case 268:  // NoMDEntries — reset group state
                out.num_entries = 0;
                current_entry   = -1;
                break;

            case 269:  // MDEntryType
                //
                // In 35=W, tag 269 is the repeating-group delimiter — each
                // occurrence opens a new entry.
                //
                // In 35=X, tag 279 (MDUpdateAction) is the delimiter and tag
                // 269 fills the entry_type of the already-opened entry.
                // We also handle 35=X messages that omit tag 279 by opening
                // a new entry whenever current_entry < 0.
                //
                if (out.msg_type == FIXMsgType::MarketDataSnapshot ||
                    current_entry < 0) {
                    if (out.num_entries < FIX_MAX_MD_ENTRIES) {
                        current_entry = out.num_entries++;
                        out.entries[current_entry] = {};
                    } else {
                        current_entry = -1;  // overflow guard — ignore extra entries
                    }
                }
                if (current_entry >= 0)
                    out.entries[current_entry].entry_type = parse_int(value);
                break;

            case 279:  // MDUpdateAction — repeating-group delimiter for 35=X
                if (out.num_entries < FIX_MAX_MD_ENTRIES) {
                    current_entry = out.num_entries++;
                    out.entries[current_entry] = {};
                } else {
                    current_entry = -1;
                }
                if (current_entry >= 0)
                    out.entries[current_entry].update_action = parse_int(value);
                break;

            case 270:  // MDEntryPx
                if (current_entry >= 0)
                    out.entries[current_entry].price = parse_double(value);
                break;

            case 271:  // MDEntrySize
                if (current_entry >= 0)
                    out.entries[current_entry].quantity = parse_double(value);
                break;

            // ── Order fields (35=D and 35=8) ────────────────────────────────
            case 11: out.cl_ord_id     = value;                        break;
            case 17: out.exec_id       = value;                        break;
            case 31: out.last_px       = parse_double(value);          break;
            case 32: out.last_qty      = parse_double(value);          break;
            case 38: out.order_qty     = parse_double(value);          break;
            case 39: out.ord_status    = parse_int(value);             break;
            case 40: out.ord_type      = parse_int(value);             break;
            case 44: out.order_price   = parse_double(value);          break;
            case 54: out.side          = parse_int(value);             break;
            case 59: out.time_in_force = parse_int(value);             break;

            // ── Trailer ───────────────────────────────────────────────────────
            case 10:  // Checksum — marks end of message
                return out.valid();

            default: break;
        }
    }

    return out.valid();
}

// ─── FIXMessage::to_ticker_data ───────────────────────────────────────────────

std::optional<TickerData> FIXMessage::to_ticker_data() const {
    if (!is_market_data()) return std::nullopt;

    double best_bid     = 0.0;
    double best_ask     = std::numeric_limits<double>::max();
    double best_bid_qty = 0.0;
    double best_ask_qty = 0.0;
    bool   has_bid      = false;
    bool   has_ask      = false;

    for (int i = 0; i < num_entries; ++i) {
        const MDEntry& e = entries[i];
        if (e.entry_type == static_cast<int>(MDEntryType::Bid)) {
            if (!has_bid || e.price > best_bid) {
                best_bid     = e.price;
                best_bid_qty = e.quantity;
                has_bid      = true;
            }
        } else if (e.entry_type == static_cast<int>(MDEntryType::Ask)) {
            if (!has_ask || e.price < best_ask) {
                best_ask     = e.price;
                best_ask_qty = e.quantity;
                has_ask      = true;
            }
        }
    }

    if (!has_bid || !has_ask) return std::nullopt;

    TickerData t;
    t.symbol       = std::string(symbol);
    t.exchange     = "FIX";
    t.bid_price    = best_bid;
    t.ask_price    = best_ask;
    t.bid_quantity = best_bid_qty;
    t.ask_quantity = best_ask_qty;
    t.timestamp_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    return t;
}

// ─── build_fix_message (file-local helper) ────────────────────────────────────

// Shared logic for build_snapshot and build_incremental.
// Computes FIX body length and checksum correctly.
static std::string build_fix_message(
    const std::string&          msg_type_str,
    const std::string&          symbol,
    const std::vector<MDEntry>& entries,
    int                         seq_num,
    char                        delim)
{
    // ── SendingTime (tag 52): current UTC, FIX format YYYYMMDD-HH:MM:SS.sss ──
    const auto   now_tp  = std::chrono::system_clock::now();
    const auto   now_tt  = std::chrono::system_clock::to_time_t(now_tp);
    const auto   now_ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                               now_tp.time_since_epoch()) % 1000;
    std::tm      gmt{};
#ifdef _WIN32
    gmtime_s(&gmt, &now_tt);
#else
    gmtime_r(&now_tt, &gmt);
#endif
    char ts_buf[24];
    std::strftime(ts_buf, sizeof(ts_buf), "%Y%m%d-%H:%M:%S", &gmt);
    std::snprintf(ts_buf + 15, sizeof(ts_buf) - 15, ".%03d",
                  static_cast<int>(now_ms.count()));

    // ── Build body (tag 35 onward, before the 10= checksum field) ─────────
    std::ostringstream body;
    body << "35=" << msg_type_str    << delim
         << "34=" << seq_num         << delim
         << "49=SIMULATOR"           << delim
         << "56=CLIENT"              << delim
         << "52=" << ts_buf          << delim
         << "55=" << symbol          << delim
         << "268=" << entries.size() << delim;

    for (const auto& e : entries) {
        if (msg_type_str == "X")
            body << "279=" << e.update_action << delim;
        body << "269=" << e.entry_type << delim
             << "270=" << e.price      << delim
             << "271=" << e.quantity   << delim;
    }

    const std::string body_str = body.str();
    const int         body_len = static_cast<int>(body_str.size());

    // ── Prepend header ─────────────────────────────────────────────────────
    std::ostringstream full;
    full << "8=FIX.4.4"      << delim
         << "9=" << body_len << delim
         << body_str;

    // ── Checksum: sum of all preceding bytes mod 256, zero-padded to 3 ────
    const std::string so_far = full.str();
    unsigned int cksum = 0;
    for (unsigned char c : so_far) cksum += c;
    cksum %= 256;

    full << "10=" << std::setw(3) << std::setfill('0') << cksum << delim;
    return full.str();
}

// ─── FIXParser::build_snapshot ────────────────────────────────────────────────

std::string FIXParser::build_snapshot(
    const std::string&          symbol,
    const std::vector<MDEntry>& entries,
    int                         seq_num,
    bool                        use_soh)
{
    return build_fix_message("W", symbol, entries, seq_num,
                             use_soh ? FIX_SOH : '|');
}

// ─── FIXParser::build_incremental ─────────────────────────────────────────────

std::string FIXParser::build_incremental(
    const std::string&          symbol,
    const std::vector<MDEntry>& entries,
    int                         seq_num,
    bool                        use_soh)
{
    return build_fix_message("X", symbol, entries, seq_num,
                             use_soh ? FIX_SOH : '|');
}
