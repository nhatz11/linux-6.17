import sys, statistics as st
# MECH per migration, ns -- MEASURED on this kernel with migcost_light.bt.
# MECH = t_onrq - t_commit. DELAY and the ivh_cs_enter duration are deliberately
# excluded: a migrating syscall blocks through the move, so its duration already
# CONTAINS cost+delay and summing them double-counts.
MECH = {'hackbench':3330,'memtier':1790,'ebizzy':3870,'nhextend':5980,'fsmark':3420,'dedup':5250}
DELAY= {'hackbench':241970,'memtier':94730,'ebizzy':624190,'nhextend':864010,'fsmark':28410,'dedup':1113250}
f=sys.argv[1]
rows=[l.split('\t') for l in open(f).read().splitlines()[1:] if l.strip()]
order=['hackbench','nhextend','ebizzy','fsmark','memtier','dedup']
print()
print("  workload    n   perf          wait pv ->  wait mig      wait saved   mig cost   saving/cost")
print("  " + "-"*96)
tab=[]
for wl in order:
    r=[x for x in rows if x[0]==wl and x[5]!='NA']
    if not r: continue
    metric=r[0][1]; by={}
    for x in r: by.setdefault(x[2],[]).append(dict(v=float(x[5]),w=int(x[6]),m=int(x[8])))
    if 'pv_t1' not in by or 'mig_t1' not in by: continue
    A,B=by['pv_t1'],by['mig_t1']; n=min(len(A),len(B))
    mn=lambda d,k: st.mean([y[k] for y in d])
    cvf=lambda d,k:(100*st.stdev([y[k] for y in d])/mn(d,k)) if len(d)>1 and mn(d,k) else 0
    pa,pb=mn(A,'v'),mn(B,'v')
    perf=100*(pa-pb)/pa if metric=='TIME' else 100*(pb-pa)/pa
    sa=st.stdev([y['v'] for y in A])/len(A)**.5 if len(A)>1 else 0
    sb=st.stdev([y['v'] for y in B])/len(B)**.5 if len(B)>1 else 0
    pse=100*((sa**2+sb**2)**.5)/pa
    wa,wb=mn(A,'w'),mn(B,'w'); migs=mn(B,'m')
    cost=migs*MECH[wl]; saved=wa-wb
    ratio=saved/cost if cost else float('inf')
    ca,cb=wa,wb+cost
    dcost=100*(ca-cb)/ca if ca else 0
    print(f"  {wl:<10s} {n:2d}  {perf:+7.2f}%+-{pse:4.2f}  {wa/1e6:9.1f} -> {wb/1e6:9.1f} ms  {saved/1e6:+10.1f} ms  {cost/1e6:7.1f} ms  {ratio:9.2f}x")
    tab.append(dict(wl=wl,n=n,perf=perf,pse=pse,wa=wa,wb=wb,saved=saved,cost=cost,ratio=ratio,
                    migs=migs,dcost=dcost,cva=cvf(A,'v'),cvb=cvf(B,'v'),delay=migs*DELAY[wl]))
print()
print("  WAIT COST (= lock wait + total migration cost), pv baseline vs mig+t1")
print("  " + "-"*96)
print("  workload    n   wait cost pv -> wait cost mig    change    verdict")
for t in tab:
    v = "wait saving EXCEEDS mig cost" if t['saved']>t['cost'] else "mig cost exceeds wait saving"
    print(f"  {t['wl']:<10s} {t['n']:2d}  {t['wa']/1e6:12.1f} -> {(t['wb']+t['cost'])/1e6:12.1f} ms  {t['dcost']:+7.2f}%  {v}")
print()
print("  migration cost as a share of the wait it removes, and the pessimistic bound")
print("  " + "-"*96)
print("  workload    migs/run   MECH cost    % of wait saved    +DELAY (bound)   still wins?")
for t in tab:
    pc = 100*t['cost']/t['saved'] if t['saved']>0 else float('nan')
    tot = t['cost']+t['delay']
    ok = "yes" if t['saved']>tot else "no"
    print(f"  {t['wl']:<10s} {t['migs']:9.0f}  {t['cost']/1e6:8.1f} ms  {pc:14.2f}%  {tot/1e6:13.1f} ms   {ok}")
hi=[t for t in tab if t['cva']>20 or t['cvb']>20]
if hi: print("\n  high-variance rows (CV>20%, add reps): " + ", ".join(f"{t['wl']}(pv {t['cva']:.0f}%/mig {t['cvb']:.0f}%)" for t in hi))
