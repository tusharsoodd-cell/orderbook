#!/usr/bin/env python3
"""Generate a synthetic OBF1 binary feed file for bench/replay demos.

usage: python3 gen_feed.py [--n 200000] [--out feed.obf] [--seed 7]

writes the format documented in src/feed.h: magic "OBF1" then messages.
prices are integer ticks around 100000 with a slow random walk so the
book stays reasonably tight and matching actually happens.
"""

import argparse
import random
import struct
import sys

MAGIC = b"OBF1"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=200000)
    ap.add_argument("--out", default="feed.obf")
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    next_id = 1
    live = []  # ids currently resting (approximately; cancels may miss)
    mid = 100000
    ts = 1_700_000_000_000_000_000  # fake ns epoch, we don't care

    n_add = n_x = n_mkt = 0
    with open(args.out, "wb") as f:
        f.write(MAGIC)
        for i in range(args.n):
            ts += rng.randint(50, 5000)  # 50ns..5us between messages
            r = rng.random()
            if r < 0.65 or not live:
                price = mid + rng.randint(-10, 10)
                price = max(price, 1)
                qty = rng.randint(1, 100)
                side = rng.randint(0, 1)
                f.write(struct.pack("<BQQBqq", 0x41, ts, next_id, side, price, qty))
                live.append(next_id)
                next_id += 1
                n_add += 1
            elif r < 0.85:
                oid = rng.choice(live)
                f.write(struct.pack("<BQQ", 0x58, ts, oid))  # 'X'
                # drop it from live tracking most of the time; occasional
                # double-cancel is fine, the book just ignores it
                if rng.random() < 0.9:
                    live.remove(oid)
                n_x += 1
            else:
                side = rng.randint(0, 1)
                qty = rng.randint(1, 50)
                f.write(struct.pack("<BQQBq", 0x4D, ts, next_id, side, qty))
                next_id += 1
                n_mkt += 1
            if i % 500 == 0:
                mid = max(1, mid + rng.randint(-3, 3))

    print(f"wrote {args.out}: {args.n} messages "
          f"({n_add} add, {n_x} cancel, {n_mkt} market)")


if __name__ == "__main__":
    sys.exit(main())
