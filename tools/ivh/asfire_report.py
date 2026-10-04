#!/usr/bin/env python3
"""Per-mechanism firing census for as_fire_vs_threshold.sh.

The point is the RATE, not the count. "tier2_enable=1" tells you nothing; a
mechanism that is checked 4 million times and fires 0 is inert, and an arm whose
only live mechanism is tier 1 is mig+tier1 wearing an AS label.

Rep 1 is dropped: floortest_1004-014814 showed a warm-up regime over reps 1-4
(PV mean 18.2 -> 10.2 ms, as32768 132.5 -> 13.5) that inverted the verdict
between n=4 and n=12.
"""
import sys
import statistics as st

NS = 26e-9
DROP_REPS = {1}

rows = [l.split("\t") for l in open(sys.argv[1]).read().splitlines() if l.strip()]
hdr, rows = rows[0], rows[1:]
ix = {k: i for i, k in enumerate(hdr)}


def g(r, k):
    return float(r[ix[k]])


recs = [r for r in rows if r[ix["sec"]] not in ("NA", "") and int(r[ix["rep"]]) not in DROP_REPS]
arms = []
for r in recs:
    if r[0] not in arms:
        arms.append(r[0])

print(f"\n  === MECHANISM FIRING CENSUS (vips, rep1 dropped) ===")
print(f"  {'arm':<6s} {'n':>2s} {'tier1/s':>9s} {'tier2':>14s} {'HEH fire':>14s} "
      f"{'HEH young':>11s} {'skip req':>9s} {'la refused':>10s}")
for a in arms:
    g_ = [r for r in recs if r[0] == a]
    if not g_:
        continue
    sec = sum(g(r, "sec") for r in g_)
    t1 = sum(g(r, "t1f") for r in g_)
    t2c, t2f = sum(g(r, "t2chk") for r in g_), sum(g(r, "t2f") for r in g_)
    csc, csf = sum(g(r, "cschk") for r in g_), sum(g(r, "csf") for r in g_)
    yg = sum(g(r, "csyoung") for r in g_)
    ev = sum(g(r, "evreq") for r in g_)
    la = sum(g(r, "evlaref") for r in g_)
    t2r = f"{t2f:,.0f} ({100*t2f/t2c:.3f}%)" if t2c else f"{t2f:,.0f} (no chk)"
    csr = f"{csf:,.0f} ({100*csf/csc:.3f}%)" if csc else f"{csf:,.0f} (no chk)"
    print(f"  {a:<6s} {len(g_):>2d} {t1/sec:9,.0f} {t2r:>14s} {csr:>14s} "
          f"{yg/sec:11,.0f} {ev/sec:9,.0f} {la/sec:10,.0f}")

print(f"\n  === OUTCOME (ms spin, seconds) ===")
print(f"  {'arm':<6s} {'n':>2s} {'node':>7s} {'head':>7s} {'TOTAL':>7s} {'us/entry':>9s} "
      f"{'sec':>7s} {'cap':>5s} {'vs pv spin':>11s} {'vs pv sec':>10s}")
base = {}
for a in arms:
    g_ = [r for r in recs if r[0] == a]
    if not g_:
        continue
    nd = st.mean(g(r, "node") * NS * 1000 for r in g_)
    hd = st.mean(g(r, "head") * NS * 1000 for r in g_)
    sec = st.mean(g(r, "sec") for r in g_)
    cap = st.mean(g(r, "cap") for r in g_)
    ent = sum(g(r, "ent") for r in g_)
    tot = nd + hd
    upe = (sum((g(r, "node") + g(r, "head")) for r in g_) * NS * 1e6) / max(ent, 1)
    if a == "pv":
        base = dict(tot=tot, sec=sec)
    ds = f"{100*(base['tot']-tot)/base['tot']:+10.1f}%" if base else " " * 11
    dp = f"{100*(base['sec']-sec)/base['sec']:+9.1f}%" if base else " " * 10
    print(f"  {a:<6s} {len(g_):>2d} {nd:7.1f} {hd:7.1f} {tot:7.1f} {upe:9.3f} "
          f"{sec:7.2f} {cap:5.0f} {ds} {dp}")
print("\n  (+ = AS better. spin = node+head iterations x 26ns.)")
