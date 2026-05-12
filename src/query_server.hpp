#pragma once

// query_server.hpp
//
// Shared protocol types and JSON serialization for the TickPlant query server.
//
// Wire protocol (text, each request/response terminated by exactly one '\n'):
//
//   Request:
//     SNAPSHOT <canonical_symbol>\n   e.g. "SNAPSHOT BTC\n"
//     HEALTH\n
//     REPORT <name>\n                 e.g. "REPORT bbo_summary\n"
//     LISTREPORTS\n
//
//   Response (compact JSON + '\n'):
//     SNAPSHOT ok:
//       {"status":"ok","symbol":"BTC","books":[
//         {"exchange":"Binance","symbol":"BTCUSDT",
//          "bids":[[p,q],...],"asks":[[p,q],...],
//          "timestamp_ms":1234567890123},
//         ...
//       ]}
//     SNAPSHOT not found:
//       {"status":"error","message":"symbol not found: XYZ"}
//     HEALTH:
//       {"status":"ok","feeds":[
//         {"exchange":"Binance","staleness_ms":45,"live":true},
//         ...
//       ]}
//     REPORT:
//       {"status":"ok","report":"bbo_summary","generated_at_ms":...,"resolution":{...},"symbols":{...}}
//     LISTREPORTS:
//       {"status":"ok","reports":["bbo_summary","cross_venue",...]}

#include "order_book.hpp"
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ─── Request parsing ──────────────────────────────────────────────────────────

enum class RequestType { SNAPSHOT, HEALTH, REPORT, LISTREPORTS, UNKNOWN };

struct ParsedRequest {
    RequestType type        = RequestType::UNKNOWN;
    std::string symbol;     // canonical symbol for SNAPSHOT, empty for others
    std::string report_name; // report name for REPORT
};

// Parse one request line (without the trailing '\n').
ParsedRequest parse_request(std::string_view line);

// ─── Response builders ────────────────────────────────────────────────────────

// Serialize a set of order book snapshots for one canonical symbol.
// max_levels caps bid/ask depth per book to keep response size bounded.
// Returned string ends with '\n'.
std::string snapshot_response(const std::string& canonical_sym,
                               const std::vector<OrderBookSnapshot>& books,
                               int max_levels = 5);

// Serialize per-exchange feed staleness.
// live_threshold_ms: staleness below this value → "live":true.
// Returned string ends with '\n'.
std::string health_response(
    const std::unordered_map<std::string, uint64_t>& staleness_ms,
    uint64_t live_threshold_ms = 2000);

// Generic error response. Returned string ends with '\n'.
std::string error_response(std::string_view message);
