#!/usr/bin/env python3
"""Floor/ceiling + per-mechanism firing census for floor_test2.sh.

Two questions, two tables:
  1. DISTRIBUTION -- does AS lower the ceiling (p90/max) without raising the
     floor (min/p10)? The mean hides both effects because they cancel.
  2. CENSUS -- which mechanism actually FIRED? An arm whose only live mechanism
     is tier 1 is mig+tier1 wearing an AS label, and its spin delta is overhead.

Reps 1-4 are dropped as warm-up: floortest_1004-014814 inverted its verdict
between n=4 and n=12 (PV mean 18.2 -> 10.2 ms, as32768 132.5 -> 13.5 ms).
"""
import sys
import statistics as st

NS = 26e-9
import os
WARMUP = int(os.environ.get("WARMUP", 4))

rows = [l.split("\t") for l in open(sys.argv[1]).read().splitlines() if l.strip()]
hdr, rows = rows[0], rows[1:]
ix = {k: i for i, k in enumerate(hdr)}
arms = ["pv", "as32768", "as32767"]
_seen = {l.split("\t")[0] for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()}
arms = [a for a in arms if a in _seen]


def g(r, k):
    return float(r[ix[k]])


recs = [r for r in rows if r[ix["sec"]] not in ("NA", "") and int(r[ix["rep"]]) > WARMUP]
nrep = max((int(r[ix["rep"]]) for r in rows if r[ix["sec"]] not in ("NA", "")), default=0)
if not recs:
    print(f"  only {nrep} reps so far, all warm-up (need > {WARMUP})")
    sys.exit()


def q(v, p):
    v = sorted(v)
    i = p * (len(v) - 1)
    lo, hi = int(i), min(int(i) + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (i - lo)


print(f"\n  === SPIN DISTRIBUTION, ms (reps {WARMUP+1}-{nrep}; warm-up dropped) ===")
print(f"  {'arm':<9s} {'n':>2s} {'MIN':>7s} {'p10':>7s} {'p25':>7s} {'med':>7s} {'p75':>7s} "
      f"{'p90':>7s} {'MAX':>8s} {'us/ent':>7s} {'sec':>6s} {'cap':>5s}")
D = {}
for a in arms:
    gg = [r for r in recs if r[0] == a]
    if not gg:
        continue
    v = [(g(r, "node") + g(r, "head")) * NS * 1000 for r in gg]
    D[a] = v
    ent = sum(g(r, "ent") for r in gg)
    upe = sum((g(r, "node") + g(r, "head")) for r in gg) * NS * 1e6 / max(ent, 1)
    print(f"  {a:<9s} {len(v):>2d} {min(v):7.1f} {q(v,.10):7.1f} {q(v,.25):7.1f} {st.median(v):7.1f} "
          f"{q(v,.75):7.1f} {q(v,.90):7.1f} {max(v):8.1f} {upe:7.3f} "
          f"{st.mean(g(r,'sec') for r in gg):6.2f} {st.mean(g(r,'cap') for r in gg):5.0f}")

if "pv" in D:
    pv = D["pv"]
    print(f"\n  vs stock PV (+ = AS better):")
    for a in [x for x in arms if x != "pv"]:
        if a not in D:
            continue
        v = D[a]
        print(f"    {a:<9s} floor {100*(min(pv)-min(v))/min(pv):+7.1f}%  "
              f"p10 {100*(q(pv,.1)-q(v,.1))/q(pv,.1):+7.1f}%  "
              f"med {100*(st.median(pv)-st.median(v))/st.median(pv):+7.1f}%  "
              f"p90 {100*(q(pv,.9)-q(v,.9))/q(pv,.9):+7.1f}%  "
              f"ceiling {100*(max(pv)-max(v))/max(pv):+7.1f}%")

print(f"\n  === MECHANISM FIRING CENSUS (per second of run) ===")
print(f"  {'arm':<9s} {'tier1/s':>10s} {'tier2':>15s} {'HEH':>15s} {'HEHyoung/s':>11s} "
      f"{'skipreq/s':>10s} {'VERDICT':>22s}")
for a in arms:
    gg = [r for r in recs if r[0] == a]
    if not gg:
        continue
    sec = sum(g(r, "sec") for r in gg)
    t1 = sum(g(r, "t1f") for r in gg)
    t2c, t2f = sum(g(r, "t2chk") for r in gg), sum(g(r, "t2f") for r in gg)
    csc, csf = sum(g(r, "cschk") for r in gg), sum(g(r, "csf") for r in gg)
    yg, ev = sum(g(r, "csyoung") for r in gg), sum(g(r, "evreq") for r in gg)
    live = [n for n, c in (("t1", t1), ("t2", t2f), ("HEH", csf), ("skip", ev)) if c > 0]
    t2s = f"{t2f:,.0f}({100*t2f/t2c:.3f}%)" if t2c else f"{t2f:,.0f}"
    css = f"{csf:,.0f}({100*csf/csc:.3f}%)" if csc else f"{csf:,.0f}"
    print(f"  {a:<9s} {t1/sec:10,.0f} {t2s:>15s} {css:>15s} {yg/sec:11,.0f} {ev/sec:10,.1f} "
          f"{('live: ' + '+'.join(live)) if live else 'ALL DEAD':>22s}")
print("\n  HEHyoung = holds shorter than the threshold -- the direct witness that")
print("  vips's waits are too short for the detector, not that it is mistuned.")
