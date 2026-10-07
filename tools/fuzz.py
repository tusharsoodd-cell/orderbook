#!/usr/bin/env python3
"""Fuzz the C++ order book against a dead-simple python reference.

usage: python3 fuzz.py /path/to/replay [--seeds 200] [--ops 500]

generates random op sequences, runs both implementations, and diffs the
exact trade stream + final book snapshot. any mismatch prints the failing
seed and the first differing lines, then exits non-zero.

the reference below is intentionally naive (dicts and lists) so it's easy
to audit by eye. if the reference and the C++ disagree, fix whichever one
is actually wrong — the reference has been wrong before.
"""

import argparse
import random
import subprocess
import sys
from collections import deque

BID, ASK = 0, 1


class RefBook:
    """price-time priority book. mirrors src/book.cpp semantics exactly:
    integer ticks, maker price on trades, duplicate/degenerate orders ignored,
    market leftovers discarded, cancel of unknown id is a no-op."""

    def __init__(self):
        self.bids = {}  # price -> deque of [id, qty]
        self.asks = {}
        self.loc = {}  # id -> (side, price)

    def add_limit(self, oid, side, price, qty):
        trades = []
        if oid == 0 or price <= 0 or qty <= 0:
            return trades
        if oid in self.loc:
            return trades
        left = [qty]
        if side == BID:
            trades = self._match(self.asks, BID, oid, left, False, price)
            if left[0] > 0:
                self.bids.setdefault(price, deque()).append([oid, left[0]])
                self.loc[oid] = (BID, price)
        else:
            trades = self._match(self.bids, ASK, oid, left, False, price)
            if left[0] > 0:
                self.asks.setdefault(price, deque()).append([oid, left[0]])
                self.loc[oid] = (ASK, price)
        return trades

    def add_market(self, oid, side, qty):
        if qty <= 0:
            return []
        if side == BID:
            return self._match(self.asks, BID, oid, [qty], True, 0)
        return self._match(self.bids, ASK, oid, [qty], True, 0)

    def _match(self, resting, taker_side, taker_id, qty_box, is_market, limit):
        # qty_box is a 1-list so the caller sees the leftover. ugly, i know.
        # (wanted to mirror the c++ in/out param)
        trades = []
        qty = qty_box[0]
        prices = sorted(resting.keys(), reverse=(resting is self.bids))
        for px in prices:
            if qty <= 0:
                break
            crosses = is_market or (px <= limit if taker_side == BID else px >= limit)
            if not crosses:
                break
            q = resting[px]
            while qty > 0 and q:
                mid, mq = q[0]
                fill = min(qty, mq)
                if taker_side == BID:
                    trades.append((taker_id, mid, px, fill))
                else:
                    trades.append((mid, taker_id, px, fill))
                qty -= fill
                mq -= fill
                if mq == 0:
                    q.popleft()
                    del self.loc[mid]
                else:
                    q[0][1] = mq
            if not q:
                del resting[px]
        qty_box[0] = qty
        return trades

    def cancel(self, oid):
        if oid not in self.loc:
            return False
        side, price = self.loc[oid]
        levels = self.bids if side == BID else self.asks
        q = levels.get(price, deque())
        for i, (mid, _) in enumerate(q):
            if mid == oid:
                del q[i]
                if not q:
                    del levels[price]
                del self.loc[oid]
                return True
        return False

    def snapshot(self):
        lines = []
        for px in sorted(self.bids.keys(), reverse=True):
            lines.append(f"B {px} {sum(q for _, q in self.bids[px])}")
        for px in sorted(self.asks.keys()):
            lines.append(f"A {px} {sum(q for _, q in self.asks[px])}")
        return lines


def gen_ops(rng, n):
    ops = []
    next_id = 1
    for _ in range(n):
        r = rng.random()
        if r < 0.6:
            side = rng.choice(["B", "A"])
            # prices cluster tightly so crossing actually happens a lot
            price = rng.randint(95, 105)
            qty = rng.randint(1, 20)
            ops.append(f"A {next_id} {side} {price} {qty}")
            next_id += 1
        elif r < 0.8:
            # cancel: sometimes a real id, sometimes garbage
            oid = rng.randint(1, next_id + 5)
            ops.append(f"C {oid}")
        else:
            side = rng.choice(["B", "A"])
            qty = rng.randint(1, 30)
            ops.append(f"M {next_id} {side} {qty}")
            next_id += 1
    # sprinkle in degenerate ops: dup ids, zero qty, bad price
    for _ in range(max(1, n // 50)):
        ops.insert(rng.randint(0, len(ops)), "A 1 B 100 5")   # dup id
        ops.insert(rng.randint(0, len(ops)), "A 999999 B 0 5")  # bad price
        ops.insert(rng.randint(0, len(ops)), "M 999998 A 0")    # zero qty
    return ops


def run_reference(ops):
    book = RefBook()
    out = []
    for line in ops:
        p = line.split()
        if p[0] == "A":
            _, oid, s, price, qty = p
            side = BID if s == "B" else ASK
            for (b, a, px, q) in book.add_limit(int(oid), side, int(price), int(qty)):
                out.append(f"T {b} {a} {px} {q}")
        elif p[0] == "M":
            _, oid, s, qty = p
            side = BID if s == "B" else ASK
            for (b, a, px, q) in book.add_market(int(oid), side, int(qty)):
                out.append(f"T {b} {a} {px} {q}")
        else:
            book.cancel(int(p[1]))
    out.append("BOOK")
    out.extend(book.snapshot())
    return out


def run_cpp(replay_bin, ops):
    with open("/tmp/fuzz_ops.txt", "w") as f:
        f.write("\n".join(ops) + "\n")
    r = subprocess.run([replay_bin, "/tmp/fuzz_ops.txt"],
                       capture_output=True, text=True, timeout=60)
    if r.returncode != 0:
        raise RuntimeError(f"replay crashed: {r.stderr[:500]}")
    return r.stdout.strip().split("\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("replay", help="path to the compiled replay binary")
    ap.add_argument("--seeds", type=int, default=200)
    ap.add_argument("--ops", type=int, default=500)
    args = ap.parse_args()

    for seed in range(args.seeds):
        rng = random.Random(seed)
        ops = gen_ops(rng, args.ops)
        expected = run_reference(ops)
        try:
            got = run_cpp(args.replay, ops)
        except RuntimeError as e:
            print(f"seed {seed}: {e}")
            return 1
        if got != expected:
            print(f"MISMATCH on seed {seed}")
            for i, (g, e) in enumerate(zip(got, expected)):
                if g != e:
                    print(f"  first diff at line {i}:")
                    print(f"    c++: {g}")
                    print(f"    ref: {e}")
                    break
            if len(got) != len(expected):
                print(f"  line counts differ: c++ {len(got)} vs ref {len(expected)}")
            with open("/tmp/fuzz_fail_ops.txt", "w") as f:
                f.write("\n".join(ops) + "\n")
            print("  failing ops saved to /tmp/fuzz_fail_ops.txt")
            return 1
        if seed % 50 == 0:
            print(f"  seed {seed} ok ({args.ops} ops)")
    print(f"all {args.seeds} seeds passed ({args.seeds * args.ops} ops total)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
