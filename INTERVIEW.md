# INTERVIEW.md — questions I expect about this project

**How do you avoid allocations on the hot path?**

Honestly: I don't, not fully. The matching loop itself doesn't allocate — it walks existing deques — but touching a new price level allocates a `std::map` node, and returning trades allocates a `std::vector`. That's where the p99 tail comes from (see the benchmark histogram). The real fix: preallocated object pools for orders, a fixed array of price levels instead of a map, and trades written into a caller-provided buffer. I kept the simple version because the goal here was a correct, readable book, and the benchmark shows exactly what the simple version costs.

**What breaks under out-of-order packets?**

The replay assumes messages arrive in sequence. In production, every message carries a sequence number; the feed handler detects gaps and either requests a retransmit or, if it's too late, rebuilds the book from a snapshot. Applying a cancel for an order that hasn't arrived yet would silently no-op here and corrupt the book. The timestamps are parsed and stored in the file format but currently unused — gap detection would start there.

**Why integer ticks?**

Because floating point and money don't mix. 0.1 + 0.2 != 0.3, and in a matching engine that kind of error means printing trades at prices that don't exist. Every venue works in integer ticks (or integerized decimals) for exactly this reason. It also makes price levels hashable/comparable for free.

**How would you make cancel O(1)?**

Right now cancel is hash-map lookup plus a linear scan of the price level's queue. To make it O(1): use an intrusive doubly-linked list per level (the order nodes themselves are the list nodes, no separate container), and store the list iterator/node pointer in the id map. I actually tried storing `std::deque` iterators first — deque invalidates iterators on middle erase, so that was broken. Intrusive lists don't have that problem because erasing a node only touches its neighbors.

**Why price-time priority and not pro-rata?**

Price-time is what most equity venues use and it's the simplest to reason about. Pro-rata (splitting fills proportionally across orders at a level) is common in futures. The matching core here would need a second pass over the level for pro-rata; the data structures wouldn't change.

**What's the biggest correctness risk in this code?**

The matching loop mutating the level map while iterating it — erasing an empty level mid-iteration. `std::map::erase` returns the next iterator, which is what the code relies on. The fuzzer (100k random ops diffed against an independent Python implementation) is what gives me confidence there, more than the unit tests.
