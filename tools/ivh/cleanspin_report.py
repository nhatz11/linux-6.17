#!/usr/bin/env python3
import sys, statistics as st
NS=26e-9
LOWER={'hackbench','vips'}   # value is seconds: lower is better, fixed work -> R=1
r=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
print("\n"+"="*96)
print("NODE + COUNTED-HEAD spin iterations -- both loops, NO stolen time".center(96))
print("conservative LOWER BOUND (head goto-gotlock successes excluded from both arms)".center(96))
print("="*96)
print(f"  {'workload':<11s} {'n':>2s} {'PV spin':>9s} {'AS spin':>9s} {'saved':>9s} {'reduction':>19s} {'perf':>8s}")
for wl in ('hackbench','memtier','dbench','ebizzy','vips'):
    wr=[x for x in r if x[0]==wl and x[3] not in ('NA','')]
    by={}
    for x in wr: by.setdefault(x[2],{})[x[1]]=x
    pct=[];sec=[];pf=[]
    for rep,g in sorted(by.items(), key=lambda z:int(z[0])):
        if 'pv' not in g or 'as' not in g: continue
        p,a=g['pv'],g['as']
        R = 1.0 if wl in LOWER else float(a[3])/float(p[3])
        t0=(int(p[4])+int(p[5]))*NS*R; t1=(int(a[4])+int(a[5]))*NS
        if t0<=0: continue
        pct.append(100*(t0-t1)/t0); sec.append(t0-t1)
        pf.append(100*(float(p[3])-float(a[3]))/float(p[3]) if wl in LOWER
                  else 100*(float(a[3])-float(p[3]))/float(p[3]))
    n=len(pct)
    if n<3: print(f"  {wl:<11s} -- too few pairs"); continue
    tc={3:4.30,4:3.18,5:2.78,6:2.57,7:2.45,8:2.36}.get(n,2.36)
    def ci(v):
        m=st.mean(v); se=st.stdev(v)/n**0.5; return m,m-tc*se,m+tc*se
    m,lo,hi=ci(pct); ms,_,_=ci(sec); mp,lop,_=ci(pf)
    sig="SIG" if (lo>0 or hi<0) else "ns "
    ok = "PASS" if (lo>0 and lop>=-1.0) else ("spin ok, perf?" if lo>0 else "FAIL")
    print(f"  {wl:<11s} {n:>2d} {ms/max(m/100,1e-9):8.2f}s {'':9s} {ms:+8.2f}s {m:+7.2f}% CI[{lo:+6.2f},{hi:+6.2f}] {sig} {mp:+7.2f}%  {ok}")
