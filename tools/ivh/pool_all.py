#!/usr/bin/env python3
"""Pool every AS-alone-@50us-vs-stock-PV measurement taken today.

WHY POOL: individual sittings ran n=4..12 and host contention drifted between
them (cap_mean 565..880), so single-sitting numbers carry wide intervals and
differ in magnitude. Pooling the PAIRED PER-REP DELTAS is valid because each
delta is computed within its own rep against its own PV run, so between-sitting
drift cancels inside each delta.

WHAT IS NOT POOLED:
  migas_*, p11sweep_*  -- those ran migration ON (mig+AS). Different arm.
  cleanspin_1003-230643 -- aborted run, had a leftover memcached inflating load
                           (cap_mean 565 vs 746); 1 row.

This reports the POOLED estimate, not the best sitting. Per-sitting values are
printed underneath so any cherry-picking would be visible.
"""
import glob, statistics as st, math, sys

NS = 26e-9
# file -> (workload_col, arm_col, rep_col, value_col, node_col, head_col|None, pv_name, as_names, wl_map)
SPECS = [
    ("as6hunt_*.tsv",      0, 2, 3, 4, 5, None, "pv", {"50"},  None),
    ("cleanspin_1003-231416.tsv", 0, 1, 2, 3, 4, 5, "pv", {"as"}, None),
    ("bothmetrics_*.tsv",  0, 1, 2, 3, 7, None, "pv", {"as"},  None),
    ("restmetrics_*.tsv",  0, 1, 2, 3, 7, None, "pv", {"as"},  None),
    ("powerup_*.tsv",      0, 1, 2, 3, 4, 5, "pv", {"as"},     None),
]
# ebizzy_settle has no workload column (ebizzy only): arm rep ops iters entries t2f cap
SETTLE = ("ebizzy_settle_*.tsv", 0, 1, 2, 3)

LOWER_IS_BETTER = {"hackbench", "hackbench_pipe_thr", "vips", "parsec_vips"}
CANON = {"hackbench_pipe_thr": "hackbench", "memtier_memcached": "memtier",
         "dbench_16_noF": "dbench", "ebizzy_mmap": "ebizzy", "parsec_vips": "vips"}


def canon(w):
    return CANON.get(w, w)


def rows_of(path):
    L = open(path).read().splitlines()
    return [l.split("\t") for l in L[1:] if l.strip()]


def collect():
    """returns {workload: {'perf':[(delta,src)], 'node':[...], 'nodehead':[...]}}"""
    out = {}

    def add(w, kind, val, src):
        out.setdefault(w, {}).setdefault(kind, []).append((val, src))

    for pat, wc, ac, rc, vc, nc, hc, pvn, asn, _ in SPECS:
        for path in sorted(glob.glob("/root/ivh_logs/" + pat)):
            if "230643" in path:          # aborted run, memcached leftover
                continue
            src = path.split("/")[-1].split("_")[0]
            by = {}
            for x in rows_of(path):
                if len(x) <= max(wc, ac, rc, vc, nc, hc or 0):
                    continue
                if x[vc] in ("NA", ""):
                    continue
                by.setdefault((canon(x[wc]), x[rc]), {})[x[ac]] = x
            for (w, rep), g in by.items():
                a_key = next((k for k in g if k in asn), None)
                if pvn not in g or a_key is None:
                    continue
                p, a = g[pvn], g[a_key]
                try:
                    o0, o1 = float(p[vc]), float(a[vc])
                except ValueError:
                    continue
                if not o0:
                    continue
                lower = w in LOWER_IS_BETTER
                add(w, "perf", 100 * (o0 - o1) / o0 if lower else 100 * (o1 - o0) / o0, src)
                R = 1.0 if lower else o1 / o0
                n0, n1 = int(p[nc]) * R, int(a[nc])
                if n0 > 0:
                    add(w, "node", 100 * (n0 - n1) / n0, src)
                if hc is not None:
                    t0 = (int(p[nc]) + int(p[hc])) * R
                    t1 = int(a[nc]) + int(a[hc])
                    if t0 > 0:
                        add(w, "nodehead", 100 * (t0 - t1) / t0, src)

    for path in sorted(glob.glob("/root/ivh_logs/" + SETTLE[0])):
        src = "settle"
        by = {}
        for x in rows_of(path):
            if len(x) < 5 or x[2] in ("NA", ""):
                continue
            by.setdefault(x[1], {})[x[0]] = x
        for rep, g in by.items():
            if "pv" not in g or "as" not in g:
                continue
            o0, o1 = float(g["pv"][2]), float(g["as"][2])
            i0, i1 = int(g["pv"][3]), int(g["as"][3])
            if not o0:
                continue
            R = o1 / o0
            add("ebizzy", "perf", 100 * (o1 - o0) / o0, src)
            if i0 * R > 0:
                add("ebizzy", "node", 100 * (i0 * R - i1) / (i0 * R), src)
    return out


def ci(v):
    n = len(v)
    if n < 3:
        return None
    m = st.mean(v); se = st.stdev(v) / math.sqrt(n)
    t = 1.96 + 2.4 / n
    return m, m - t * se, m + t * se, n


def main():
    data = collect()
    order = ["hackbench", "memtier", "dbench", "ebizzy", "vips"]
    print("=" * 100)
    print("POOLED: every AS-alone @50us mask255 vs stock PV rep taken today".center(100))
    print("paired per-rep deltas; between-sitting drift cancels inside each delta".center(100))
    print("=" * 100)
    print(f"  {'workload':<10s} {'metric':<9s} {'n':>3s} {'mean':>8s} {'95% CI':>18s}  {'pos':>7s}  verdict")
    print("  " + "-" * 92)
    verdict = {}
    for w in order:
        if w not in data:
            continue
        for kind in ("node", "nodehead", "perf"):
            vals = [v for v, _ in data[w].get(kind, [])]
            c = ci(vals)
            if not c:
                continue
            m, lo, hi, n = c
            tag = "POSITIVE" if lo > 0 else ("NEGATIVE" if hi < 0 else "neutral")
            print(f"  {w:<10s} {kind:<9s} {n:>3d} {m:+7.2f}% [{lo:+7.2f},{hi:+7.2f}] "
                  f"{sum(1 for z in vals if z > 0):>3d}/{n:<3d} {tag}")
            verdict.setdefault(w, {})[kind] = (m, lo, hi, n)
        print()
    print("=" * 100)
    print("GOAL TEST  (spin not negative  AND  perf >= -1%)")
    for w in order:
        v = verdict.get(w)
        if not v:
            continue
        spin = v.get("nodehead") or v.get("node")
        perf = v.get("perf")
        if not spin or not perf:
            continue
        ok = spin[2] > 0 and perf[1] >= -1.0
        print(f"  {w:<10s} spin {spin[0]:+6.2f}% (CI hi {spin[2]:+6.2f})   "
              f"perf {perf[0]:+6.2f}% (CI lo {perf[1]:+6.2f})   -> {'PASS' if ok else 'FAIL'}")
    print("=" * 100)
    print("\nPER-SITTING breakdown (so pooling cannot hide a cherry-pick):")
    for w in order:
        if w not in data:
            continue
        print(f"  {w}:")
        for kind in ("node", "perf"):
            bysrc = {}
            for v, s in data[w].get(kind, []):
                bysrc.setdefault(s, []).append(v)
            cells = "  ".join(f"{s}:{st.median(x):+6.2f}%(n={len(x)})" for s, x in sorted(bysrc.items()))
            print(f"    {kind:<9s} {cells}")


main()
