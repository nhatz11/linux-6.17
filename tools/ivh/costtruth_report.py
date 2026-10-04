#!/usr/bin/env python3
"""Cost split + detector validation for as_cost_and_truth.sh.

Scored on NODE iterations only: ivh_head_spin_iters_sum counts only budget
exhaustions, so it is quantised to multiples of ivh_pv_spin_threshold and it
manufactured a fake "AS has a hard floor" result once already.

vips PV spin is BIMODAL (~5-15 ms vs ~43-76 ms, roughly 3 in 10), so a median or
mean at this n is dominated by how many disasters land in the sample -- the same
comparison reversed at n=4, 6, 8 and 10 in one sitting. Hence: report the
disaster RATE and the disaster MAGNITUDE separately, and the variance ratio, and
do not pretend a mean difference is a verdict.
"""
import sys
import os
import statistics as st

NS = 26e-9
WARMUP = int(os.environ.get("WARMUP", 2))
ARMS = ["pv", "mig", "as", "migas"]

rows = [l.split("\t") for l in open(sys.argv[1]).read().splitlines() if l.strip()]
hdr, rows = rows[0], rows[1:]
ix = {k: i for i, k in enumerate(hdr)}
g = lambda r, k: float(r[ix[k]])

recs = [r for r in rows if r[ix["sec"]] not in ("NA", "") and int(r[ix["rep"]]) > WARMUP]
arms = [a for a in ARMS if any(r[0] == a for r in recs)]
if not recs:
    print("  all reps still warm-up")
    sys.exit()


def q(v, p):
    v = sorted(v); i = p * (len(v) - 1)
    lo, hi = int(i), min(int(i) + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (i - lo)


print(f"\n  === NODE spin, ms (reps >{WARMUP}) ===")
print(f"  {'arm':<6s} {'n':>2s} {'min':>6s} {'p25':>6s} {'med':>6s} {'p75':>6s} {'max':>7s} "
      f"{'mean':>6s} {'CV':>6s} {'it/ent':>7s} {'sec':>6s} {'hwait%':>7s}")
S = {}
for a in arms:
    gg = [r for r in recs if r[0] == a]
    v = [g(r, "node") * NS * 1000 for r in gg]
    S[a] = dict(v=v, gg=gg)
    ipe = sum(g(r, "node") for r in gg) / max(sum(g(r, "ent") for r in gg), 1)
    # raw hwait scales with run length (measured 7.8-7.9 ms/s in every arm), so
    # it cannot separate a disaster from a long run. The duration-independent
    # ground truth is the fraction of vCPU time spent on the runqueue:
    #   hwait_ns / (16 vCPUs * sec).  ~49% at the 2:1 contention used here.
    hw = 100 * st.mean(g(r, "hwait") / 1e9 / (16 * g(r, "sec")) for r in gg)
    cv = 100 * st.stdev(v) / st.mean(v) if len(v) > 1 else 0
    print(f"  {a:<6s} {len(v):>2d} {min(v):6.2f} {q(v,.25):6.2f} {st.median(v):6.2f} {q(v,.75):6.2f} "
          f"{max(v):7.2f} {st.mean(v):6.2f} {cv:5.1f}% {ipe:7.1f} "
          f"{st.mean(g(r,'sec') for r in gg):6.2f} {hw:6.1f}%")

print(f"\n  === Q1: COST SPLIT (mean node ms; + = that component COSTS spin) ===")
if {"pv", "mig", "as", "migas"} <= set(S):
    m = {a: st.mean(S[a]["v"]) for a in S}
    print(f"    stock PV                       {m['pv']:7.2f} ms")
    print(f"    migration alone (migas - as)   {m['migas']-m['as']:+7.2f} ms")
    print(f"    AS alone        (migas - mig)  {m['migas']-m['mig']:+7.2f} ms")
    print(f"    mig+pedestal    (mig   - pv)   {m['mig']-m['pv']:+7.2f} ms")
    print(f"    AS +pedestal    (as    - pv)   {m['as']-m['pv']:+7.2f} ms")
    print(f"    full stack      (migas - pv)   {m['migas']-m['pv']:+7.2f} ms")
    print(f"    (bimodal: treat any single term smaller than the disaster spread as unresolved)")

print(f"\n  === Q2: IS THE DETECTOR RIGHT? host wait_ns vs in-guest fires, per rep ===")
for a in arms:
    if a in ("pv", "mig"):
        continue
    gg = S[a]["gg"]
    v = S[a]["v"]
    med = st.median(v)
    print(f"\n  -- {a} --")
    print(f"     {'rep':>4s} {'node ms':>8s} {'it/ent':>7s} {'t2chk':>6s} {'t2f':>6s} {'t2%':>6s} "
          f"{'hwait%':>7s} {'hsw/s':>8s} {'kind':>9s}")
    for r in gg:
        ms = g(r, "node") * NS * 1000
        ipe = g(r, "node") / max(g(r, "ent"), 1)
        t2c, t2f = g(r, "t2chk"), g(r, "t2f")
        pct = f"{100*t2f/t2c:.2f}%" if t2c else "-"
        kind = "DISASTER" if ms > 2 * med else ""
        print(f"     {int(g(r,'rep')):>4d} {ms:8.2f} {ipe:7.1f} {t2c:6.0f} {t2f:6.0f} {pct:>6s} "
              f"{100*g(r,'hwait')/1e9/(16*max(g(r,'sec'),1e-9)):6.1f}% "
              f"{g(r,'hsw')/max(g(r,'sec'),1):8.0f} {kind:>9s}")
    dis = [r for r in gg if g(r, "node") * NS * 1000 > 2 * med]
    ok = [r for r in gg if g(r, "node") * NS * 1000 <= 2 * med]
    if dis and ok:
        frac = lambda r: 100 * g(r, "hwait") / 1e9 / (16 * max(g(r, "sec"), 1e-9))
        hwd = st.mean(frac(r) for r in dis)
        hwo = st.mean(frac(r) for r in ok)
        fd = st.mean(100 * g(r, "t2f") / max(g(r, "t2chk"), 1) for r in dis)
        fo = st.mean(100 * g(r, "t2f") / max(g(r, "t2chk"), 1) for r in ok)
        print(f"     DISASTERS n={len(dis)}: host wait {hwd:5.1f}% of vCPU time, fire rate {fd:5.2f}%")
        print(f"     NORMAL    n={len(ok)}: host wait {hwo:5.1f}% of vCPU time, fire rate {fo:5.2f}%")
        ratio = hwd / hwo if hwo else float("inf")
        print(f"     host-wait ratio disaster/normal = {ratio:.2f}x "
              f"({hwd:.1f}% vs {hwo:.1f}%)")
        if ratio > 1.15 and fd < fo * 1.5:
            print("     => REAL PREEMPTION the detector MISSED: host wait is up, firing is not.")
        elif ratio <= 1.15:
            print("     => host wait is FLAT on disasters: vips's tail is NOT vCPU preemption,")
            print("        so a preemption detector cannot fix it. Detector is not broken.")
        else:
            print("     => both up: detector sees it; the cost is that halting does not recover it.")
