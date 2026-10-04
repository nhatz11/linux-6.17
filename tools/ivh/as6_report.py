#!/usr/bin/env python3
"""Goal test: EVERY workload must save spin AND be perf >= -1% (neutral-or-positive).
THROUGHPUT saved = spin_PV*(ops_AS/ops_PV) - spin_AS ; TIME saved = spin_PV - spin_AS"""
import sys, statistics as st
NS=26e-9
f=sys.argv[1]; want=sys.argv[2] if len(sys.argv)>2 else None
r=[l.split('\t') for l in open(f).read().splitlines()[1:] if l.strip()]
def num(x):
    try: return float(x)
    except Exception: return None
wls=[want] if want else list(dict.fromkeys(x[0] for x in r))
arms=[a for a in dict.fromkeys(x[2] for x in r) if a!='pv']
res={a:[] for a in arms}
for w in wls:
    wr=[x for x in r if x[0]==w and num(x[4]) is not None]
    if not wr: continue
    ty=wr[0][1]; by={}
    for x in wr: by.setdefault(x[3],{})[x[2]]=x
    pv=[v['pv'] for v in by.values() if 'pv' in v]
    if not pv: continue
    s0=st.mean(int(x[5]) for x in pv)*NS; o0=st.mean(num(x[4]) for x in pv)
    if not o0 or s0<=0: print(f"  {w:<20s} PV spin {s0:.4f}s -- unscorable"); continue
    print(f"\n  {w} [{ty}]  PV spin {s0:.2f}s  cap {st.mean(int(x[10]) for x in wr):.0f}")
    for a in arms:
        g=[v[a] for v in by.values() if a in v]
        if not g: continue
        s1=st.mean(int(x[5]) for x in g)*NS; o1=st.mean(num(x[4]) for x in g)
        saved = s0*(o1/o0)-s1 if ty=='THROUGHPUT' else s0-s1
        spd=[];pfd=[]
        for rep,gg in by.items():
            if 'pv' not in gg or a not in gg: continue
            a0,a1=num(gg['pv'][4]),num(gg[a][4]); i0,i1=int(gg['pv'][5]),int(gg[a][5])
            if not a0 or not i0: continue
            spd.append(100*(i0-i1)/i0)
            pfd.append(100*(a1-a0)/a0 if ty=='THROUGHPUT' else 100*(a0-a1)/a0)
        if not spd: continue
        sm, pm = st.median(spd), st.median(pfd)
        ok = "PASS" if (sm>0 and pm>=-1.0) else "FAIL"
        res[a].append((w, sm, pm, ok))
        print(f"    {a:>5s}us spin {sm:+7.2f}% ({sum(1 for z in spd if z>0)}/{len(spd)})  "
              f"perf {pm:+7.2f}% ({sum(1 for z in pfd if z>0)}/{len(pfd)})  saved {saved:+7.2f}s  [{ok}]")
        print(f"          spin {[round(z,1) for z in spd]}")
        print(f"          perf {[round(z,1) for z in pfd]}")
if not want:
    print("\n" + "="*70)
    print("GOAL: spin saved AND perf >= -1% on EVERY workload")
    for a in arms:
        d=res[a]
        if not d: continue
        npass=sum(1 for _,_,_,o in d if o=="PASS")
        print(f"  {a}us: {npass}/{len(d)} PASS" + ("   *** GOAL MET ***" if npass==len(d) and len(d)>=5 else ""))
        for w,sm,pm,o in d:
            if o=="FAIL": print(f"      FAIL {w:<22s} spin {sm:+7.2f}%  perf {pm:+7.2f}%")
