#!/usr/bin/env python3
"""Cost split + detector validation, scored on the kernel's own wait measurement.

PRIMARY METRIC: ivh_slowpath_wait_ns / ivh_slowpath_wait_events = mean slowpath
residence per acquisition, in real ns. Complete scope (node loop + head loop +
halt), no quantisation, no iterations-to-time constant.

Why not iterations: node-only reads 1.2-3 us/entry on vips while the kernel's own
measurement reads 10.66 us -- the iteration counters see only the node loop. And
node+head x 26ns converts to 104.60% of total slowpath residence, which is
impossible given 26.2% of residence is halt; the implied real cost is 18.35
ns/iter, so iteration-derived absolute times are inflated ~42%.

vips PV spin is BIMODAL (~3-in-10 disaster rate), so this reports the disaster
RATE and MAGNITUDE separately and does not treat a mean difference as a verdict.
"""
import sys
import os
import statistics as st

WARMUP = int(os.environ.get("WARMUP", 2))
# Arm names are DISCOVERED from the file, not hardcoded: a hardcoded list
# silently dropped the migas0/migas10 arms of the 2026-10-04 halt_min test and
# printed a two-arm report as if the others had not run. "pv" is forced first so
# the vs-PV deltas have a baseline; the rest keep file order.
ARMS = None

rows = [l.split("\t") for l in open(sys.argv[1]).read().splitlines() if l.strip()]
_seen_order = []
for _l in rows[1:]:
    _a = _l[0]
    if _a not in _seen_order:
        _seen_order.append(_a)
ARMS = (["pv"] if "pv" in _seen_order else []) + [a for a in _seen_order if a != "pv"]
hdr, rows = rows[0], rows[1:]
ix = {k: i for i, k in enumerate(hdr)}
g = lambda r, k: float(r[ix[k]])

recs = [r for r in rows if r[ix["sec"]] not in ("NA", "") and int(r[ix["rep"]]) > WARMUP
        and g(r, "ent") > 0]
# BALANCE THE ARMS. Arm means computed over whatever reps each arm happens to have
# are not comparable: on 2026-10-04 `pv` and `as` had a catastrophic rep 9
# (200.5 s wait vs a typical 17 s) that `migas` had not yet run, and that one
# unpaired outlier alone produced a spurious "+10.00% total wait" for AS. Keep
# only reps where EVERY arm present in the file has a datum.
_arms_present = {r[0] for r in recs}
_cnt = {}
for _r in recs:
    _cnt.setdefault(int(_r[ix["rep"]]), set()).add(_r[0])
_complete = {k for k, v in _cnt.items() if v >= _arms_present}
_dropped = sorted(set(_cnt) - _complete)
recs = [r for r in recs if int(r[ix["rep"]]) in _complete]
if _dropped:
    print(f"  [balanced: dropped incomplete reps {_dropped} "
          f"-- arms needed {sorted(_arms_present)}]")
arms = [a for a in ARMS if any(r[0] == a for r in recs)]
if not recs:
    print("  all reps still warm-up"); sys.exit()

wpa = lambda r: g(r, "waitns") / g(r, "ent")          # total residence ns/acq
# THE GOAL METRIC: on-CPU spin = residence minus halted time, both in ns from the
# same two counters. Boot-cumulative this is 73.8% of residence and never
# negative, so unlike the old "wall_ns minus halt_cyc" formula it does not mix
# clocks. AS is expected to TRADE spin for halt, so spin/acq can fall while
# wait/acq stays flat -- which is exactly the distinction the goal turns on.
spa = lambda r: max(g(r, "waitns") - g(r, "haltns"), 0) / g(r, "ent")
hfrac = lambda r: 100 * g(r, "hwait") / 1e9 / (16 * max(g(r, "sec"), 1e-9))


def q(v, p):
    v = sorted(v); i = p * (len(v) - 1)
    lo, hi = int(i), min(int(i) + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (i - lo)


print(f"\n  === ON-CPU SPIN PER ACQUISITION, ns  <-- THE GOAL METRIC (reps >{WARMUP}) ===")
print(f"  {'arm':<6s} {'n':>2s} {'min':>7s} {'p25':>7s} {'med':>7s} {'p75':>7s} {'max':>8s} "
      f"{'mean':>8s} {'CV':>6s} {'vs PV':>8s}")
SP = {}
for a in arms:
    gg = [r for r in recs if r[0] == a]
    v = [spa(r) for r in gg]
    SP[a] = v
    cv = 100 * st.stdev(v) / st.mean(v) if len(v) > 1 else 0
    rel = ""
    if "pv" in SP and a != "pv" and st.mean(SP["pv"]):
        rel = f"{100*(st.mean(SP['pv'])-st.mean(v))/st.mean(SP['pv']):+7.2f}%"
    print(f"  {a:<6s} {len(v):>2d} {min(v):7.0f} {q(v,.25):7.0f} {st.median(v):7.0f} {q(v,.75):7.0f} "
          f"{max(v):8.0f} {st.mean(v):8.0f} {cv:5.1f}% {rel:>8s}")
if "pv" in SP and len(SP["pv"]) > 1:
    print(f"\n  spin cost terms (+ = that component COSTS on-CPU spin):")
    sdp = {a: st.stdev(SP[a])/len(SP[a])**.5 for a in SP if len(SP[a]) > 1}
    mp = {a: st.mean(SP[a]) for a in SP}
    for lbl, x, y in (("migration alone (migas-as)", "migas", "as"),
                      ("AS alone        (migas-mig)", "migas", "mig"),
                      ("AS +pedestal    (as-pv)", "as", "pv"),
                      ("full stack      (migas-pv)", "migas", "pv")):
        if x in mp and y in mp and x in sdp and y in sdp:
            d = mp[x]-mp[y]; e = (sdp[x]**2 + sdp[y]**2)**.5
            print(f"    {lbl:<30s} {d:+9.0f} ns +/- {e:6.0f} se   "
                  f"{'RESOLVED' if abs(d) > 2*e else 'unresolved'}")

print(f"\n  === TOTAL WAIT PER ACQUISITION, ns (residence = spin + halt; reps >{WARMUP}) ===")
print(f"  {'arm':<6s} {'n':>2s} {'min':>7s} {'p25':>7s} {'med':>7s} {'p75':>7s} {'max':>8s} "
      f"{'mean':>8s} {'CV':>6s} {'halt%':>6s} {'haltrate':>8s} {'ent':>7s} {'sec':>6s} {'hwait%':>7s}")
S = {}
for a in arms:
    gg = [r for r in recs if r[0] == a]
    v = [wpa(r) for r in gg]
    S[a] = dict(v=v, gg=gg)
    tw, th = sum(g(r, "waitns") for r in gg), sum(g(r, "haltns") for r in gg)
    he, te = sum(g(r, "halte") for r in gg), sum(g(r, "ent") for r in gg)
    cv = 100 * st.stdev(v) / st.mean(v) if len(v) > 1 else 0
    print(f"  {a:<6s} {len(v):>2d} {min(v):7.0f} {q(v,.25):7.0f} {st.median(v):7.0f} {q(v,.75):7.0f} "
          f"{max(v):8.0f} {st.mean(v):8.0f} {cv:5.1f}% {100*th/tw:5.1f}% {100*he/te:7.1f}% "
          f"{te/len(gg):7.0f} {st.mean(g(r,'sec') for r in gg):6.2f} {st.mean(hfrac(r) for r in gg):6.1f}%")

if "pv" in S:
    base = st.mean(S["pv"]["v"])
    print(f"\n  vs stock PV (ratio-of-means; + = AS waits LESS per acquisition):")
    for a in arms:
        if a == "pv":
            continue
        print(f"    {a:<6s} {100*(base-st.mean(S[a]['v']))/base:+7.2f}%")

print(f"\n  === Q1: COST SPLIT (mean ns per acquisition; + = that component COSTS) ===")
if {"pv", "mig", "as", "migas"} <= set(S):
    m = {a: st.mean(S[a]["v"]) for a in S}
    sd = {a: (st.stdev(S[a]["v"]) / len(S[a]["v"]) ** .5 if len(S[a]["v"]) > 1 else 0) for a in S}
    def term(lbl, x, y):
        d = m[x] - m[y]; e = (sd[x] ** 2 + sd[y] ** 2) ** .5
        verdict = "RESOLVED" if abs(d) > 2 * e else "unresolved (|d| < 2se)"
        print(f"    {lbl:<30s} {d:+9.0f} ns  +/- {e:6.0f} se   {verdict}")
    print(f"    {'stock PV baseline':<30s} {m['pv']:9.0f} ns")
    term("migration alone (migas-as)", "migas", "as")
    term("AS alone        (migas-mig)", "migas", "mig")
    term("mig+pedestal    (mig-pv)", "mig", "pv")
    term("AS +pedestal    (as-pv)", "as", "pv")
    term("full stack      (migas-pv)", "migas", "pv")

print(f"\n  === Q2: DETECTOR vs HOST GROUND TRUTH ===")
for a in arms:
    if a in ("pv", "mig"):
        continue
    gg, v = S[a]["gg"], S[a]["v"]
    med = st.median(v)
    print(f"\n  -- {a} --   (disaster = wait/acq > 2x this arm's median)")
    print(f"     {'rep':>4s} {'wait/acq':>9s} {'halt%':>6s} {'t2chk':>6s} {'t2f':>6s} {'t2%':>6s} "
          f"{'hwait%':>7s} {'hsw/s':>7s} {'kind':>9s}")
    for r in gg:
        w = wpa(r); t2c, t2f = g(r, "t2chk"), g(r, "t2f")
        hs = 100 * g(r, "haltns") / max(g(r, "waitns"), 1)
        print(f"     {int(g(r,'rep')):>4d} {w:9.0f} {hs:5.1f}% {t2c:6.0f} {t2f:6.0f} "
              f"{(f'{100*t2f/t2c:.2f}%' if t2c else '-'):>6s} {hfrac(r):6.1f}% "
              f"{g(r,'hsw')/max(g(r,'sec'),1):7.0f} {('DISASTER' if w>2*med else ''):>9s}")
    dis = [r for r in gg if wpa(r) > 2 * med]
    ok = [r for r in gg if wpa(r) <= 2 * med]
    if dis and ok:
        hwd, hwo = st.mean(hfrac(r) for r in dis), st.mean(hfrac(r) for r in ok)
        fd = st.mean(100 * g(r, "t2f") / max(g(r, "t2chk"), 1) for r in dis)
        fo = st.mean(100 * g(r, "t2f") / max(g(r, "t2chk"), 1) for r in ok)
        print(f"     DISASTERS n={len(dis)}: host wait {hwd:5.1f}% of vCPU time, fire {fd:5.2f}%")
        print(f"     NORMAL    n={len(ok)}: host wait {hwo:5.1f}% of vCPU time, fire {fo:5.2f}%")
        ratio = hwd / hwo if hwo else float("inf")
        print(f"     host-wait ratio disaster/normal = {ratio:.2f}x")
        if ratio > 1.15 and fd < fo * 1.5:
            print("     => REAL PREEMPTION the detector MISSED.")
        elif ratio <= 1.15:
            print("     => host wait FLAT on disasters: vips's tail is NOT vCPU preemption,")
            print("        so a preemption detector cannot fix it. Detector is not broken.")
        else:
            print("     => both up: it detects; halting just does not recover the time.")
    else:
        print(f"     (no disasters at this n -- {len(gg)} reps, all within 2x median)")
