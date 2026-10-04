#!/usr/bin/env python3
"""Accuracy of Gate 2's time-to-preemption prediction, on a real workload.

Gate 2 predicts remaining runway as  last_active - elapsed_since_active,
where last_active is the length of the PREVIOUS active burst. So burst[i]
is the prediction and burst[i+1] is the ground truth for that prediction.
Sampling the per-rq burst sequence gives prediction/outcome pairs directly;
no kernel change, no microbenchmark, no instrumentation of the workload.

Scored two ways:
  1. regression  -- how close is the predicted duration to the real one
  2. DECISION    -- the gate only asks "is there more than T of runway?".
                    Predicted yes = burst[i] > T. True yes = burst[i+1] > T.
                    That is a binary classifier; report its confusion matrix.

usage: timeleft_accuracy.py <seconds>
"""
import sys, time, statistics as st
sys.path.insert(0,"/root/ivh_tools")
import read_vact_rq as r
KHZ=2200000; CYC_US=KHZ/1000
SECS=float(sys.argv[1]) if len(sys.argv)>1 else 30
LA, JUMPS = 3976, 3984

sym=r.load_kallsyms(); cpus=r.online_cpus()
_f0=open(r.KCORE,"rb"); ph=r.read_phdrs(_f0)
offs=[r.read_u64(_f0,ph,sym["__per_cpu_offset"]+8*c) for c in cpus]
_f0.close()
base=sym["runqueues"]

# A HELD-OPEN /proc/kcore fd returns FROZEN values -- measured 2026-10-02:
# 1 distinct value in 30 samples over 1.5s under load, versus 30/30 when the
# fd is reopened per sample. This file previously held one fd for the whole
# run, so every burst sequence it produced came from a single frozen snapshot.
# Reopening the fd while REUSING the cached phdrs is both correct and the
# cheapest option (5us/sample; reparsing the phdrs costs 13us and misses
# changes). ph is static for the life of the kernel, so caching it is safe.
def snap(fld):
    g = open(r.KCORE, "rb")
    try:
        return [r.read_u64(g, ph, base + fld + o) for o in offs]
    finally:
        g.close()

seq={c:[] for c in cpus}; last={c:None for c in cpus}
j0=snap(JUMPS); t0=time.time()
while time.time()-t0 < SECS:
    v=snap(LA)
    for i,c in enumerate(cpus):
        if v[i]!=last[c] and v[i]>0:
            seq[c].append(v[i]/CYC_US); last[c]=v[i]
j1=snap(JUMPS)

# only vCPUs that actually saw bursts (kernel jump counter advanced)
live=[c for i,c in enumerate(cpus) if (j1[i]-j0[i])>5 and len(seq[c])>20]
# Guard against the frozen-fd failure mode coming back: if almost nothing
# changed, we sampled a stuck snapshot, not a quiet machine.
_tot=sum(len(v) for v in seq.values())
if _tot < 3*len(cpus):
    sys.exit(f"ABORT: only {_tot} burst transitions seen across {len(cpus)} cpus -- "
             "this is the frozen-/proc/kcore signature, not a real sample. "
             "Check that snap() reopens the fd.")
pairs=[]
for c in live: pairs += list(zip(seq[c][:-1], seq[c][1:]))
if not pairs: sys.exit("no burst pairs captured")

pred=[p for p,_ in pairs]; act=[a for _,a in pairs]
print(f"real-workload burst pairs: {len(pairs)} on {len(live)} contended vCPUs "
      f"(cpus {live[0]}-{live[-1]})\n")
print("1. REGRESSION  -- does last_active predict the NEXT burst length?")
mp,ma=st.mean(pred),st.mean(act)
cov=sum((p-mp)*(a-ma) for p,a in pairs)/len(pairs)
cor=cov/(st.pstdev(pred)*st.pstdev(act)) if st.pstdev(pred) and st.pstdev(act) else 0
rel=sorted(abs(p-a)/a for p,a in pairs if a>0)
print(f"   correlation(pred, actual)      {cor:+.3f}   (1.0 = perfect, 0 = useless)")
print(f"   median |error| / actual        {100*rel[len(rel)//2]:.0f}%")
print(f"   90th pct |error| / actual      {100*rel[int(.9*len(rel))]:.0f}%")
print(f"   median predicted {st.median(pred):8.1f} us   median actual {st.median(act):8.1f} us")
print("\n2. DECISION  -- the gate only asks: is runway > T?")
print(f"   {'T':>6}{'predict yes':>12}{'true yes':>10}{'accuracy':>10}{'precision':>11}{'recall':>9}   verdict")
print("   " + "-"*68)
for T in (1000,2000,4000,8000,16000):
    tp=sum(1 for p,a in pairs if p>T and a>T); fp=sum(1 for p,a in pairs if p>T and a<=T)
    fn=sum(1 for p,a in pairs if p<=T and a>T); tn=sum(1 for p,a in pairs if p<=T and a<=T)
    acc=100*(tp+tn)/len(pairs)
    prec=100*tp/(tp+fp) if tp+fp else float('nan')
    rec=100*tp/(tp+fn) if tp+fn else float('nan')
    base=100*max(tp+fn, tn+fp)/len(pairs)   # always-guess-majority baseline
    verdict="no better than guessing" if acc<=base+1 else f"beats {base:.0f}% baseline"
    print(f"   {T//1000:>4}ms{tp+fp:>12}{tp+fn:>10}{acc:>9.1f}%{prec:>10.1f}%{rec:>8.1f}%   {verdict}")
