#!/usr/bin/env python3
import sys, statistics as st, math
NS=26e-9
f=sys.argv[1]; want=sys.argv[2] if len(sys.argv)>2 else None
r=[l.split('\t') for l in open(f).read().splitlines()[1:] if l.strip()]
ths=[want] if want else sorted({x[0] for x in r}, key=int)
if not want: print("\n=== vips: AS firing rate vs damage, by threshold ===")
for th in ths:
    wr=[x for x in r if x[0]==th and x[3] not in ('NA','')]
    by={}
    for x in wr: by.setdefault(x[2],{})[x[1]]=x
    sp=[];pf=[];fires=[]
    for rep,g in by.items():
        if 'pv' not in g or 'as' not in g: continue
        p,a=g['pv'],g['as']
        t0=int(p[4])+int(p[5]); t1=int(a[4])+int(a[5])
        if t0<=0: continue
        sp.append(100*(t0-t1)/t0)
        pf.append(100*(float(p[3])-float(a[3]))/float(p[3]))
        fires.append(int(a[7])+int(a[8])+int(a[9]))
    n=len(sp)
    if n<3: print(f"  {th:>5s}us  n={n} too few"); continue
    tc=1.96+2.4/n
    def ci(v):
        m=st.mean(v); se=st.stdev(v)/math.sqrt(n); return m,m-tc*se,m+tc*se
    m,lo,hi=ci(sp); mp,lop,hip=ci(pf)
    sv = "POSITIVE" if lo>0 else ("NEGATIVE" if hi<0 else "neutral")
    pvd = "POSITIVE" if lop>0 else ("NEGATIVE" if hip<0 else "neutral")
    ok = (hi>0) and lop>=-1.0
    print(f"  {th:>5s}us n={n} fires/run {st.mean(fires):7.0f}  SPIN {m:+8.2f}% [{lo:+8.2f},{hi:+8.2f}] {sv:8s}"
          f"  PERF {mp:+7.2f}% [{lop:+7.2f},{hip:+7.2f}] {pvd:8s}  {'OK' if ok else '--'}")
