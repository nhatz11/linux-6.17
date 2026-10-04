#!/usr/bin/env python3
"""exit 0 if this (threshold, workload) cell has settled, else exit 1.

Uses the SAME estimator as the scorer: ratio-of-means with a paired bootstrap CI.
Mean-of-ratios is unsafe here -- vips's PV spin denominator varies 256x, which
made its per-rep deltas track the denominator rather than the effect.

STOPPING RULE: PRECISION ONLY. A cell is settled when both spin and perf have a
bootstrap CI half-width below HW -- regardless of what the result says.

Deliberately NOT "stop when the CI excludes zero": that is outcome-dependent
stopping (optional stopping). Adding reps until a result turns significant and
then halting inflates the false-positive rate and makes the reported CI invalid.
Stopping on precision is independent of the answer, so the interval means what it
says whichever way it comes out.
"""
import sys
import statistics as st
import random

random.seed(99)
LOWER = {"hackbench", "vips"}
BOOT = 1200

path, th, wl, hw = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
rows = [l.split("\t") for l in open(path).read().splitlines()[1:] if l.strip()]
wr = [x for x in rows if x[0] == th and x[1] == wl and x[4] not in ("NA", "")]
by = {}
for x in wr:
    by.setdefault(x[3], {})[x[2]] = x

ps, as_, pv, av = [], [], [], []
for rep, g in by.items():
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
    ps.append(t0); as_.append(t1); pv.append(o0); av.append(o1)

n = len(ps)
if n < 3:
    sys.exit(1)


def boot(x, y):
    out = []
    for _ in range(BOOT):
        idx = [random.randrange(n) for _ in range(n)]
        mx = st.mean([x[i] for i in idx])
        if mx <= 0:
            continue
        out.append(100 * (mx - st.mean([y[i] for i in idx])) / mx)
    if not out:
        return None
    out.sort()
    return out[int(0.025 * len(out))], out[int(0.975 * len(out))]


def settled(ci):
    if not ci:
        return False
    lo, hi = ci
    return (hi - lo) / 2 < hw          # precision only -- never "lo > 0"


sys.exit(0 if settled(boot(ps, as_)) and settled(boot(pv, av)) else 1)
