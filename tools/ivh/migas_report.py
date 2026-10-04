#!/usr/bin/env python3
"""Paired-per-rep report for migas_vs_pv.sh (full IVH stack vs stock PV).
THROUGHPUT: saved = spin_PV*(ops_AS/ops_PV) - spin_AS   TIME: saved = spin_PV - spin_AS"""
import sys, statistics as st
NS=26e-9
r=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
def num(x):
    try: return float(x)
    except Exception: return None
print("\n"+"="*108)
print("FULL IVH STACK (migration + AS @50us) vs STOCK PV -- paired per rep".center(108))
print("spin = node_spin_iters x 26ns (NODE only).  + = IVH better".center(108))
print("="*108)
print(f"  {'workload':<20s} {'type':>5s} {'PV spin':>8s} {'IVH spin':>9s} {'saved':>9s} "
      f"{'SPIN med':>9s} {'PERF med':>9s} {'migs':>8s} {'t2 fires':>9s} {'cap':>4s}")
print("  "+"-"*104)
sp_all=[]; pf_all=[]
for w in dict.fromkeys(x[0] for x in r):
    wr=[x for x in r if x[0]==w and num(x[4]) is not None]
    if not wr: continue
    ty=wr[0][1]; by={}
    for x in wr: by.setdefault(x[3],{})[x[2]]=x
    pv=[v['pv'] for v in by.values() if 'pv' in v]; asr=[v['as'] for v in by.values() if 'as' in v]
    if not(pv and asr): continue
    s0=st.mean(int(x[5]) for x in pv)*NS; s1=st.mean(int(x[5]) for x in asr)*NS
    o0=st.mean(num(x[4]) for x in pv);    o1=st.mean(num(x[4]) for x in asr)
    if not o0 or s0<=0:
        print(f"  {w:<20s} {ty[:5]:>5s}  PV spin {s0:.4f}s -- too little spin to score"); continue
    saved = s0*(o1/o0)-s1 if ty=='THROUGHPUT' else s0-s1
    spd=[];pfd=[]
    for rep,g in by.items():
        if 'pv' not in g or 'as' not in g: continue
        a0,a1=num(g['pv'][4]),num(g['as'][4]); i0,i1=int(g['pv'][5]),int(g['as'][5])
        if not a0 or not i0: continue
        spd.append(100*(i0-i1)/i0)
        pfd.append(100*(a1-a0)/a0 if ty=='THROUGHPUT' else 100*(a0-a1)/a0)
    mg=st.mean(int(x[10]) for x in asr); t2=st.mean(int(x[7]) for x in asr)
    cap=st.mean(int(x[11]) for x in wr)
    sp_all+=spd; pf_all+=pfd
    print(f"  {w:<20s} {ty[:5]:>5s} {s0:7.2f}s {s1:8.2f}s {saved:+8.2f}s "
          f"{st.median(spd):+8.2f}% {st.median(pfd):+8.2f}% {mg:8,.0f} {t2:9,.0f} {cap:4.0f}")
    print(f"  {'':20s} {'':>5s}  per-rep spin {[round(z,1) for z in spd]}  perf {[round(z,1) for z in pfd]}")
if sp_all:
    print("\n  POOLED over all reps/workloads:")
    print(f"    SPIN median {st.median(sp_all):+6.2f}%  ({sum(1 for z in sp_all if z>0)}/{len(sp_all)} reps less spin)")
    print(f"    PERF median {st.median(pf_all):+6.2f}%  ({sum(1 for z in pf_all if z>0)}/{len(pf_all)} reps faster)")
print("="*108)
