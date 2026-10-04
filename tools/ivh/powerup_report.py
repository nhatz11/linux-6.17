#!/usr/bin/env python3
import sys, statistics as st, math
NS=26e-9
f=sys.argv[1]; want=sys.argv[2] if len(sys.argv)>2 else None
r=[l.split('\t') for l in open(f).read().splitlines()[1:] if l.strip()]
LOWER={'vips'}
for wl in ([want] if want else ['ebizzy','vips']):
    wr=[x for x in r if x[0]==wl and x[3] not in ('NA','')]
    by={}
    for x in wr: by.setdefault(x[2],{})[x[1]]=x
    sp=[];pf=[];sec=[]
    for rep,g in sorted(by.items(), key=lambda z:int(z[0])):
        if 'pv' not in g or 'as' not in g: continue
        p,a=g['pv'],g['as']
        R = 1.0 if wl in LOWER else float(a[3])/float(p[3])
        t0=(int(p[4])+int(p[5]))*NS*R; t1=(int(a[4])+int(a[5]))*NS
        if t0<=0: continue
        sp.append(100*(t0-t1)/t0); sec.append((t0-t1)*1000)
        pf.append(100*(float(p[3])-float(a[3]))/float(p[3]) if wl in LOWER
                  else 100*(float(a[3])-float(p[3]))/float(p[3]))
    n=len(sp)
    if n<4: print(f"  {wl}: n={n}, too few"); continue
    tc=1.96+2.4/n
    def ci(v):
        m=st.mean(v); se=st.stdev(v)/math.sqrt(n); return m, m-tc*se, m+tc*se
    print(f"\n=== {wl}, n={n} ===")
    for lbl,v,u in (("SPIN %",sp,"%"),("SPIN abs",sec," ms"),("PERF %",pf,"%")):
        m,lo,hi=ci(v)
        tag = "POSITIVE" if lo>0 else ("NEGATIVE" if hi<0 else "neutral (CI spans 0)")
        print(f"  {lbl:<9s} mean {m:+8.2f}{u}  CI [{lo:+7.2f},{hi:+7.2f}]  {tag}  ({sum(1 for z in v if z>0)}/{n} pos)")
    ms,los,his=ci(sp); mp,lop,hip=ci(pf)
    ok = (his>0 and ms>0) and lop>=-1.0
    print(f"  GOAL: spin not-negative AND perf >= -1%  ->  {'PASS' if ok else 'FAIL'}"
          f"   [spin CI hi {his:+.2f}, perf CI lo {lop:+.2f}]")
