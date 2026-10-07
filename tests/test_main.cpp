// unit tests. no framework on purpose — a handful of asserts and a
// main() is all this needs, and it keeps the repo dependency-free.

#include <cstdio>
#include <vector>

#include "book.h"

static int failures = 0;

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            ++failures;                                                          \
        }                                                                       \
    } while (0)

using ob::OrderBook;
using ob::Side;
using ob::Trade;

static void test_empty_book() {
    OrderBook b;
    CHECK(!b.best_bid().has_value());
    CHECK(!b.best_ask().has_value());
    CHECK(!b.cancel(123));
    CHECK(b.add_market(1, Side::Bid, 10).empty());  // nothing to sweep
    CHECK(b.resting_count() == 0);
}

static void test_simple_match() {
    OrderBook b;
    CHECK(b.add_limit(1, Side::Ask, 100, 10).empty());  // rests
    auto trades = b.add_limit(2, Side::Bid, 100, 5);    // crosses
    CHECK(trades.size() == 1);
    CHECK(trades[0].buy_id == 2 && trades[0].sell_id == 1);
    CHECK(trades[0].price == 100);  // maker price
    CHECK(trades[0].qty == 5);
    CHECK(b.qty_at(Side::Ask, 100) == 5);  // remainder still resting
    CHECK(b.resting_count() == 1);
}

static void test_price_priority() {
    OrderBook b;
    b.add_limit(1, Side::Ask, 102, 10);
    b.add_limit(2, Side::Ask, 100, 10);  // best ask
    b.add_limit(3, Side::Ask, 101, 10);
    auto trades = b.add_limit(4, Side::Bid, 105, 25);
    CHECK(trades.size() == 3);
    CHECK(trades[0].price == 100);  // cheapest first
    CHECK(trades[1].price == 101);
    CHECK(trades[2].price == 102);
    CHECK(trades[2].qty == 5);  // partial fill on the last level
    CHECK(b.qty_at(Side::Ask, 102) == 5);
}

static void test_time_priority() {
    OrderBook b;
    b.add_limit(1, Side::Ask, 100, 10);  // first at this level
    b.add_limit(2, Side::Ask, 100, 10);  // second
    auto trades = b.add_limit(3, Side::Bid, 100, 12);
    CHECK(trades.size() == 2);
    CHECK(trades[0].sell_id == 1);  // earlier order fills first
    CHECK(trades[0].qty == 10);
    CHECK(trades[1].sell_id == 2);
    CHECK(trades[1].qty == 2);
    CHECK(b.qty_at(Side::Ask, 100) == 8);
}

static void test_no_cross_rests() {
    OrderBook b;
    b.add_limit(1, Side::Ask, 100, 10);
    CHECK(b.add_limit(2, Side::Bid, 99, 10).empty());  // doesn't touch
    CHECK(b.resting_count() == 2);
    auto bid = b.best_bid();
    auto ask = b.best_ask();
    CHECK(bid.has_value() && *bid == 99);
    CHECK(ask.has_value() && *ask == 100);
}

static void test_cancel() {
    OrderBook b;
    b.add_limit(1, Side::Bid, 100, 10);
    b.add_limit(2, Side::Bid, 100, 10);
    CHECK(b.cancel(1));
    CHECK(!b.cancel(1));      // already gone
    CHECK(!b.cancel(999));    // never existed
    CHECK(b.qty_at(Side::Bid, 100) == 10);
    CHECK(b.resting_count() == 1);
    // cancel the last order at a level removes the level
    CHECK(b.cancel(2));
    CHECK(!b.best_bid().has_value());
}

static void test_cancel_after_partial_fill() {
    OrderBook b;
    b.add_limit(1, Side::Ask, 100, 10);
    b.add_limit(2, Side::Bid, 100, 6);
    CHECK(b.qty_at(Side::Ask, 100) == 4);
    CHECK(b.cancel(1));
    CHECK(b.qty_at(Side::Ask, 100) == 0);
    CHECK(b.resting_count() == 0);
}

static void test_market_sweeps_levels() {
    OrderBook b;
    b.add_limit(1, Side::Ask, 100, 5);
    b.add_limit(2, Side::Ask, 101, 5);
    b.add_limit(3, Side::Ask, 102, 5);
    auto trades = b.add_market(9, Side::Bid, 12);
    CHECK(trades.size() == 3);
    CHECK(trades[0].price == 100 && trades[0].qty == 5);
    CHECK(trades[1].price == 101 && trades[1].qty == 5);
    CHECK(trades[2].price == 102 && trades[2].qty == 2);
    CHECK(b.qty_at(Side::Ask, 102) == 3);
    // over-sized market order: leftover just vanishes
    auto trades2 = b.add_market(10, Side::Bid, 1000);
    CHECK(!b.best_ask().has_value());
    int64_t filled = 0;
    for (const auto& t : trades2) filled += t.qty;
    CHECK(filled == 3);
}

static void test_degenerate_orders_ignored() {
    OrderBook b;
    CHECK(b.add_limit(0, Side::Bid, 100, 10).empty());    // id 0
    CHECK(b.add_limit(1, Side::Bid, 0, 10).empty());     // bad price
    CHECK(b.add_limit(1, Side::Bid, -5, 10).empty());    // bad price
    CHECK(b.add_limit(1, Side::Bid, 100, 0).empty());     // zero qty
    CHECK(b.resting_count() == 0);
    b.add_limit(1, Side::Bid, 100, 10);
    CHECK(b.add_limit(1, Side::Bid, 100, 10).empty());    // duplicate id
    CHECK(b.resting_count() == 1);
    CHECK(b.qty_at(Side::Bid, 100) == 10);
}

static void test_sell_side_mirror() {
    // same as test_simple_match but flipped, to catch side mixups
    OrderBook b;
    CHECK(b.add_limit(1, Side::Bid, 100, 10).empty());
    auto trades = b.add_limit(2, Side::Ask, 100, 4);
    CHECK(trades.size() == 1);
    CHECK(trades[0].buy_id == 1 && trades[0].sell_id == 2);
    CHECK(trades[0].price == 100 && trades[0].qty == 4);
    CHECK(b.qty_at(Side::Bid, 100) == 6);
}

static void test_depth_snapshot() {
    OrderBook b;
    b.add_limit(1, Side::Bid, 101, 3);
    b.add_limit(2, Side::Bid, 100, 5);
    b.add_limit(3, Side::Bid, 101, 7);
    auto d = b.depth(Side::Bid);
    CHECK(d.size() == 2);
    CHECK(d[0].first == 101 && d[0].second == 10);  // best first, aggregated
    CHECK(d[1].first == 100 && d[1].second == 5);
}

int main() {
    test_empty_book();
    test_simple_match();
    test_price_priority();
    test_time_priority();
    test_no_cross_rests();
    test_cancel();
    test_cancel_after_partial_fill();
    test_market_sweeps_levels();
    test_degenerate_orders_ignored();
    test_sell_side_mirror();
    test_depth_snapshot();

    if (failures == 0) {
        std::printf("all tests passed\n");
        return 0;
    }
    std::printf("%d test(s) failed\n", failures);
    return 1;
}
