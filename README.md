# orderbook

A limit order book and matching engine in C++17, with a binary market-data feed parser and a latency benchmark harness. Zero dependencies — just the standard library and CMake.

I built this to learn how matching engines actually work under the hood, and because it's the closest thing to a real HFT interview task you can put on a resume. Prices are integer ticks throughout (floats and money don't mix).

## How the matching works

- **Price-time priority**: resting orders sit in FIFO queues grouped by price level. Incoming orders match against the best opposing price first, and within a price level the earliest order fills first.
- **Limit orders** match while the prices cross; whatever doesn't fill rests in the book.
- **Market orders** sweep the book until filled or the book is empty. Leftover quantity is discarded — it never rests.
- **Trades always print at the maker's price.** The resting order was there first, so it sets the price. This matches how real venues work.
- **Cancels** look up the order id in a hash map, then do a linear scan of its price level to remove it.

Data structures: `std::map<price, std::deque<Order>>` per side (bids sorted high→low, asks low→high), plus an `unordered_map<order_id, location>` for cancels. Deliberately simple — see [tradeoffs](#tradeoffs) for what production would do instead.

## The binary feed format (OBF1)

A tiny ITCH-style format, all integers little-endian:

```
magic: 4 bytes "OBF1"
then messages, each starting with a 1-byte type tag:
  'A'  add limit:   u64 timestamp_ns | u64 order_id | u8 side (0=bid, 1=ask)
                    | i64 price_ticks | i64 qty
  'X'  cancel:      u64 timestamp_ns | u64 order_id
  'M'  market:      u64 timestamp_ns | u64 order_id | u8 side | i64 qty
```

`tools/gen_feed.py` generates synthetic feed files so the repo is self-contained — no exchange data needed.

## Benchmarks

`bench` measures per-message processing latency with `high_resolution_clock`. Numbers below are real, measured on this machine: Intel Xeon Platinum 8321HC @ 1.40GHz, single thread, g++ 13.3 `-O2`. Latencies include the clock read itself (~20ns), so treat the low end as an upper bound.

**Synthetic workload** — 2M ops (65% adds, 20% cancels, 15% markets), deterministic seed:

| metric | value |
|---|---|
| throughput | ~2.08M events/sec |
| p50 latency | 331 ns |
| p99 latency | 2,117 ns |
| p999 latency | 13,742 ns |

**Feed replay** — 300k messages from `gen_feed.py`:

| metric | value |
|---|---|
| throughput | ~2.46M events/sec |
| p50 latency | 331 ns |
| p99 latency | 1,309 ns |
| p999 latency | 4,019 ns |

Latency histogram (synthetic run, log2 buckets, ns) — most messages land in the 128–512ns range; the tail is `std::map` node allocation and `std::vector` growth on the trade path:

```
  2^6  [     64,      128)     49471
  2^7  [    128,      256)    526575
  2^8  [    256,      512)    989879
  2^9  [    512,     1024)    365196
  2^10 [   1024,     2048)     48273
```

Honestly, ~2M events/sec is fine for a portfolio project and nowhere near production (real engines do 10M+ with single-digit microsecond tail). The gap is almost entirely allocation: every level touch can allocate a map node, and every trade allocates into a vector. See tradeoffs.

## Tradeoffs

- **Why `std::map` and not something faster?** Readability. The level count stays small in any sane test, so O(log n) level lookup is lost in the noise next to allocation costs. A production book uses a fixed-size array of price levels indexed by tick offset from a base price — O(1), zero allocation, but you have to handle the price range explicitly.
- **Cancel is O(depth at level)**, not O(1). I tried storing deque iterators in the id map for O(1) cancel; `std::deque` invalidates iterators on middle erase, so that was a bug farm. An intrusive list (order nodes linked directly, no separate container) is the real answer.
- **Trades are returned in a `std::vector`**, which allocates. Production would use a caller-provided buffer or a small static array.
- **No handling of out-of-order packets.** The replay assumes the feed is sequenced. A real feed handler tracks sequence numbers and requests retransmits on gaps (see INTERVIEW.md).
- **Single-threaded.** No locking at all — the book is not thread-safe by design.

## Build & test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build            # unit tests
python3 tools/fuzz.py build/replay --seeds 200 --ops 500   # fuzz vs python reference
./build/bench --n 2000000         # benchmark
python3 tools/gen_feed.py --n 300000 --out feed.obf && ./build/bench feed.obf
```

Correctness: 11 unit tests covering price/time priority, partial fills, cancels, market sweeps, and degenerate inputs — plus a fuzzer that diffs the C++ book against an independent Python reference implementation on 100k+ random operations. They agree exactly.

## Layout

```
src/book.h, src/book.cpp    the order book
src/feed.h, src/feed.cpp    OBF1 binary feed parser
src/bench.cpp               benchmark harness
tools/replay.cpp            fuzz driver (text ops -> trades + book dump)
tools/gen_feed.py           synthetic feed generator
tools/fuzz.py               differential fuzzer (C++ vs python reference)
tests/test_main.cpp         unit tests, no framework
```

## Limitations

- Not thread-safe, not crash-safe, no persistence.
- Feed parser trusts the file; corrupt input fails cleanly but there's no checksum.
- Integer ticks only — no fractional prices, no implied decimal places convention.
- The benchmark measures this code on this machine; your numbers will differ.
