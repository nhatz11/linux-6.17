#!/usr/bin/env python3
"""Report for overnight.sh.

SPIN REDUCTION follows the required convention:
  THROUGHPUT: saved = spin_PV * (ops_AS / ops_PV) - spin_AS
      Both arms run a fixed wall time, so the faster arm mechanically accrues
      more total spin. Normalising to the work actually done is mandatory.
  TIME:       saved = spin_PV - spin_AS
      Fixed work, so totals are directly comparable and must NOT be scaled.
"""
import sys
import statistics as st

NS = 26e-9
path = sys.argv[1]
title = sys.argv[2] if len(sys.argv) > 2 else "RESULTS"
rows = [l.split('\t') for l in open(path).read().splitlines()[1:] if l.strip()]


def num(x):
    try:
        return float(x)
    except Exception:
        return None


print("\n" + "=" * 112)
print(title.center(112))
print("spin = node_spin_iters x 26ns (NODE only; head ~20-27% uninstrumented)".center(112))
print("THROUGHPUT saved = spin_PV*(ops_AS/ops_PV) - spin_AS   |   TIME saved = spin_PV - spin_AS".center(112))
print("=" * 112)

arms = [a for a in dict.fromkeys(x[2] for x in rows) if a != 'pv']
workloads = list(dict.fromkeys(x[0] for x in rows))
summary = {a: [] for a in arms}

hdr = f"  {'workload':<22s} {'type':>4s} {'arm':>5s} {'spin_PV':>9s} {'spin_AS':>9s} {'saved':>9s} {'ratio':>6s} {'perf Δ':>8s} {'t2 fires':>10s}"
print(hdr)
print("  " + "-" * 108)
for w in workloads:
    wr = [x for x in rows if x[0] == w]
    pv = [x for x in wr if x[2] == 'pv' and num(x[4]) is not None]
    if not pv:
        print(f"  {w:<22s}  no usable PV arm")
        continue
    ty = pv[0][1]
    sp_pv = st.mean(int(x[5]) for x in pv) * NS
    ops_pv = st.mean(num(x[4]) for x in pv)
    if sp_pv <= 0 or not ops_pv:
        print(f"  {w:<22s} {ty[:4]:>4s}  PV spin {sp_pv:.3f}s / value {ops_pv} -- unusable")
        continue
    first = True
    for a in arms:
        g = [x for x in wr if x[2] == a and num(x[4]) is not None]
        if not g:
            continue
        sp_as = st.mean(int(x[5]) for x in g) * NS
        ops_as = st.mean(num(x[4]) for x in g)
        if ty == 'THROUGHPUT':
            saved = sp_pv * (ops_as / ops_pv) - sp_as
            perf = 100 * (ops_as - ops_pv) / ops_pv
        else:
            saved = sp_pv - sp_as
            perf = 100 * (ops_pv - ops_as) / ops_pv
        pct = 100 * saved / sp_pv
        t2 = st.mean(int(x[7]) for x in g)
        summary[a].append((w, pct, perf, t2))
        lbl = w if first else ""
        tylbl = ty[:4] if first else ""
        print(f"  {lbl:<22s} {tylbl:>4s} {a:>5s} {sp_pv:8.2f}s {sp_as:8.2f}s "
              f"{saved:+8.2f}s {sp_as/sp_pv:6.3f} {perf:+7.2f}% {t2:10,.0f}")
        first = False

print("\n" + "=" * 112)
print("SUMMARY -- spin reduction % (positive = AS removed wait time)")
for a in arms:
    d = summary[a]
    if not d:
        continue
    pcts = [p for _, p, _, _ in d]
    perfs = [q for _, _, q, _ in d]
    win = sum(1 for p in pcts if p > 0)
    print(f"\n  arm {a}us   n={len(d)} workloads   spin reduced in {win}/{len(d)}")
    print(f"       spin median {st.median(pcts):+6.2f}%   mean {st.mean(pcts):+6.2f}%")
    print(f"       perf median {st.median(perfs):+6.2f}%   mean {st.mean(perfs):+6.2f}%   "
          f"({sum(1 for q in perfs if q > 0)}/{len(perfs)} better)")
    for w, p, q, t2 in sorted(d, key=lambda z: -z[1]):
        dead = "  (tier2 ~0: inert)" if t2 < 50 else ""
        print(f"         {w:<24s} spin {p:+7.2f}%   perf {q:+7.2f}%{dead}")
print("=" * 112)
