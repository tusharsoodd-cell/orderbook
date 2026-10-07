// replay: fuzz driver. reads a text op log, applies it to the book,
// prints every trade and the final book snapshot in a stable text format.
//
// op log format (one per line):
//   A <id> <B|A> <price> <qty>    add limit order
//   M <id> <B|A> <qty>            market order
//   C <id>                        cancel
//
// output:
//   T <buy_id> <sell_id> <price> <qty>     (one per trade, in order)
//   BOOK
//   B <price> <qty>                        (bids, best first)
//   A <price> <qty>                        (asks, best first)
//
// tools/fuzz.py diffs this against a python reference implementation.

#include <cinttypes>
#include <cstdio>
#include <string>

#include "book.h"

int main(int argc, char** argv) {
    FILE* f = stdin;
    if (argc > 1) {
        f = std::fopen(argv[1], "r");
        if (!f) {
            std::fprintf(stderr, "can't open %s\n", argv[1]);
            return 1;
        }
    }

    ob::OrderBook book;
    char kind;
    while (std::fscanf(f, " %c", &kind) == 1) {
        if (kind == 'A') {
            uint64_t id;
            char s;
            int64_t price, qty;
            if (std::fscanf(f, "%" SCNu64 " %c %" SCNd64 " %" SCNd64, &id, &s, &price, &qty) != 4) break;
            auto side = (s == 'B') ? ob::Side::Bid : ob::Side::Ask;
            for (const auto& t : book.add_limit(id, side, price, qty))
                std::printf("T %" PRIu64 " %" PRIu64 " %" PRId64 " %" PRId64 "\n",
                            t.buy_id, t.sell_id, t.price, t.qty);
        } else if (kind == 'M') {
            uint64_t id;
            char s;
            int64_t qty;
            if (std::fscanf(f, "%" SCNu64 " %c %" SCNd64, &id, &s, &qty) != 3) break;
            auto side = (s == 'B') ? ob::Side::Bid : ob::Side::Ask;
            for (const auto& t : book.add_market(id, side, qty))
                std::printf("T %" PRIu64 " %" PRIu64 " %" PRId64 " %" PRId64 "\n",
                            t.buy_id, t.sell_id, t.price, t.qty);
        } else if (kind == 'C') {
            uint64_t id;
            if (std::fscanf(f, "%" SCNu64, &id) != 1) break;
            book.cancel(id);
        } else {
            std::fprintf(stderr, "bad op '%c'\n", kind);
            return 1;
        }
    }

    std::printf("BOOK\n");
    for (const auto& [price, qty] : book.depth(ob::Side::Bid))
        std::printf("B %" PRId64 " %" PRId64 "\n", price, qty);
    for (const auto& [price, qty] : book.depth(ob::Side::Ask))
        std::printf("A %" PRId64 " %" PRId64 "\n", price, qty);
    return 0;
}
