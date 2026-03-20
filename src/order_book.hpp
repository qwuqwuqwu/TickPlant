#pragma once

// order_book.hpp
//
// L2 order book engine.
//
// Maintains full bid/ask depth from FIX 4.4 market data messages:
//   apply_snapshot() — 35=W MarketDataSnapshotFullRefresh (full replace)
//   apply_update()   — 35=X MarketDataIncrementalRefresh  (delta update)
//
// Also exposes a direct L2 interface (set_level / delete_level) used by the
// NASDAQ ITCH 5.0 replay test harness and any future non-FIX data sources.
//
// Design decisions (ROADMAP §2.2):
//   - std::map<price, PriceLevel> for sorted iteration:
//       bids: std::greater<double>  → begin() = best bid  (O(1))
//       asks: default ascending     → begin() = best ask  (O(1))
//     std::unordered_map would give O(1) insert but O(n log n) to find BBO
//     and O(n) for range queries.  std::map's O(log n) insert is fine for
//     the expected update rate.
//
//   - std::shared_mutex (reader-writer lock):
//       Multiple threads may call get_snapshot() / best_bid() concurrently
//       without blocking each other.  Only apply_snapshot / apply_update /
//       set_level / delete_level take an exclusive (write) lock.
//
//   - PriceLevel uses double for price.  In production you would use a
//     fixed-point integer (e.g., int64_t in units of 1/10000) to avoid
//     floating-point key collisions in the map.  Double is fine here because
//     FIX prices have at most 4-6 decimal places and we always use the exact
//     parsed value as the map key.

#include "fix_parser.hpp"

#include <functional>
#include <map>
#include <shared_mutex>
#include <string>
#include <vector>
#include <cstdint>

// ─── PriceLevel ───────────────────────────────────────────────────────────────

// One aggregated price level on either the bid or ask side.
struct PriceLevel {
    double   price       = 0.0;
    double   quantity    = 0.0;
    int      order_count = 0;     // number of individual orders at this level
                                  // (0 if not provided by the feed)
};

// ─── OrderBookSnapshot ────────────────────────────────────────────────────────

// Immutable copy of the book state at a point in time.
// Owns its data — safe to pass across threads and hold indefinitely.
//
// is_snapshot semantics (used by ArbitrageEngine::update_order_book):
//   true  — full book replace: engine clears its book then inserts every level.
//           (Binance @depth20@100ms always sends full partial-book; Bybit/Coinbase/
//            Kraken initial snapshot.)
//   false — incremental delta: engine applies each level individually.
//           quantity == 0.0 signals DELETE this price level; quantity > 0 is
//           INSERT/REPLACE.  (Bybit delta, Coinbase update, Kraken update.)
struct OrderBookSnapshot {
    std::string              symbol;
    std::string              exchange;
    uint64_t                 timestamp_ms = 0;
    bool                     is_snapshot  = true;   // true = full replace, false = delta

    std::vector<PriceLevel>  bids;   // descending price (index 0 = best bid)
    std::vector<PriceLevel>  asks;   // ascending  price (index 0 = best ask)

    bool   empty()      const noexcept { return bids.empty() && asks.empty(); }
    size_t bid_depth()  const noexcept { return bids.size(); }
    size_t ask_depth()  const noexcept { return asks.size(); }

    double best_bid()   const noexcept {
        return bids.empty() ? 0.0 : bids.front().price;
    }
    double best_ask()   const noexcept {
        return asks.empty() ? 0.0 : asks.front().price;
    }
    double mid_price()  const noexcept {
        if (bids.empty() || asks.empty()) return 0.0;
        return (best_bid() + best_ask()) / 2.0;
    }
    double spread_bps() const noexcept {
        if (best_bid() <= 0.0) return 0.0;
        return ((best_ask() - best_bid()) / best_bid()) * 10'000.0;
    }
};

// ─── OrderBook ────────────────────────────────────────────────────────────────

class OrderBook {
public:
    // Side enum used by the direct L2 interface
    enum class Side { Bid, Ask };

    explicit OrderBook(std::string symbol, std::string exchange = "FIX");

    // ── FIX feed interface ────────────────────────────────────────────────

    // Apply a 35=W MarketDataSnapshotFullRefresh:
    //   clears both sides and repopulates from all MDEntry records.
    //   msg.symbol is used to update the book's symbol if not yet set.
    void apply_snapshot(const FIXMessage& msg);

    // Apply a 35=X MarketDataIncrementalRefresh:
    //   processes each MDEntry according to its MDUpdateAction:
    //     New    (0) — insert or replace the price level
    //     Change (1) — update quantity at an existing level
    //     Delete (2) — remove the price level
    void apply_update(const FIXMessage& msg);

    // ── Direct L2 interface (ITCH replay, unit tests) ─────────────────────

    // Insert or replace the quantity at the given price level.
    void set_level(Side side, double price, double quantity,
                   int order_count = 0);

    // Remove the given price level (no-op if it does not exist).
    void delete_level(Side side, double price);

    // Remove all price levels on both sides.
    void clear();

    // ── Read-side (all thread-safe via shared_mutex) ──────────────────────

    // Return a full copy of the book — multiple threads may call concurrently.
    OrderBookSnapshot get_snapshot() const;

    double best_bid()   const;   // O(1) — highest bid price, 0 if empty
    double best_ask()   const;   // O(1) — lowest  ask price, 0 if empty
    double mid_price()  const;   // (best_bid + best_ask) / 2, 0 if either empty
    double spread_bps() const;   // (ask - bid) / bid * 10000, 0 if bid == 0

    size_t bid_depth()  const;   // number of distinct bid price levels
    size_t ask_depth()  const;   // number of distinct ask price levels
    bool   empty()      const;

    const std::string& symbol()         const noexcept { return symbol_;         }
    const std::string& exchange()       const noexcept { return exchange_;       }
    uint64_t           last_update_ms() const;
    uint64_t           snapshot_count() const;
    uint64_t           update_count()   const;

private:
    std::string symbol_;
    std::string exchange_;

    // Bids: descending — begin() is always the best (highest) bid
    std::map<double, PriceLevel, std::greater<double>> bids_;
    // Asks: ascending  — begin() is always the best (lowest)  ask
    std::map<double, PriceLevel>                       asks_;

    mutable std::shared_mutex rw_mutex_;

    uint64_t last_update_ms_ = 0;
    uint64_t snapshot_count_ = 0;
    uint64_t update_count_   = 0;

    // Apply a single MDEntry during apply_update().
    void apply_md_entry(const MDEntry& entry);

    // Set last_update_ms_ to current wall-clock time.
    void touch();
};
