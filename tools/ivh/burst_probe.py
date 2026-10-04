#!/usr/bin/env python3
"""Sample the quantity Gate 2 predicts from: rq->ivh_vact_last_active_c.

Gate 2 computes  runway = last_active - elapsed_since_active,
                 time_left = runway - last_cs_ns,
and rejects migration when time_left > ivh_time_left_threshold_ns (4 ms).

last_active is the length of ONE previous burst -- a single sample used to
predict the next one. If burst lengths are heavy-tailed, that predictor is
unreliable no matter how accurately each sample is measured. This polls the
field fast enough to catch every distinct value (it persists until the next
preemption) and reports the distribution.

usage: burst_probe.py [seconds]
"""
import sys, time, statistics as st
sys.path.insert(0,"/root/ivh_tools")
import read_vact_rq as r
KHZ=2200000
SECS=float(sys.argv[1]) if len(sys.argv)>1 else 20
LA, JUMPS = 3976, 3984

sym=r.load_kallsyms(); cpus=r.online_cpus()
f=open(r.KCORE,"rb"); ph=r.read_phdrs(f)
offs=[r.read_u64(f,ph,sym["__per_cpu_offset"]+8*c) for c in cpus]
base=sym["runqueues"]
def snap(field): return [r.read_u64(f,ph,base+field+o) for o in offs]

seen={c:[] for c in cpus}; last={c:None for c in cpus}
j0=snap(JUMPS); t0=time.time(); n=0
while time.time()-t0 < SECS:
    v=snap(LA); n+=1
    for i,c in enumerate(cpus):
        if v[i]!=last[c] and v[i]>0:
            seen[c].append(v[i]); last[c]=v[i]
j1=snap(JUMPS); dur=time.time()-t0
print(f"polled {n} times in {dur:.1f}s ({n/dur:.0f} Hz per-cpu sweep)\n")
print(f"{'cpu':>4}{'bursts seen':>12}{'jumps(kern)':>12}{'median us':>11}{'p90 us':>10}{'p99 us':>10}{'max us':>11}{'CV':>7}{'>4ms':>7}")
print("-"*90)
allv=[]
for i,c in enumerate(cpus):
    s=[x/(KHZ/1000) for x in seen[c]]   # cycles -> us  (2.2e6 kHz => 2200 cyc/us)
    if not s: print(f"{c:>4}{0:>12}{j1[i]-j0[i]:>12}"); continue
    allv+=s
    cv=st.stdev(s)/st.mean(s) if len(s)>1 and st.mean(s) else 0
    over=100*sum(1 for x in s if x>4000)/len(s)
    q=sorted(s)
    print(f"{c:>4}{len(s):>12}{j1[i]-j0[i]:>12}{st.median(s):>11.1f}"
          f"{q[int(.9*len(q))-1]:>10.1f}{q[int(.99*len(q))-1]:>10.1f}{max(s):>11.1f}{cv:>7.2f}{over:>6.1f}%")
if allv:
    q=sorted(allv); cv=st.stdev(allv)/st.mean(allv)
    print("-"*90)
    print(f"ALL  n={len(allv)}  median={st.median(allv):.1f}us  mean={st.mean(allv):.1f}us  "
          f"p99={q[int(.99*len(q))-1]:.1f}us  max={max(allv):.1f}us")
    print(f"coefficient of variation = {cv:.2f}   "
          f"({'HEAVY-TAILED: single-sample prediction is unreliable' if cv>1 else 'tight: single sample is a fair predictor'})")
    print(f"fraction of bursts above the 4 ms threshold: {100*sum(1 for x in allv if x>4000)/len(allv):.1f}%")
