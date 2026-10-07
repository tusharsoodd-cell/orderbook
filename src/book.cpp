#include "book.h"

namespace ob {

std::vector<Trade> OrderBook::add_limit(uint64_t id, Side side, int64_t price, int64_t qty) {
    std::vector<Trade> trades;
    if (id == 0 || price <= 0 || qty <= 0) return trades;
    if (id_map_.count(id)) return trades;  // duplicate id, ignore. don't be clever.

    if (side == Side::Bid) {
        trades = match_into(asks_, side, id, qty, false, price);
        if (qty > 0) {
            bids_[price].push_back(Order{id, side, price, qty, seq_++});
            id_map_[id] = Loc{side, price};
        }
    } else {
        trades = match_into(bids_, side, id, qty, false, price);
        if (qty > 0) {
            asks_[price].push_back(Order{id, side, price, qty, seq_++});
            id_map_[id] = Loc{side, price};
        }
    }
    return trades;
}

std::vector<Trade> OrderBook::add_market(uint64_t id, Side side, int64_t qty) {
    std::vector<Trade> trades;
    if (qty <= 0) return trades;
    // note: market order ids aren't tracked. they can't rest so there's
    // nothing to cancel later. (fuzz driver relies on unique ids anyway.)
    if (side == Side::Bid) {
        trades = match_into(asks_, side, id, qty, true, 0);
    } else {
        trades = match_into(bids_, side, id, qty, true, 0);
    }
    return trades;  // whatever's left of qty just disappears
}

bool OrderBook::cancel(uint64_t id) {
    auto it = id_map_.find(id);
    if (it == id_map_.end()) return false;
    Side side = it->second.side;
    int64_t price = it->second.price;
    bool ok;
    if (side == Side::Bid) {
        ok = cancel_from(bids_, price, id);
    } else {
        ok = cancel_from(asks_, price, id);
    }
    if (ok) id_map_.erase(it);
    return ok;
}

std::optional<int64_t> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<int64_t> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

int64_t OrderBook::qty_at(Side side, int64_t price) const {
    int64_t total = 0;
    if (side == Side::Bid) {
        auto it = bids_.find(price);
        if (it != bids_.end())
            for (const auto& o : it->second) total += o.qty;
    } else {
        auto it = asks_.find(price);
        if (it != asks_.end())
            for (const auto& o : it->second) total += o.qty;
    }
    return total;
}

std::vector<std::pair<int64_t, int64_t>> OrderBook::depth(Side side) const {
    std::vector<std::pair<int64_t, int64_t>> out;
    if (side == Side::Bid) {
        for (const auto& [price, queue] : bids_) {
            int64_t total = 0;
            for (const auto& o : queue) total += o.qty;
            out.emplace_back(price, total);
        }
    } else {
        for (const auto& [price, queue] : asks_) {
            int64_t total = 0;
            for (const auto& o : queue) total += o.qty;
            out.emplace_back(price, total);
        }
    }
    return out;
}

template <typename LevelMap>
std::vector<Trade> OrderBook::match_into(LevelMap& resting, Side taker_side,
                                         uint64_t taker_id, int64_t& qty,
                                         bool is_market, int64_t limit) {
    std::vector<Trade> trades;
    auto it = resting.begin();
    while (qty > 0 && it != resting.end()) {
        int64_t px = it->first;
        bool crosses = is_market ||
                       (taker_side == Side::Bid ? px <= limit : px >= limit);
        if (!crosses) break;

        auto& queue = it->second;
        while (qty > 0 && !queue.empty()) {
            Order& maker = queue.front();
            int64_t fill = qty < maker.qty ? qty : maker.qty;

            Trade t;
            if (taker_side == Side::Bid) {
                t.buy_id = taker_id;
                t.sell_id = maker.id;
            } else {
                t.buy_id = maker.id;
                t.sell_id = taker_id;
            }
            t.price = maker.price;  // maker was here first, maker sets the price
            t.qty = fill;
            trades.push_back(t);

            qty -= fill;
            maker.qty -= fill;
            if (maker.qty == 0) {
                id_map_.erase(maker.id);
                queue.pop_front();
            }
        }
        // erase() on a map returns the iterator to the next element.
        // took me a while to trust that, but it does (since c++11).
        if (queue.empty())
            it = resting.erase(it);
        else
            ++it;
    }
    return trades;
}

template <typename LevelMap>
bool OrderBook::cancel_from(LevelMap& levels, int64_t price, uint64_t id) {
    auto it = levels.find(price);
    if (it == levels.end()) return false;
    auto& queue = it->second;
    // linear scan of the level. levels are short in practice so this is fine.
    // O(1) cancel would need an intrusive list + storing iterators in id_map_,
    // but deque invalidates iterators on middle erase, so that was a bug farm.
    // (tried it. don't.)
    for (auto qit = queue.begin(); qit != queue.end(); ++qit) {
        if (qit->id == id) {
            queue.erase(qit);
            if (queue.empty()) levels.erase(it);
            return true;
        }
    }
    return false;  // shouldn't happen if id_map_ is consistent, but be safe
}

}  // namespace ob
