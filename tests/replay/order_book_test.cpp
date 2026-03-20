// order_book_test.cpp
//
// Deterministic regression test for the L2 OrderBook engine.
//
// Two test modes:
//
//   1. Synthetic (always runs):
//      Constructs known ITCH 5.0 binary messages, replays them through
//      ITCHReplay, feeds L2Events into OrderBook via set_level/delete_level,
//      and asserts the resulting book state is exactly correct.
//
//   2. FIX message path (always runs):
//      Builds FIX 35=W and 35=X messages via FIXParser::build_snapshot /
//      build_incremental, applies them via OrderBook::apply_snapshot /
//      apply_update, and verifies the same book state.
//
//   3. Real ITCH file (optional — pass path as argv[1]):
//      Replays a full NASDAQ ITCH 5.0 session file and reports the final
//      book state for the first traded symbol.  Use to verify correctness
//      against a real exchange session.
//      Download from: https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/
//
// Run:
//   ./order_book_test                          # synthetic + FIX tests only
//   ./order_book_test path/to/session.itch     # + real file replay

#include "itch_replay.hpp"
#include "order_book.hpp"
#include "fix_parser.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

// ─── Helpers ─────────────────────────────────────────────────────────────────

static bool approx_eq(double a, double b, double tol = 1e-6) {
    return std::abs(a - b) < tol;
}

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
        std::exit(1);
    }
}

static void print_book(const OrderBookSnapshot& snap) {
    std::cout << "  Symbol: " << snap.symbol
              << "  Exchange: " << snap.exchange << "\n";
    std::cout << "  BID                           ASK\n";

    const size_t rows = std::max(snap.bids.size(), snap.asks.size());
    for (size_t i = 0; i < rows; ++i) {
        if (i < snap.bids.size())
            std::cout << "  " << snap.bids[i].price
                      << " x " << snap.bids[i].quantity
                      << " (" << snap.bids[i].order_count << ")";
        else
            std::cout << "  " << std::string(30, ' ');

        std::cout << "    |    ";

        if (i < snap.asks.size())
            std::cout << snap.asks[i].price
                      << " x " << snap.asks[i].quantity
                      << " (" << snap.asks[i].order_count << ")";
        std::cout << "\n";
    }
    std::cout << "  Spread: " << snap.spread_bps() << " bps"
              << "  Mid: "    << snap.mid_price()  << "\n";
}

// ─── Test 1: ITCH synthetic replay ───────────────────────────────────────────
//
// Session:
//   Order 1: BUY  100 AAPL @ $150.00   ref=1
//   Order 2: SELL 200 AAPL @ $150.50   ref=2
//   Order 3: BUY   50 AAPL @ $149.50   ref=3
//   Order 4: SELL 100 AAPL @ $151.00   ref=4
//   Order 1 executed for 50 shares     (ref=1, partial)
//   Order 2 cancelled for 100 shares   (ref=2, partial)
//
// Expected L2 after all events:
//   BID: 150.00 × 50  (1 order)
//        149.50 × 50  (1 order)
//   ASK: 150.50 × 100 (1 order)
//        151.00 × 100 (1 order)

static void test_itch_synthetic() {
    std::cout << "\n=== Test 1: ITCH synthetic replay ===\n";

    OrderBook book("AAPL", "ITCH");
    itch::ITCHReplay replay;

    // Wire ITCH L2 events into OrderBook via the direct interface
    replay.set_callback([&](const itch::L2Event& ev) {
        if (ev.type == itch::L2Event::Type::Set)
            book.set_level(ev.side, ev.price, ev.quantity, ev.order_count);
        else
            book.delete_level(ev.side, ev.price);
    });

    // Replay synthetic events
    auto msg1 = itch::ITCHReplay::make_add_order(1, 'B', 100, "AAPL    ", 1500000);
    auto msg2 = itch::ITCHReplay::make_add_order(2, 'S', 200, "AAPL    ", 1505000);
    auto msg3 = itch::ITCHReplay::make_add_order(3, 'B',  50, "AAPL    ", 1495000);
    auto msg4 = itch::ITCHReplay::make_add_order(4, 'S', 100, "AAPL    ", 1510000);
    auto exec1 = itch::ITCHReplay::make_order_executed(1, 50);   // partial fill
    auto cxl2  = itch::ITCHReplay::make_order_cancel  (2, 100);  // partial cancel

    for (const auto& msg : {msg1, msg2, msg3, msg4, exec1, cxl2})
        replay.process_message(msg.data(), msg.size());

    const auto snap = book.get_snapshot();
    print_book(snap);

    // ── Assertions ──────────────────────────────────────────────────────────
    check(snap.bid_depth() == 2, "bid depth == 2");
    check(snap.ask_depth() == 2, "ask depth == 2");

    // Best bid: 150.00 × 50
    check(approx_eq(snap.best_bid(), 150.00), "best bid == 150.00");
    check(approx_eq(snap.bids[0].quantity, 50.0), "bid[0] qty == 50");
    check(snap.bids[0].order_count == 1, "bid[0] order_count == 1");

    // Second bid: 149.50 × 50
    check(approx_eq(snap.bids[1].price, 149.50), "bid[1] price == 149.50");
    check(approx_eq(snap.bids[1].quantity, 50.0), "bid[1] qty == 50");

    // Best ask: 150.50 × 100
    check(approx_eq(snap.best_ask(), 150.50), "best ask == 150.50");
    check(approx_eq(snap.asks[0].quantity, 100.0), "ask[0] qty == 100");

    // Second ask: 151.00 × 100
    check(approx_eq(snap.asks[1].price, 151.00), "ask[1] price == 151.00");
    check(approx_eq(snap.asks[1].quantity, 100.0), "ask[1] qty == 100");

    // Spread and mid
    check(approx_eq(snap.spread_bps(),
                    (150.50 - 150.00) / 150.00 * 10'000.0, 0.01),
          "spread_bps correct");
    check(approx_eq(snap.mid_price(), (150.00 + 150.50) / 2.0), "mid_price correct");

    std::cout << "PASS\n";
}

// ─── Test 2: ITCH order delete ────────────────────────────────────────────────
//
// Add two orders at the same price level, delete one, verify level halved.
// Delete both, verify level removed.

static void test_itch_order_delete() {
    std::cout << "\n=== Test 2: ITCH order delete / level removal ===\n";

    OrderBook book("MSFT", "ITCH");
    itch::ITCHReplay replay;

    replay.set_callback([&](const itch::L2Event& ev) {
        if (ev.type == itch::L2Event::Type::Set)
            book.set_level(ev.side, ev.price, ev.quantity, ev.order_count);
        else
            book.delete_level(ev.side, ev.price);
    });

    // Two buy orders at the same price
    auto a1 = itch::ITCHReplay::make_add_order(10, 'B', 300, "MSFT    ", 2000000);
    auto a2 = itch::ITCHReplay::make_add_order(11, 'B', 200, "MSFT    ", 2000000);
    // One sell order
    auto a3 = itch::ITCHReplay::make_add_order(12, 'S', 100, "MSFT    ", 2005000);

    replay.process_message(a1.data(), a1.size());
    replay.process_message(a2.data(), a2.size());
    replay.process_message(a3.data(), a3.size());

    check(approx_eq(book.best_bid(), 200.00), "best bid == 200.00");
    check(approx_eq(book.get_snapshot().bids[0].quantity, 500.0),
          "combined bid qty == 500");
    check(book.get_snapshot().bids[0].order_count == 2, "order_count == 2");

    // Delete order 10 (300 shares) — level should remain with 200 shares
    auto d1 = itch::ITCHReplay::make_order_delete(10);
    replay.process_message(d1.data(), d1.size());

    check(approx_eq(book.get_snapshot().bids[0].quantity, 200.0),
          "after delete: bid qty == 200");
    check(book.get_snapshot().bids[0].order_count == 1,
          "after delete: order_count == 1");

    // Delete order 11 — level should disappear entirely
    auto d2 = itch::ITCHReplay::make_order_delete(11);
    replay.process_message(d2.data(), d2.size());

    check(book.bid_depth() == 0, "after both deletes: bid depth == 0");
    check(book.ask_depth() == 1, "ask depth still 1");

    std::cout << "PASS\n";
}

// ─── Test 3: FIX 35=W snapshot path ──────────────────────────────────────────

static void test_fix_snapshot() {
    std::cout << "\n=== Test 3: FIX 35=W apply_snapshot ===\n";

    OrderBook book("BTCUSD", "FIX");
    FIXParser  parser;

    const std::string raw = FIXParser::build_snapshot("BTCUSD", {
        { static_cast<int>(MDEntryType::Bid), 0, 101.250, 500  },
        { static_cast<int>(MDEntryType::Bid), 0, 101.000, 1200 },
        { static_cast<int>(MDEntryType::Bid), 0, 100.750,  800 },
        { static_cast<int>(MDEntryType::Ask), 0, 101.500,  300 },
        { static_cast<int>(MDEntryType::Ask), 0, 101.750,  600 },
        { static_cast<int>(MDEntryType::Ask), 0, 102.000,  400 },
    });

    FIXMessage msg;
    check(parser.parse(raw, msg),          "35=W parsed");
    check(msg.msg_type == FIXMsgType::MarketDataSnapshot, "msg_type == W");

    book.apply_snapshot(msg);

    const auto snap = book.get_snapshot();
    print_book(snap);

    check(snap.bid_depth() == 3,                         "bid depth == 3");
    check(snap.ask_depth() == 3,                         "ask depth == 3");
    check(approx_eq(snap.best_bid(), 101.250),           "best bid == 101.250");
    check(approx_eq(snap.best_ask(), 101.500),           "best ask == 101.500");
    check(approx_eq(snap.bids[0].quantity,   500.0),     "bid[0] qty == 500");
    check(approx_eq(snap.bids[1].quantity,  1200.0),     "bid[1] qty == 1200");
    check(approx_eq(snap.asks[0].quantity,   300.0),     "ask[0] qty == 300");
    check(approx_eq(snap.mid_price(), (101.250 + 101.500) / 2.0), "mid_price");

    check(book.snapshot_count() == 1, "snapshot_count == 1");
    check(book.update_count()   == 0, "update_count == 0");

    std::cout << "PASS\n";
}

// ─── Test 4: FIX 35=X incremental path ───────────────────────────────────────

static void test_fix_incremental() {
    std::cout << "\n=== Test 4: FIX 35=X apply_update ===\n";

    // Start from the snapshot in Test 3
    OrderBook book("BTCUSD", "FIX");
    FIXParser  parser;

    const std::string snap_raw = FIXParser::build_snapshot("BTCUSD", {
        { static_cast<int>(MDEntryType::Bid), 0, 101.250, 500 },
        { static_cast<int>(MDEntryType::Ask), 0, 101.500, 300 },
    });
    FIXMessage snap_msg;
    parser.parse(snap_raw, snap_msg);
    book.apply_snapshot(snap_msg);

    // Incremental: change bid qty, delete ask, add new ask level
    const std::string incr_raw = FIXParser::build_incremental("BTCUSD", {
        // Change: bid 101.250 qty → 750
        { static_cast<int>(MDEntryType::Bid),
          static_cast<int>(MDUpdateAction::Change), 101.250, 750 },
        // Delete: ask 101.500
        { static_cast<int>(MDEntryType::Ask),
          static_cast<int>(MDUpdateAction::Delete), 101.500, 0 },
        // New: ask 101.600 qty 450
        { static_cast<int>(MDEntryType::Ask),
          static_cast<int>(MDUpdateAction::New),    101.600, 450 },
    });

    FIXMessage incr_msg;
    check(parser.parse(incr_raw, incr_msg),                "35=X parsed");
    check(incr_msg.msg_type == FIXMsgType::MarketDataIncremental, "msg_type == X");

    book.apply_update(incr_msg);

    const auto snap = book.get_snapshot();
    print_book(snap);

    check(snap.bid_depth() == 1,                      "bid depth == 1");
    check(snap.ask_depth() == 1,                      "ask depth == 1");
    check(approx_eq(snap.best_bid(),    101.250),     "best bid unchanged");
    check(approx_eq(snap.bids[0].quantity, 750.0),    "bid qty updated to 750");
    check(approx_eq(snap.best_ask(),    101.600),     "new best ask == 101.600");
    check(approx_eq(snap.asks[0].quantity, 450.0),    "new ask qty == 450");

    check(book.snapshot_count() == 1, "snapshot_count == 1");
    check(book.update_count()   == 1, "update_count == 1");

    std::cout << "PASS\n";
}

// ─── Test 5: Snapshot replaces previous state ─────────────────────────────────

static void test_snapshot_replaces() {
    std::cout << "\n=== Test 5: snapshot replaces previous book state ===\n";

    OrderBook book("ETHUSDT", "FIX");
    FIXParser  parser;

    // First snapshot: 3 levels
    auto raw1 = FIXParser::build_snapshot("ETHUSDT", {
        { static_cast<int>(MDEntryType::Bid), 0, 3000.0, 10 },
        { static_cast<int>(MDEntryType::Bid), 0, 2999.0, 20 },
        { static_cast<int>(MDEntryType::Ask), 0, 3001.0, 15 },
    });
    FIXMessage msg1;
    parser.parse(raw1, msg1);
    book.apply_snapshot(msg1);

    check(book.bid_depth() == 2, "after first snap: bid depth == 2");

    // Second snapshot: only 1 bid level — first snapshot must be fully replaced
    auto raw2 = FIXParser::build_snapshot("ETHUSDT", {
        { static_cast<int>(MDEntryType::Bid), 0, 3000.0, 5 },
        { static_cast<int>(MDEntryType::Ask), 0, 3001.5, 8 },
    });
    FIXMessage msg2;
    parser.parse(raw2, msg2);
    book.apply_snapshot(msg2);

    check(book.bid_depth() == 1, "after second snap: bid depth == 1 (old levels gone)");
    check(approx_eq(book.get_snapshot().bids[0].quantity, 5.0),
          "bid qty from second snapshot");

    std::cout << "PASS\n";
}

// ─── Test 6: Real ITCH file (optional) ───────────────────────────────────────

static void test_real_itch_file(const std::string& path) {
    std::cout << "\n=== Test 6: Real ITCH file replay: " << path << " ===\n";

    OrderBook book("", "ITCH");
    itch::ITCHReplay replay;

    std::string first_symbol;

    replay.set_callback([&](const itch::L2Event& ev) {
        if (first_symbol.empty()) first_symbol = ev.stock;
        if (ev.stock != first_symbol) return;

        if (ev.type == itch::L2Event::Type::Set)
            book.set_level(ev.side, ev.price, ev.quantity, ev.order_count);
        else
            book.delete_level(ev.side, ev.price);
    });

    const size_t count = replay.replay_file(path);

    std::cout << "  Messages processed: " << count << "\n";
    std::cout << "  Add: "     << replay.add_count()
              << "  Execute: " << replay.execute_count()
              << "  Cancel: "  << replay.cancel_count()
              << "  Delete: "  << replay.delete_count() << "\n";

    if (!first_symbol.empty()) {
        std::cout << "  Final L2 book for first symbol: " << first_symbol << "\n";
        const auto snap = book.get_snapshot();
        snap.bids.empty()
            ? std::cout << "  (empty bids)\n"
            : std::cout << "  Best bid: " << snap.best_bid()
                        << " x " << snap.bids.front().quantity << "\n";
        snap.asks.empty()
            ? std::cout << "  (empty asks)\n"
            : std::cout << "  Best ask: " << snap.best_ask()
                        << " x " << snap.asks.front().quantity << "\n";
        std::cout << "  Bid depth: " << snap.bid_depth()
                  << "  Ask depth: " << snap.ask_depth() << "\n";
    }

    std::cout << "PASS (no assertions — visual inspection required)\n";
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::cout << "OrderBook regression tests\n";
    std::cout << "==========================\n";

    try {
        test_itch_synthetic();
        test_itch_order_delete();
        test_fix_snapshot();
        test_fix_incremental();
        test_snapshot_replaces();

        if (argc >= 2)
            test_real_itch_file(argv[1]);

    } catch (const std::exception& e) {
        std::cerr << "EXCEPTION: " << e.what() << "\n";
        return 1;
    }

    std::cout << "\n===========================\n";
    std::cout << "All tests PASSED\n";
    return 0;
}
