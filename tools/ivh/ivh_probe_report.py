#!/usr/bin/env python3
"""Report an ivh_probe sweep: performance, coherence traffic, benefit/work.

Three families, per the evaluation plan:
  PERF       median throughput, as % over the stock-PV arm, AND the same
             result as % of wall time saved. BOTH are printed because the
             existing docs mix the two conventions depending on what the
             benchmark natively reports, and they are not interchangeable:
             hackbench PV 59.57s -> IVH 15.55s is "+73.9%" as time saved but
             "+283%" as throughput. fs_mark reports files/s, so its recorded
             +167% is already a throughput figure. Comparing a throughput
             gain against a recorded time-saving silently looks like a 3.3x
             discrepancy (it did, on 2026-09-27).
  COHERENCE  (RES+CAL+TLB) IPI per 1000 units of work. The PMU is not
             virtualised on this TDX guest and resctrl is absent, so this is
             coherence TRAFFIC, never a miss rate.
  RATIO      (preempted_cs_pv - preempted_cs_arm) / migrations_arm.
             /Mhold is the same preempted-CS count per million holds, shown
             because PV and IVH do not execute the same NUMBER of critical
             sections (4.94M vs 3.07M on hackbench), so the raw difference
             in RATIO mixes a rate change with a volume change.
             = preempted critical sections avoided per migration performed.

A benchmark is only reported in the ranking table if it clears +5% over PV.
A benchmark with no preempted-CS signal in the PV arm cannot produce a
RATIO at all -- fs_mark is the known case -- and is marked n/s rather than
being given a fabricated 0.
"""
import os, statistics as st, sys, collections

knob = os.environ.get("KNOB", "knob")
rows = collections.defaultdict(lambda: collections.defaultdict(list))
for ln in open(sys.argv[1]):
    f = ln.rstrip('\n').split('\t')
    if len(f) < 9:
        continue
    rows[f[0]][f[1]].append(dict(val=float(f[2]), migs=int(f[3]), ipi=int(f[4]),
                                 g1=int(f[5]), g2=int(f[6]), lng=int(f[7]),
                                 tot=int(f[8]),
                                 dur=float(f[9]) if len(f) > 9 else 0.0))


def lab(k):
    if k == "pv":
        return "PV"
    n = int(k)
    return f"{n/1e6:g}ms" if n >= 100000 else str(n)


summary = {}
for bench, arms in rows.items():
    if "pv" not in arms:
        continue
    med = lambda k, f: st.median([r[f] for r in arms[k]])
    base = med("pv", "val")
    pv_lng = st.median([r["lng"] for r in arms["pv"]])
    pv_tot = st.median([r["tot"] for r in arms["pv"]])
    order = ["pv"] + sorted((k for k in arms if k != "pv"), key=int)
    print(f"\n########## {bench} ##########")
    print(f"{knob:>14} {'perf':>11} {'vs PV':>8} {'migs':>6} {'secs':>6} "
          f"{'IPI/kwork':>10} {'holds':>10} {'preCS':>6} {'/Mhold':>7} "
          f"{'ratio':>9}  {'g2rej':>8}")
    print(f"{'':14} {'(thr)':>11} {'(thr)':>8} {'':6} {'':6} "
          f"{'':10} {'':10} {'':6} {'':7} {'':9}  {'':8}")
    for k in order:
        v = med(k, "val")
        rel = 100 * (v - base) / base if base else 0
        # same result as % of wall time saved: t ~ 1/throughput
        tsave = 100 * (1 - base / v) if v else 0
        work = v * med(k, "dur")          # throughput x wall-clock
        ipik = med(k, "ipi") / (work / 1000.0) if work else 0
        lng, migs = med(k, "lng"), med(k, "migs")
        if k == "pv":
            ratio = "--"
        elif pv_lng == 0:
            ratio = "n/s"
        elif migs == 0:
            ratio = "nomig"
        else:
            ratio = f"{(pv_lng - lng)/migs:9.4f}"
        tot_h = med(k, "tot")
        per_m = (1e6 * lng / tot_h) if tot_h else 0
        print(f"{lab(k):>14} {v:11.1f} {rel:+7.1f}% {migs:6.0f} {med(k,'dur'):6.1f} "
              f"{ipik:10.1f} {tot_h:10.0f} {lng:6.0f} {per_m:7.1f} "
              f"{ratio:>9}  {med(k,'g2'):8.0f}")
    if pv_lng == 0:
        print(f"  NOTE: PV arm recorded {pv_tot:.0f} holds but ZERO >=476us -- this")
        print( "        benchmark carries no preempted-CS signal, so RATIO is n/s.")
        print( "        Check ivh_cs_owner_enable survived spin_mode if this is unexpected.")
    print("  as % WALL TIME SAVED (the convention the older docs use for "
          "time-reported benchmarks):")
    for k in order:
        if k == "pv":
            continue
        v = med(k, "val")
        print(f"      {lab(k):>10}  {100*(1-base/v) if v else 0:+6.1f}%  "
              f"(= {100*(v-base)/base if base else 0:+.1f}% throughput)")
    sw = [k for k in order if k != "pv"]
    if sw:
        best = max(sw, key=lambda k: med(k, "val"))
        spread = [med(k, "val") for k in sw]
        allv = [r["val"] for k in sw for r in arms[k]]
        noise = st.stdev(allv) if len(allv) > 1 else 0
        gap = max(spread) - min(spread)
        print(f"  best perf at {lab(best)}; spread {gap:.1f} vs pooled sd {noise:.1f}"
              f"{'  -> INSIDE NOISE, knob does not pick a winner' if gap < noise else ''}")
        m = [med(k, "migs") for k in sw]
        if max(m) == min(m) == 0:
            print("  *** NO MIGRATIONS IN ANY ARM -- check ivh_preempt_event_source=2")
        summary[bench] = dict(rel=100*(med(best,'val')-base)/base if base else 0,
                              best=lab(best), pv_lng=pv_lng)

print("\n\n########## benchmarks clearing +5% over PV ##########")
keep = {b: s for b, s in summary.items() if s["rel"] >= 5}
if not keep:
    print("  none")
for b, s in sorted(keep.items(), key=lambda x: -x[1]["rel"]):
    sig = "has preempted-CS signal" if s["pv_lng"] > 0 else "NO preempted-CS signal (ratio n/s)"
    print(f"  {b:16} {s['rel']:+8.1f}% at {s['best']:>8}   {sig}")
dropped = [b for b, s in summary.items() if s["rel"] < 5]
if dropped:
    print(f"\n  below +5%, excluded: {', '.join(dropped)}")
