#pragma once

// Replays a binary market-data feed file into an OrderBook.
//
// file format (all integers little-endian):
//   magic: 4 bytes "OBF1"
//   then a sequence of messages, each starting with a 1-byte type tag:
//     'A'  add limit order:  u64 timestamp_ns | u64 order_id | u8 side (0=bid, 1=ask)
//                            | i64 price_ticks | i64 qty
//     'X'  cancel order:     u64 timestamp_ns | u64 order_id
//     'M'  market order:     u64 timestamp_ns | u64 order_id | u8 side | i64 qty
//
// it's deliberately tiny. real ITCH/OUCH is the same idea with more fields.

#include <cstdint>
#include <string>
#include <vector>

#include "book.h"

namespace ob {

// Replays the whole file. latencies_ns gets one entry per message: the
// nanoseconds spent applying that message to the book (includes the clock
// read itself, ~20ns on my machine). returns false on corrupt input, with
// a human-readable reason in error.
bool replay_feed(const std::string& path, OrderBook& book,
                 std::vector<uint64_t>& latencies_ns, std::string& error);

}  // namespace ob
