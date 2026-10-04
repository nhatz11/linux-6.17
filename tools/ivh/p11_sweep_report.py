#!/usr/bin/env python3
import sys
NS=26e-9
f=sys.argv[1]; want=sys.argv[2] if len(sys.argv)>2 else None
r=[l.split('\t') for l in open(f).read().splitlines()[1:] if l.strip()]
for w in ([want] if want else list(dict.fromkeys(x[0] for x in r))):
    rows=[x for x in r if x[0]==w]
    pv=[x for x in rows if x[2]=='pv']
    if not pv: continue
    i0=sum(int(x[6]) for x in pv)/len(pv)
    try: v0=sum(float(x[5]) for x in pv)/len(pv)
    except Exception: v0=0
    met=pv[0][1]
    print(f"\n== {w} [{met}] == vs STOCK PV (migration off both). PV spin {i0*NS:.1f}s")
    print(f"  {'thr':>6s} {'n':>2s} {'spin':>8s} {'ratio':>7s} {'SAVED':>9s} {'perf':>8s} {'t2 fires':>10s} {'heh':>8s} {'ev':>6s}")
    for a in [x for x in dict.fromkeys(y[2] for y in rows) if x!='pv']:
        g=[x for x in rows if x[2]==a]
        if not g: continue
        i1=sum(int(x[6]) for x in g)/len(g)
        try:
            v1=sum(float(x[5]) for x in g)/len(g)
            pf=(100*(v1-v0)/v0) if met=='THROUGHPUT' else (100*(v0-v1)/v0 if v0 else 0)
        except Exception: pf=float('nan')
        t2=sum(int(x[10]) for x in g)/len(g); cs=sum(int(x[12]) for x in g)/len(g); ev=sum(int(x[13]) for x in g)/len(g)
        flag=" <-- BEST" if False else ""
        print(f"  {a:>6s} {len(g):>2d} {i1*NS:7.1f}s {i1/i0:7.3f} {(i0-i1)*NS:+8.1f}s {pf:+7.2f}% {t2:10,.0f} {cs:8,.0f} {ev:6,.0f}")
    print("  ratio < 1 = AS spun LESS than stock PV")
