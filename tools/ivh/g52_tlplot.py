#!/usr/bin/env python3
"""
Render the time-left curve: Gate 2 firing rate AND throughput vs threshold,
for last_active (source=1) and the EWMA (source=2).

The point of showing both axes together: #7 showed only throughput, was
flat, and was therefore read as "the knob is dead". G-LOCK-50 later showed
the knob moves the firing rate 7.4x over the same range. A flat throughput
curve beside a steep firing curve is a RESULT -- rejection does not drive
throughput -- not a failed experiment. Only the pair can say which it is.
"""
import sys, os, statistics, collections

out = sys.argv[1]
rows = []
with open(os.path.join(out, "raw.tsv")) as f:
    next(f)
    for ln in f:
        p = ln.rstrip("\n").split("\t")
        if len(p) < 8 or not p[4]:
            continue
        rows.append((p[0], int(p[1]), p[2], float(p[4]), int(p[5]), int(p[6]), int(p[7])))

HI = {"dbench", "ebizzy"}          # hackbench is a TIME: lower is better
SRC = {"1": "last_active", "2": "EWMA"}
by = collections.defaultdict(list)
for src, th, b, v, ev, fi, mg in rows:
    by[(src, th, b)].append((v, ev, fi, mg))

benches = sorted({r[2] for r in rows})
for src in sorted({r[0] for r in rows}):
    ths = sorted({r[1] for r in rows if r[0] == src})
    print(f"\n=== source={src} ({SRC.get(src,'?')}) ===")
    hdr = f"{'thresh':>9} {'fire%':>8} {'migs':>9} " + " ".join(f"{b:>11}" for b in benches)
    print(hdr)
    base = {}
    for th in ths:
        cells = [f"{th/1e6:>7.2f}ms"]
        ev = fi = mg = 0
        for b in benches:
            for _, e, f_, m in by.get((src, th, b), []):
                ev += e; fi += f_; mg += m
        cells.append(f"{100.0*fi/ev:>7.2f}%" if ev else f"{'-':>8}")
        cells.append(f"{mg:>9}")
        for b in benches:
            d = by.get((src, th, b))
            cells.append(f"{statistics.median(x[0] for x in d):>11.2f}" if d else f"{'-':>11}")
        print(" ".join(cells))
        for b in benches:
            d = by.get((src, th, b))
            if d:
                base.setdefault(b, statistics.median(x[0] for x in d))

    # spread across the swept range: this is the number that says whether
    # the knob is worth a figure at all
    print(f"\n  {'':<12}{'fire% span':>14}{'throughput span':>20}")
    evs = []
    for th in ths:
        ev = fi = 0
        for b in benches:
            for _, e, f_, _m in by.get((src, th, b), []):
                ev += e; fi += f_
        if ev:
            evs.append(100.0*fi/ev)
    if evs:
        print(f"  {'gate':<12}{min(evs):>6.1f} -> {max(evs):<6.1f}"
              f"{'  (x%.1f)' % (max(evs)/min(evs)) if min(evs) else '':>20}")
    for b in benches:
        vals = [statistics.median(x[0] for x in by[(src, th, b)])
                for th in ths if (src, th, b) in by]
        if len(vals) < 2:
            continue
        lo, hi = min(vals), max(vals)
        span = (hi-lo)/lo*100
        flag = "  <-- FLAT" if span < 3 else ("  <-- RESPONDS" if span > 8 else "")
        print(f"  {b:<12}{'':>14}{lo:>8.2f} -> {hi:<8.2f} ({span:+.1f}%){flag}")
