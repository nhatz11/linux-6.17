#!/usr/bin/env python3
"""Turn a Phase 0b before/after pair into the one number that decides it."""
import sys, json, subprocess, re
sys.path.insert(0, "/root/ivh_tools")
from read_ivh_counters import IVH_ROT_CLASS_NAMES, IVH_BEAT_AGE_HIST_BUCKETS

b, a, wall = json.load(open(sys.argv[1])), json.load(open(sys.argv[2])), float(sys.argv[3])
d = lambda k: (a[k] - b[k]) if a.get(k) is not None else 0
da = lambda k: [x - y for x, y in zip(a[k], b[k])] if a.get(k) else []
m = re.search(r"tsc: Detected ([0-9.]+) MHz",
              subprocess.run(["dmesg"], capture_output=True, text=True).stdout)
MHZ = float(m.group(1)) if m else 2200.0
NB = IVH_BEAT_AGE_HIST_BUCKETS

h, p = d("ivh_rot_handoffs"), d("ivh_rot_preempted")
print(f"wall={wall:.2f}s  handoffs={h}  stale_successor={p}"
      f" ({100*p/h if h else 0:.4f}%)")

dh = da("ivh_rot_depth_hist")
if dh:
    found = sum(dh[1:-1])
    print(f"  skippable (live node behind) = {found}"
          f"   no live node = {dh[-1]}   [tail_stop={d('ivh_rot_tail_stop')},"
          f" a SUBSET of no-live, over-counts true tails]")

ev, cyc = da("ivh_rot_idle_events"), da("ivh_rot_idle_cycles")
unk, bwd, cap = d("ivh_rot_idle_unknown"), d("ivh_rot_idle_backward"), d("ivh_rot_idle_capped")
attr = sum(ev) if ev else 0
acks = attr + unk + bwd + cap
print(f"\nattribution: {attr}/{acks} = {100*attr/acks if acks else 0:.1f}%"
      f"   (unknown={unk} backward={bwd} capped={cap})")
if acks and attr < 0.8 * acks:
    print("  *** <80% attributed: histograms NOT representative, do not quote ***")

if ev:
    hist = da("ivh_rot_idle_hist")
    print("\nlock idle time by class (ABSOLUTE; class 0 is NOT a baseline to subtract):")
    for i, lab in enumerate(IVH_ROT_CLASS_NAMES):
        if not ev[i]:
            continue
        row = hist[i*NB:(i+1)*NB]
        tot = sum(row)
        med, run = 0, 0
        for bk, v in enumerate(row):
            run += v
            if run >= tot/2: med = bk; break
        print(f"  {lab:22s} n={ev[i]:<8d} total={cyc[i]/MHZ/1e6:.4f}s"
              f"  mean={cyc[i]/ev[i]/MHZ:.1f}us  median~{(2**med)/MHZ:.1f}us")
    rec = cyc[3] / MHZ / 1e6 if len(cyc) > 3 else 0.0
    print(f"\n>>> RECOVERABLE (stale+skippable, absolute) = {rec:.4f}s"
          f" = {100*rec/wall:.4f}% of wall time")
    print("    valid as a %% of wall only for a SINGLE-LOCK workload (qlockbench);")
    print("    meaningless for hackbench, which spreads over thousands of locks.")
print(f"\ncontrol: steals-from-a-queued-waiter = {d('ivh_rot_steals')}"
      f"  (cost already recovered without rotation)")
