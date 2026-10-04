#!/usr/bin/env python3
"""Scorer for p11_full.sh -- RATIO OF MEANS with a bootstrap CI.

WHY NOT MEAN-OF-RATIOS: the paired delta (PV-AS)/PV has PV in the denominator.
vips's PV spin varies 256x across runs, so low-PV draws blow the ratio up
negative and dominate the average -- measured corr(PV draw, per-rep delta) =
+0.73, i.e. the "effect" was tracking the denominator. Mean-of-ratios reported
vips at -81.54%; ratio-of-means on the same data gives +18.59%. The two agree on
every workload whose PV spread is small (hackbench 2.0x, memtier 1.5x, dbench
1.3x, ebizzy 1.6x) and diverge only on vips. Jensen: E[(X-Y)/X] != (E[X]-E[Y])/E[X].

Estimator: (mean(PV) - mean(AS)) / mean(PV), interval by paired bootstrap.
"""
import sys
import statistics as st
import random

random.seed(1234)
LOWER = {"hackbench", "vips"}          # value is seconds: lower is better
BOOT = 2000

path = sys.argv[1]
fth = sys.argv[2] if len(sys.argv) > 2 else None
fwl = sys.argv[3] if len(sys.argv) > 3 else None
rows = [l.split("\t") for l in open(path).read().splitlines()[1:] if l.strip()]


def pairs(th, wl):
    wr = [x for x in rows if x[0] == th and x[1] == wl and x[4] not in ("NA", "")]
    by = {}
    for x in wr:
        by.setdefault(x[3], {})[x[2]] = x
    ps, as_, pv, av, fi = [], [], [], [], []
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
        ps.append(t0); as_.append(t1); pv.append(o0); av.append(o1); fi.append(int(a[8]))
    return ps, as_, pv, av, fi


def rom(x, y):
    mx = st.mean(x)
    return 100 * (mx - st.mean(y)) / mx if mx else 0.0


def boot_ci(x, y):
    n = len(x)
    if n < 3:
        return None
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


def score(th, wl):
    ps, as_, pv, av, fi = pairs(th, wl)
    n = len(ps)
    if n < 3:
        return None
    s = rom(ps, as_)
    sci = boot_ci(ps, as_)
    if wl in LOWER:
        p = rom(pv, av)
        pci = boot_ci(pv, av)
    else:
        p = -rom(pv, av)
        c = boot_ci(pv, av)
        pci = (-c[1], -c[0]) if c else None
    return n, s, sci, p, pci, (st.mean(fi) if fi else 0)


def tag(ci):
    if not ci:
        return "?  "
    return "POS" if ci[0] > 0 else ("NEG" if ci[1] < 0 else "neu")


if fth and fwl:
    r = score(fth, fwl)
    if not r:
        print(f"  {fwl} @{fth}us: too few pairs")
        sys.exit()
    n, s, sci, p, pci, fi = r
    ok = sci and sci[0] > 0 and pci and pci[1] >= 0
    print(f"  {fwl:<10s} @{fth:>5s}us n={n:2d} fires/run {fi:9.0f}  "
          f"SPIN {s:+7.2f}% [{sci[0]:+7.2f},{sci[1]:+7.2f}] {tag(sci)}  "
          f"PERF {p:+6.2f}% [{pci[0]:+6.2f},{pci[1]:+6.2f}] {tag(pci)}  {'PASS' if ok else 'fail'}")
    sys.exit()

ths = sorted({x[0] for x in rows}, key=int)
wls = ["hackbench", "memtier", "dbench", "ebizzy", "vips"]
print("\n" + "=" * 104)
print("POINT 11 -- ratio-of-means, bootstrap 95% CI".center(104))
print("PASS = spin CI entirely above 0  AND  perf CI not entirely below 0".center(104))
print("=" * 104)
for th in ths:
    print(f"\n  --- threshold {th}us ---")
    npass = tot = 0
    for w in wls:
        r = score(th, w)
        if not r:
            continue
        n, s, sci, p, pci, fi = r
        tot += 1
        ok = sci and sci[0] > 0 and pci and pci[1] >= 0
        npass += 1 if ok else 0
        print(f"    {w:<10s} n={n:2d}  SPIN {s:+7.2f}% [{sci[0]:+7.2f},{sci[1]:+7.2f}] {tag(sci)}"
              f"   PERF {p:+6.2f}% [{pci[0]:+6.2f},{pci[1]:+6.2f}] {tag(pci)}  {'PASS' if ok else 'fail'}")
    if tot:
        print(f"    => {npass}/{tot} pass" + ("   *** ALL PASS ***" if npass == tot and tot >= 5 else ""))
