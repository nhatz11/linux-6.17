#!/usr/bin/env python3
import sys, statistics as st
r=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
for w in dict.fromkeys(x[0] for x in r):
    rows=[x for x in r if x[0]==w and x[6] not in('NA','')]
    if not rows: continue
    met=rows[0][1]; by={}
    for x in rows: by.setdefault(x[5],{})[x[2]]=x
    print(f"\n== {w} [{met}] == paired vs mig+t1; FIRE COUNTS are the primary outcome")
    print(f"  {'arm':>10s} {'n':>2s} {'PERF':>8s} {'SPIN':>8s} | {'tier2':>10s} {'bypass':>8s} {'heh':>8s} {'evict':>8s}")
    for a in [z for z in dict.fromkeys(x[2] for x in rows) if z!='mig']:
        pf,sp,t2,hb,cs,ev=[],[],[],[],[],[]
        for rep,d in sorted(by.items()):
            if 'mig' not in d or a not in d: continue
            M,A=d['mig'],d[a]
            mv,av=float(M[6]),float(A[6]); ms,as_=int(M[7]),int(A[7])
            pf.append(100*(av-mv)/mv if met=='THROUGHPUT' else 100*(mv-av)/mv)
            sp.append(100*(ms-as_)/ms if ms else 0)
            t2.append(int(A[9])); hb.append(int(A[11])); cs.append(int(A[12])); ev.append(int(A[13]))
        if not pf: continue
        dead="".join(" DEAD:"+k for k,v in [("t2",t2),("hb",hb),("heh",cs),("ev",ev)] if st.mean(v)==0)
        print(f"  {a:>10s} {len(pf):>2d} {st.median(pf):+7.2f}% {st.median(sp):+7.2f}% | "
              f"{st.mean(t2):10,.0f} {st.mean(hb):8,.0f} {st.mean(cs):8,.0f} {st.mean(ev):8,.0f}{dead}")
