#!/usr/bin/env python3
"""PER-REP PAIRED deltas vs that rep's own mig+t1. Median + sign test."""
import sys, statistics as st
f=sys.argv[1]; want=sys.argv[2] if len(sys.argv)>2 else None
rows=[l.split('\t') for l in open(f).read().splitlines()[1:] if l.strip()]
for w in ([want] if want else list(dict.fromkeys(r[0] for r in rows))):
    rr=[x for x in rows if x[0]==w and x[4] not in('NA','')]
    if not rr: continue
    met=rr[0][1]
    byrep={}
    for x in rr: byrep.setdefault(x[3],{})[x[2]]=x
    print(f"\n== {w} [{met}]   paired vs mig+t1, per rep")
    for a in ['as255']:
        dv,ds,cap=[],[],[]
        for rep,d in sorted(byrep.items()):
            if 'mig' not in d or a not in d: continue
            M,A=d['mig'],d[a]
            mv,av=float(M[4]),float(A[4]); msp,asp=int(M[5]),int(A[5])
            dv.append(100*(mv-av)/mv if met=='TIME' else 100*(av-mv)/mv)   # perf, +=better
            ds.append(100*(msp-asp)/msp)                                   # spin, +=less spin
            cap.append(int(A[15]))
        if not dv: continue
        pos_s=sum(1 for z in ds if z>0); pos_v=sum(1 for z in dv if z>0)
        print(f"  {a:>7s} n={len(dv)}  PERF median {st.median(dv):+7.2f}%  ({pos_v}/{len(dv)} better)   "
              f"SPIN median {st.median(ds):+7.2f}%  ({pos_s}/{len(ds)} less spin)")
        print(f"          perf deltas {[round(z,1) for z in dv]}")
        print(f"          spin deltas {[round(z,1) for z in ds]}   cap_mean {cap}")
