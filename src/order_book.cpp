// order_book.cpp
//
// Implementation of the L2 order book engine.

#include "order_book.hpp"

#include <chrono>
#include <mutex>
#include <stdexcept>

// ─── Internal helpers ─────────────────────────────────────────────────────────

static uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

// ─── Constructor ──────────────────────────────────────────────────────────────

OrderBook::OrderBook(std::string symbol, std::string exchange)
    : symbol_(std::move(symbol))
    , exchange_(std::move(exchange))
{}

// ─── FIX feed interface ───────────────────────────────────────────────────────

void OrderBook::apply_snapshot(const FIXMessage& msg) {
    if (msg.msg_type != FIXMsgType::MarketDataSnapshot) return;

    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    bids_.clear();
    asks_.clear();

    // Use the FIX message's symbol if the book was constructed with an empty one
    if (symbol_.empty() && !msg.symbol.empty())
        symbol_ = std::string(msg.symbol);

    for (int i = 0; i < msg.num_entries; ++i) {
        const MDEntry& e = msg.entries[i];
        PriceLevel level{ e.price, e.quantity, 0 };

        if (e.entry_type == static_cast<int>(MDEntryType::Bid)) {
            bids_[e.price] = level;
        } else if (e.entry_type == static_cast<int>(MDEntryType::Ask)) {
            asks_[e.price] = level;
        }
        // Trade entries (type 2) are informational — not stored in the book
    }

    touch();
    ++snapshot_count_;
}

void OrderBook::apply_update(const FIXMessage& msg) {
    if (msg.msg_type != FIXMsgType::MarketDataIncremental) return;

    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    for (int i = 0; i < msg.num_entries; ++i)
        apply_md_entry(msg.entries[i]);

    touch();
    ++update_count_;
}

// Caller must hold the write lock.
void OrderBook::apply_md_entry(const MDEntry& entry) {
    const int action = entry.update_action;
    const bool is_bid = (entry.entry_type == static_cast<int>(MDEntryType::Bid));
    const bool is_ask = (entry.entry_type == static_cast<int>(MDEntryType::Ask));

    if (!is_bid && !is_ask) return;  // skip Trade entries

    if (action == static_cast<int>(MDUpdateAction::New) ||
        action == static_cast<int>(MDUpdateAction::Change)) {
        // Insert or replace the level
        PriceLevel level{ entry.price, entry.quantity, 0 };
        if (is_bid) bids_[entry.price] = level;
        else        asks_[entry.price] = level;

    } else if (action == static_cast<int>(MDUpdateAction::Delete)) {
        if (is_bid) bids_.erase(entry.price);
        else        asks_.erase(entry.price);
    }
}

// ─── Direct L2 interface ──────────────────────────────────────────────────────

void OrderBook::set_level(Side side, double price, double quantity,
                           int order_count) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    PriceLevel level{ price, quantity, order_count };
    if (side == Side::Bid) bids_[price] = level;
    else                   asks_[price] = level;
    touch();
}

void OrderBook::delete_level(Side side, double price) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    if (side == Side::Bid) bids_.erase(price);
    else                   asks_.erase(price);
    touch();
}

void OrderBook::clear() {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    bids_.clear();
    asks_.clear();
    touch();
}

// ─── Read-side ────────────────────────────────────────────────────────────────

OrderBookSnapshot OrderBook::get_snapshot() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);

    OrderBookSnapshot snap;
    snap.symbol       = symbol_;
    snap.exchange     = exchange_;
    snap.timestamp_ms = last_update_ms_;

    snap.bids.reserve(bids_.size());
    snap.asks.reserve(asks_.size());

    for (const auto& [price, level] : bids_) snap.bids.push_back(level);
    for (const auto& [price, level] : asks_) snap.asks.push_back(level);

    return snap;
}

double OrderBook::best_bid() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    return bids_.empty() ? 0.0 : bids_.begin()->second.price;
}

double OrderBook::best_ask() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    return asks_.empty() ? 0.0 : asks_.begin()->second.price;
}

double OrderBook::mid_price() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (bids_.empty() || asks_.empty()) return 0.0;
    return (bids_.begin()->second.price + asks_.begin()->second.price) / 2.0;
}

double OrderBook::spread_bps() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (bids_.empty() || asks_.empty()) return 0.0;
    const double bid = bids_.begin()->second.price;
    if (bid <= 0.0) return 0.0;
    const double ask = asks_.begin()->second.price;
    return ((ask - bid) / bid) * 10'000.0;
}

size_t OrderBook::bid_depth() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    return bids_.size();
}

size_t OrderBook::ask_depth() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    return asks_.size();
}

bool OrderBook::empty() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    return bids_.empty() && asks_.empty();
}

uint64_t OrderBook::last_update_ms() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    return last_update_ms_;
}

uint64_t OrderBook::snapshot_count() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    return snapshot_count_;
}

uint64_t OrderBook::update_count() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    return update_count_;
}

// ─── Private helpers ──────────────────────────────────────────────────────────

// Caller must hold the write lock.
void OrderBook::touch() {
    last_update_ms_ = now_ms();
}
