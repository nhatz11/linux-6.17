#!/usr/bin/env python3
"""Paired-per-rep report for hb_perf_clean.sh. Median + sign test, plus an
outlier flag so a contaminated window is visible rather than averaged in."""
import sys
import statistics as st

NS = 26e-9
rows = [l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
by = {}
for x in rows:
    by.setdefault(x[1], {})[x[0]] = x

print("\n=== hackbench, paired vs stock PV, migration OFF both arms ===")
print("    + = AS faster / AS spun less\n")
for a in ['50', '100']:
    pf, sp, raw = [], [], []
    for rep, g in sorted(by.items(), key=lambda z: int(z[0])):
        if 'pv' not in g or a not in g:
            continue
        tp, ta = float(g['pv'][2]), float(g[a][2])
        ip, ia = int(g['pv'][3]), int(g[a][3])
        pf.append(100 * (tp - ta) / tp)
        sp.append(100 * (ip - ia) / ip)
        raw.append((rep, tp, ta))
    if not pf:
        continue
    print(f"  {a}us  n={len(pf)}")
    print(f"       PERF median {st.median(pf):+7.2f}%   mean {st.mean(pf):+7.2f}%   "
          f"({sum(1 for z in pf if z > 0)}/{len(pf)} faster)")
    print(f"       SPIN median {st.median(sp):+7.2f}%   mean {st.mean(sp):+7.2f}%   "
          f"({sum(1 for z in sp if z > 0)}/{len(sp)} less spin)")
    print(f"       perf per rep {[round(z, 1) for z in pf]}")
    print(f"       spin per rep {[round(z, 1) for z in sp]}")
    # flag reps where either arm is a gross outlier vs that arm's own median
    med_a = st.median([t for _, _, t in raw])
    od = [r for r, _, t in raw if t > 2 * med_a]
    if od:
        print(f"       *** reps {od} have an AS time > 2x the arm median -- "
              f"likely host window, inspect before pooling")
    print()

# PV arm stability: if the baseline itself is bimodal, say so
pv = [float(g['pv'][2]) for g in by.values() if 'pv' in g]
if len(pv) > 2:
    print(f"  PV baseline: mean {st.mean(pv):.2f}s  CV {100*st.stdev(pv)/st.mean(pv):.1f}%  "
          f"range {min(pv):.2f}-{max(pv):.2f}s")
