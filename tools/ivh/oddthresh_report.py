#!/usr/bin/env python3
"""Report for oddthresh.sh: AS vs stock PV, split by spin budget.

The question: does making the budget odd (so the redundant first-iteration
publish never fires on short spinners) remove the spin damage on vips/ebizzy?
"""
import sys
import statistics as st
import math

NS = 26e-9
LOWER = {"vips"}          # value is seconds; lower is better, fixed work

path = sys.argv[1]
fwl = sys.argv[2] if len(sys.argv) > 2 else None
fb = sys.argv[3] if len(sys.argv) > 3 else None
rows = [l.split("\t") for l in open(path).read().splitlines()[1:] if l.strip()]


def cell(wl, b):
    wr = [x for x in rows if x[0] == wl and x[1] == b and x[4] not in ("NA", "")]
    by = {}
    for x in wr:
        by.setdefault(x[3], {})[x[2]] = x
    sp, pf, t2 = [], [], []
    for rep, g in sorted(by.items(), key=lambda z: int(z[0])):
        if "pv" not in g or "as" not in g:
            continue
        p, a = g["pv"], g["as"]
        try:
            o0, o1 = float(p[4]), float(a[4])
        except ValueError:
            continue
        if not o0:
            continue
        R = 1.0 if wl in LOWER else o1 / o0
        t0 = (int(p[5]) + int(p[6])) * R
        t1 = int(a[5]) + int(a[6])
        if t0 <= 0:
            continue
        sp.append(100 * (t0 - t1) / t0)
        pf.append(100 * (o0 - o1) / o0 if wl in LOWER else 100 * (o1 - o0) / o0)
        t2.append(int(a[8]))
    return sp, pf, t2


def ci(v):
    n = len(v)
    if n < 3:
        return None
    m = st.mean(v)
    se = st.stdev(v) / math.sqrt(n)
    t = 1.96 + 2.4 / n
    return m, m - t * se, m + t * se, n


def tag(c):
    if not c:
        return "?"
    m, lo, hi, _ = c
    return "POSITIVE" if lo > 0 else ("NEGATIVE" if hi < 0 else "neutral ")


def show(wl, b):
    sp, pf, t2 = cell(wl, b)
    cs, cp = ci(sp), ci(pf)
    if not cs:
        print(f"  {wl:<7s} budget {b}: n={len(sp)} (too few)")
        return
    print(f"  {wl:<7s} budget {b}  n={cs[3]:2d}  t2/run {st.mean(t2):7.0f}   "
          f"SPIN {cs[0]:+8.2f}% [{cs[1]:+8.2f},{cs[2]:+8.2f}] {tag(cs)}   "
          f"PERF {cp[0]:+7.2f}% [{cp[1]:+7.2f},{cp[2]:+7.2f}] {tag(cp)}")


if fwl and fb:
    show(fwl, fb)
else:
    print("\n" + "=" * 100)
    print("Does an ODD spin budget remove the redundant publish, and the damage?".center(100))
    print("32768 = power of two -> iteration 1 always publishes (redundant: pv_init_node just stamped)".center(100))
    print("32767 -> first publish moves 255 iters in; vips(138) and ebizzy(43) never publish".center(100))
    print("=" * 100)
    for wl in ("vips", "ebizzy"):
        for b in ("32768", "32767"):
            show(wl, b)
        a = ci(cell(wl, "32768")[0])
        c = ci(cell(wl, "32767")[0])
        if a and c:
            print(f"  {'':7s} -> spin moved {a[0]:+.2f}% to {c[0]:+.2f}%  "
                  f"({c[0] - a[0]:+.2f} points) when the first publish was removed\n")
