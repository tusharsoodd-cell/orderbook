#pragma once

// A minimal limit order book. price-time priority, integer ticks.
//
// this is a teaching/portfolio implementation, not production code.
// see README.md for what a real matching engine does differently.

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ob {

enum class Side : uint8_t { Bid = 0, Ask = 1 };

struct Order {
    uint64_t id = 0;
    Side side = Side::Bid;
    int64_t price = 0;  // ticks. limit orders only
    int64_t qty = 0;    // remaining quantity
    uint64_t seq = 0;   // arrival sequence number (time priority)
};

struct Trade {
    uint64_t buy_id = 0;
    uint64_t sell_id = 0;
    int64_t price = 0;  // ticks. always the resting (maker) price
    int64_t qty = 0;
};

class OrderBook {
public:
    OrderBook() = default;

    // Add a limit order. returns the trades it caused (maybe none).
    // duplicate ids, id == 0, and non-positive price/qty are silently ignored.
    std::vector<Trade> add_limit(uint64_t id, Side side, int64_t price, int64_t qty);

    // Add a market order. sweeps the book until filled or the book is empty.
    // leftover quantity is discarded, it never rests. id is not tracked.
    std::vector<Trade> add_market(uint64_t id, Side side, int64_t qty);

    // Cancel a resting order. returns false if the id isn't in the book.
    bool cancel(uint64_t id);

    std::optional<int64_t> best_bid() const;
    std::optional<int64_t> best_ask() const;

    // total resting qty at a price level (0 if the level is empty).
    // handy for tests, not on any hot path.
    int64_t qty_at(Side side, int64_t price) const;

    // (price, total qty) per level, best price first. for debugging/dumps.
    std::vector<std::pair<int64_t, int64_t>> depth(Side side) const;

    size_t resting_count() const { return id_map_.size(); }

private:
    // price -> fifo queue of resting orders.
    // bids sorted high to low, asks low to high.
    // std::map is O(log #levels) per touch. fine at this scale; a real book
    // would use a fixed-size array of levels indexed by tick offset.
    std::map<int64_t, std::deque<Order>, std::greater<int64_t>> bids_;
    std::map<int64_t, std::deque<Order>> asks_;

    struct Loc {
        Side side;
        int64_t price;
    };
    std::unordered_map<uint64_t, Loc> id_map_;  // resting order id -> where it lives

    uint64_t seq_ = 0;

    // match a taker against one side's levels. qty is in/out (leftover).
    // limit is ignored when is_market is true.
    template <typename LevelMap>
    std::vector<Trade> match_into(LevelMap& resting, Side taker_side,
                                  uint64_t taker_id, int64_t& qty,
                                  bool is_market, int64_t limit);

    template <typename LevelMap>
    bool cancel_from(LevelMap& levels, int64_t price, uint64_t id);
};

}  // namespace ob
