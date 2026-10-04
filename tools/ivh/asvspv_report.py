#!/usr/bin/env python3
import sys
NS=26e-9
r=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
print("\n=== AS vs STOCK PV, migration OFF both arms ===")
print("Metric: node_spin_iters * 26ns (spin_time_measurement.md). Ratio is exact.")
print("Scope: NODE spin only; head (~20-27% of total) uninstrumented.\n")
print(f"  {'workload':<20s} {'pv spin':>9s} {'AS spin':>9s} {'SAVED':>9s} {'ratio':>7s} {'perf':>8s} {'t2 fires':>10s}")
for w in dict.fromkeys(x[0] for x in r):
    g0=[x for x in r if x[0]==w and x[2]=='pv']; g1=[x for x in r if x[0]==w and x[2]=='as']
    if not(g0 and g1): continue
    i0=sum(int(x[5]) for x in g0); i1=sum(int(x[5]) for x in g1)
    if not i0: continue
    met=g0[0][1]
    try:
        v0=sum(float(x[4]) for x in g0)/len(g0); v1=sum(float(x[4]) for x in g1)/len(g1)
        pf=(100*(v1-v0)/v0) if met=='THROUGHPUT' else (100*(v0-v1)/v0 if v0 else 0)
    except Exception: pf=float('nan')
    t2=sum(int(x[9]) for x in g1)/max(len(g1),1)
    print(f"  {w:<20s} {i0*NS:8.1f}s {i1*NS:8.1f}s {(i0-i1)*NS:+8.1f}s {i1/i0:7.3f} {pf:+7.2f}% {t2:10,.0f}")
print("\n  ratio < 1 = AS spun LESS than stock PV.  + SAVED = wait time removed.")
