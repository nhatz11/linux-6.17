#!/usr/bin/env python3
"""Floor/ceiling report for floor_test.sh.

The mean is the wrong statistic for vips: AS raises the floor (publish false
sharing) and lowers the ceiling (halting on preemption), and those cancel. Report
the distribution instead -- that is where both effects are visible.

SUCCESS looks like: the AS@32767 floor (min/p10) near PV's, with the ceiling
(p90/max) still well below PV's.
"""
import sys
import statistics as st

NS = 26e-9
rows = [l.split("\t") for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
arms = ["pv", "as32768", "as32767"]
data = {}
for a in arms:
    v = sorted((int(x[3]) + int(x[4])) * NS * 1000
               for x in rows if x[0] == a and x[2] not in ("NA", ""))
    if v:
        data[a] = v


def q(v, p):
    if not v:
        return float("nan")
    i = p * (len(v) - 1)
    lo, hi = int(i), min(int(i) + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (i - lo)


print(f"\n  {'arm':<9s} {'n':>3s} {'MIN':>7s} {'p10':>7s} {'p25':>7s} {'med':>7s} "
      f"{'p75':>7s} {'p90':>7s} {'MAX':>8s} {'sd':>7s} {'spread':>7s}")
for a in arms:
    v = data.get(a)
    if not v:
        continue
    sd = st.stdev(v) if len(v) > 1 else 0.0
    print(f"  {a:<9s} {len(v):>3d} {min(v):7.1f} {q(v,.10):7.1f} {q(v,.25):7.1f} "
          f"{st.median(v):7.1f} {q(v,.75):7.1f} {q(v,.90):7.1f} {max(v):8.1f} {sd:7.1f} "
          f"{max(v)/max(min(v),0.01):6.1f}x")

if "pv" in data:
    pv = data["pv"]
    print(f"\n  vs stock PV  (+ = AS better):")
    for a in ("as32768", "as32767"):
        v = data.get(a)
        if not v:
            continue
        fl = 100 * (min(pv) - min(v)) / min(pv)
        p10 = 100 * (q(pv, .10) - q(v, .10)) / q(pv, .10)
        ce = 100 * (max(pv) - max(v)) / max(pv)
        p90 = 100 * (q(pv, .90) - q(v, .90)) / q(pv, .90)
        md = 100 * (st.median(pv) - st.median(v)) / st.median(pv)
        print(f"    {a:<9s} floor(min) {fl:+7.1f}%  p10 {p10:+7.1f}%  median {md:+7.1f}%"
              f"   p90 {p90:+7.1f}%  ceiling(max) {ce:+7.1f}%")
    a, b = data.get("as32768"), data.get("as32767")
    if a and b:
        print(f"\n  32768 -> 32767 moved the AS floor {min(a):.1f} -> {min(b):.1f} ms "
              f"({100*(min(a)-min(b))/min(a):+.1f}%), PV floor is {min(pv):.1f} ms")
        gap_before = min(a) - min(pv)
        gap_after = min(b) - min(pv)
        if gap_before > 0:
            print(f"  floor gap to PV: {gap_before:.1f} ms -> {gap_after:.1f} ms "
                  f"({100*(gap_before-gap_after)/gap_before:+.0f}% of the gap closed)")
