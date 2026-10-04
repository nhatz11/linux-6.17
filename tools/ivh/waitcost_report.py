import sys, os, statistics as st
f, wl = sys.argv[1], sys.argv[2]
# Mechanism cost per migration, ns. MECH = t_onrq - t_commit from migcost_light.bt.
# NEVER add DELAY or the syscall duration: a migrating ivh_cs_enter blocks through
# the move, so its duration already CONTAINS cost+delay.
MECH = float(os.environ.get('MECH_NS', 13000))
rows = [l.split('\t') for l in open(f).read().splitlines()[1:] if l.strip()]
r = [x for x in rows if x[0] == wl and x[4] != 'NA']
if not r: print(f"  {wl}: no data"); sys.exit()
metric = r[0][1]
by = {}
for x in r:
    by.setdefault(x[2], []).append(dict(val=float(x[4]), wait=int(x[5]), ev=int(x[6]), migs=int(x[8])))
if 'pv_t1' not in by or 'mig_t1' not in by: print(f"  {wl}: incomplete arms"); sys.exit()
A, B = by['pv_t1'], by['mig_t1']
n = min(len(A), len(B))
def mean(d, k): return st.mean([x[k] for x in d])
def cv(d, k):
    v = [x[k] for x in d]
    return 100*st.stdev(v)/st.mean(v) if len(v) > 1 and st.mean(v) else 0.0
pa, pb = mean(A,'val'), mean(B,'val')
perf = 100*(pa-pb)/pa if metric == 'TIME' else 100*(pb-pa)/pa
wa, wb = mean(A,'wait'), mean(B,'wait')
mg = mean(B,'migs')
mcost = mg * MECH                      # total migration mechanism cost, ns
ca, cb = wa + mean(A,'migs')*MECH, wb + mcost
dwait = 100*(wa-wb)/wa if wa else 0
dcost = 100*(ca-cb)/ca if ca else 0
print(f"  >> {wl:<10s} n={n}  perf {perf:+7.2f}%  (pv {pa:.1f} CV{cv(A,'val'):.1f}% -> mig {pb:.1f} CV{cv(B,'val'):.1f}%)")
print(f"     wait  pv {wa/1e6:10.1f} ms -> mig {wb/1e6:10.1f} ms   {dwait:+7.2f}%   (saved {(wa-wb)/1e6:+.1f} ms)")
print(f"     migs  {mg:8.0f}/run x {MECH/1000:.1f} us = {mcost/1e6:8.1f} ms migration cost")
print(f"     COST  pv {ca/1e6:10.1f} ms -> mig {cb/1e6:10.1f} ms   {dcost:+7.2f}%")
verdict = ("wait saving EXCEEDS migration cost" if (wa-wb) > mcost
           else "migration cost EXCEEDS the wait saving")
ratio = (wa-wb)/mcost if mcost else float('inf')
print(f"     ==>   {verdict};  saving/cost = {ratio:6.2f}x")
if cv(A,'val') > 20 or cv(B,'val') > 20:
    print(f"     !! perf CV >20% (pv {cv(A,'val'):.0f}% mig {cv(B,'val'):.0f}%) -- add reps")
