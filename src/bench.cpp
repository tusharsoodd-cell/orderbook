// bench: measures per-message order book processing latency.
//
// usage:
//   bench                  run a synthetic in-memory workload (1M ops)
//   bench feed.obf         replay a feed file instead
//   bench --n 5000000      synthetic workload with N ops
//
// prints: message count, wall time, events/sec, p50/p99/p999 latency in ns,
// and a log2 histogram. latencies include the clock read itself (~20ns here),
// so treat the low end as an upper bound.

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "book.h"
#include "feed.h"

namespace {

uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
}

struct Op {
    char type;  // 'A' add, 'X' cancel, 'M' market
    uint64_t id;
    uint8_t side;
    int64_t price;
    int64_t qty;
};

// deterministic synthetic workload: mostly adds around a random-walking
// mid price, some cancels, some market orders. seed fixed so runs compare.
std::vector<Op> gen_workload(size_t n) {
    std::mt19937_64 rng(42);
    std::vector<Op> ops;
    ops.reserve(n);
    std::vector<uint64_t> live_ids;
    live_ids.reserve(n / 2);
    uint64_t next_id = 1;
    int64_t mid = 100000;  // ticks

    std::uniform_real_distribution<double> u01(0.0, 1.0);
    for (size_t i = 0; i < n; ++i) {
        double r = u01(rng);
        if (r < 0.65 || live_ids.empty()) {
            // add a limit order near the touch so some of them match
            int64_t price = mid + static_cast<int64_t>(rng() % 21) - 10;
            if (price <= 0) price = 1;
            int64_t qty = 1 + static_cast<int64_t>(rng() % 100);
            uint8_t side = static_cast<uint8_t>(rng() % 2);
            ops.push_back({'A', next_id, side, price, qty});
            live_ids.push_back(next_id);
            ++next_id;
        } else if (r < 0.85) {
            // cancel a random live order
            size_t idx = rng() % live_ids.size();
            ops.push_back({'X', live_ids[idx], 0, 0, 0});
            live_ids[idx] = live_ids.back();
            live_ids.pop_back();
        } else {
            // market order
            uint8_t side = static_cast<uint8_t>(rng() % 2);
            int64_t qty = 1 + static_cast<int64_t>(rng() % 50);
            ops.push_back({'M', next_id, side, 0, qty});
            ++next_id;
        }
        if (i % 1000 == 0) mid += static_cast<int64_t>(rng() % 7) - 3;
        if (mid <= 0) mid = 1;
    }
    return ops;
}

int log2_bucket(uint64_t v) {
    int b = 0;
    while (v >>= 1) ++b;
    return b;
}

void report(const std::vector<uint64_t>& lat, const char* what) {
    if (lat.empty()) {
        std::printf("no messages processed\n");
        return;
    }
    std::vector<uint64_t> s = lat;
    std::sort(s.begin(), s.end());
    uint64_t total = 0;
    for (uint64_t v : s) total += v;

    auto pct = [&](double p) { return s[static_cast<size_t>(p * (s.size() - 1))]; };

    std::printf("workload:      %s\n", what);
    std::printf("messages:      %zu\n", s.size());
    std::printf("total time:    %.3f s\n", total / 1e9);
    std::printf("throughput:    %.1f events/sec\n", s.size() / (total / 1e9));
    std::printf("p50 latency:   %" PRIu64 " ns\n", pct(0.50));
    std::printf("p99 latency:   %" PRIu64 " ns\n", pct(0.99));
    std::printf("p999 latency:  %" PRIu64 " ns\n", pct(0.999));
    std::printf("max latency:   %" PRIu64 " ns\n", s.back());

    // log2 histogram, buckets 2^4 .. 2^20 ns
    const int LO = 4, HI = 20;
    std::vector<uint64_t> buckets(HI - LO + 2, 0);
    for (uint64_t v : s) {
        int b = log2_bucket(v);
        if (b < LO) b = LO;
        if (b > HI + 1) b = HI + 1;
        buckets[b - LO]++;
    }
    std::printf("\nlatency histogram (ns, log2 buckets):\n");
    for (int b = LO; b <= HI; ++b) {
        uint64_t c = buckets[b - LO];
        int bars = static_cast<int>(50.0 * c / s.size());
        std::printf("  2^%-2d [%7" PRIu64 ", %7" PRIu64 ")  %8" PRIu64 "  ",
                    b, static_cast<uint64_t>(1ULL << b),
                    static_cast<uint64_t>(1ULL << (b + 1)), c);
        for (int i = 0; i < bars; ++i) std::putchar('#');
        std::printf("\n");
    }
    std::printf("  overflow (>2^%d)  %8" PRIu64 "\n", HI, buckets[HI - LO + 1]);
}

}  // namespace

int main(int argc, char** argv) {
    std::string feed_path;
    size_t n = 1000000;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--n" && i + 1 < argc) {
            n = static_cast<size_t>(std::stoull(argv[++i]));
        } else {
            feed_path = a;
        }
    }

    ob::OrderBook book;
    std::vector<uint64_t> lat;
    lat.reserve(feed_path.empty() ? n : 1024);

    if (!feed_path.empty()) {
        std::string err;
        if (!ob::replay_feed(feed_path, book, lat, err)) {
            std::fprintf(stderr, "replay failed: %s\n", err.c_str());
            return 1;
        }
        char what[256];
        std::snprintf(what, sizeof(what), "feed file %s", feed_path.c_str());
        report(lat, what);
    } else {
        auto ops = gen_workload(n);
        for (const auto& op : ops) {
            uint64_t t0 = now_ns();
            if (op.type == 'A') {
                book.add_limit(op.id, static_cast<ob::Side>(op.side), op.price, op.qty);
            } else if (op.type == 'X') {
                book.cancel(op.id);
            } else {
                book.add_market(op.id, static_cast<ob::Side>(op.side), op.qty);
            }
            lat.push_back(now_ns() - t0);
        }
        char what[64];
        std::snprintf(what, sizeof(what), "synthetic (%zu ops, seed 42)", n);
        report(lat, what);
    }
    return 0;
}
