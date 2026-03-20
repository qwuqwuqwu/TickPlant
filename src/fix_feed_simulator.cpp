// fix_feed_simulator.cpp

#include "fix_feed_simulator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

// ─── Construction / destruction ───────────────────────────────────────────────

FIXFeedSimulator::FIXFeedSimulator(std::string exchange)
    : exchange_(std::move(exchange)) {}

FIXFeedSimulator::~FIXFeedSimulator() { stop(); }

// ─── Configuration ────────────────────────────────────────────────────────────

void FIXFeedSimulator::add_symbol(const SimulatedSymbol& sym) {
    if (running_) return;

    SymbolState s;
    s.config      = sym;
    s.current_mid = sym.mid_price;
    // last_snapshot stays at epoch → first iteration always emits a snapshot

    // Seed differently per symbol so price sequences diverge
    s.rng.seed(static_cast<uint32_t>(
        std::hash<std::string>{}(sym.symbol)) ^ std::random_device{}()
    );

    states_.push_back(std::move(s));
}

void FIXFeedSimulator::set_snapshot_interval_ms(int ms) {
    snapshot_interval_ms_ = ms;
}

void FIXFeedSimulator::set_incremental_hz(int hz) {
    incremental_hz_ = (hz > 0) ? hz : 1;
}

void FIXFeedSimulator::set_callback(MessageCallback cb) {
    callback_ = std::move(cb);
}

// ─── Lifecycle ────────────────────────────────────────────────────────────────

void FIXFeedSimulator::start() {
    if (running_ || states_.empty() || !callback_) return;
    running_ = true;
    thread_  = std::thread(&FIXFeedSimulator::simulation_loop, this);
}

void FIXFeedSimulator::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

// ─── Simulation loop ──────────────────────────────────────────────────────────

void FIXFeedSimulator::simulation_loop() {
    const auto sleep_ms      = std::chrono::milliseconds(
                                   std::max(1, 1000 / incremental_hz_));
    const auto snap_interval = std::chrono::milliseconds(snapshot_interval_ms_);

    while (running_) {
        const auto now = std::chrono::steady_clock::now();

        for (auto& s : states_) {
            // Apply a tiny random walk: ±0.005% per tick
            std::uniform_real_distribution<double> drift(-0.00005, 0.00005);
            s.current_mid *= (1.0 + drift(s.rng));

            if (now - s.last_snapshot >= snap_interval) {
                emit_snapshot(s);
                s.last_snapshot = now;
            } else {
                emit_incremental(s);
            }
        }

        std::this_thread::sleep_for(sleep_ms);
    }
}

// ─── Level construction ───────────────────────────────────────────────────────

std::vector<MDEntry> FIXFeedSimulator::make_levels(
    double mid, double spread_pct, double step, int depth) const
{
    std::vector<MDEntry> entries;
    entries.reserve(depth * 2);

    const double best_bid = mid * (1.0 - spread_pct / 2.0);
    const double best_ask = mid * (1.0 + spread_pct / 2.0);

    // Bids: descending price, more quantity at the best level
    for (int i = 0; i < depth; ++i) {
        MDEntry e;
        e.entry_type    = static_cast<int>(MDEntryType::Bid);
        e.update_action = static_cast<int>(MDUpdateAction::New);
        e.price         = best_bid - step * i;
        e.quantity      = static_cast<double>(depth - i) * 0.5 + 0.5;
        entries.push_back(e);
    }

    // Asks: ascending price, more quantity at the best level
    for (int i = 0; i < depth; ++i) {
        MDEntry e;
        e.entry_type    = static_cast<int>(MDEntryType::Ask);
        e.update_action = static_cast<int>(MDUpdateAction::New);
        e.price         = best_ask + step * i;
        e.quantity      = static_cast<double>(depth - i) * 0.5 + 0.5;
        entries.push_back(e);
    }

    return entries;
}

// ─── Message generation ───────────────────────────────────────────────────────

void FIXFeedSimulator::emit_snapshot(SymbolState& s) {
    const double offset  = 1.0 + s.config.price_offset_bps / 10000.0;
    const double mid     = s.current_mid * offset;
    const double step    = (s.config.depth_step > 0.0)
                               ? s.config.depth_step
                               : mid * 0.0005;   // auto: 0.05% of mid

    auto entries = make_levels(mid, s.config.spread_pct, step,
                               s.config.depth_levels);

    std::string raw = FIXParser::build_snapshot(
        s.config.symbol, entries, s.seq_num++);
    dispatch(raw);
}

void FIXFeedSimulator::emit_incremental(SymbolState& s) {
    const double offset = 1.0 + s.config.price_offset_bps / 10000.0;
    const double mid    = s.current_mid * offset;
    const double step   = (s.config.depth_step > 0.0)
                              ? s.config.depth_step
                              : mid * 0.0005;

    // Pick a random side and level to update
    std::uniform_int_distribution<int>    side_dist(0, 1);
    std::uniform_int_distribution<int>    level_dist(0, s.config.depth_levels - 1);
    std::uniform_real_distribution<double> qty_delta(-0.15, 0.15);

    const int    side      = side_dist(s.rng);
    const int    level     = level_dist(s.rng);
    const double half_spd  = s.config.spread_pct / 2.0;
    const double best      = (side == 0)
                                 ? mid * (1.0 - half_spd)   // bid
                                 : mid * (1.0 + half_spd);  // ask
    const int    step_sign = (side == 0) ? -1 : +1;

    // Base quantity for this level (matches make_levels formula)
    const double base_qty = static_cast<double>(s.config.depth_levels - level) * 0.5 + 0.5;

    MDEntry e;
    e.entry_type    = side;  // 0=Bid, 1=Ask
    e.update_action = static_cast<int>(MDUpdateAction::Change);
    e.price         = best + step_sign * step * level;
    e.quantity      = std::max(0.1, base_qty + qty_delta(s.rng));

    std::string raw = FIXParser::build_incremental(
        s.config.symbol, {e}, s.seq_num++);
    dispatch(raw);
}

// ─── Dispatch ─────────────────────────────────────────────────────────────────

void FIXFeedSimulator::dispatch(const std::string& raw) {
    // raw lives on the caller's stack; msg.string_views point into raw.
    // Both are alive for the entire callback invocation.
    FIXMessage msg;
    if (parser_.parse(raw, msg)) {
        callback_(raw, msg);
    }
}
