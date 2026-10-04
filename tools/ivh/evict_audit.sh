#!/bin/bash
# Are the SKIPPED waiters actually preempted?
# Self-check, the design that worked for the holder: when an evicted waiter
# requeues, it asks its OWN rq whether its TSC jumped since the eviction
# stamp. Self-observation is reliable; remote observation is not.
#   ivh_evict_v[verdict] 0=no jump 1=PREEMPTED 2=ambiguous 3=unknowable
# For a waiter, verdict 1 is near-unreachable by construction: the evictor
# fires BECAUSE the waiter already looks gone, so a genuine preemption
# started before the eviction stamp -> straddles -> verdict 2. So the
# correct-target signature is (1 + 2), not 1 alone.
set -u
S=/proc/sys/kernel
set_(){ echo "$2" > $S/$1 2>/dev/null; }
set_ ivh_adaptive_mode 2;      set_ ivh_pv_tier1_enable 1
set_ ivh_pv_tier2_enable 0     # isolate: threshold drives eviction only
set_ ivh_pv_evict_enable 1;    set_ ivh_pv_evict_node_stamp 1
set_ ivh_pv_evict_gap_hist 1;  set_ ivh_pv_evict_hop_cap 2
set_ ivh_pv_evict_lookahead 1; set_ ivh_pv_requeue_nosteal 1
set_ ivh_cs_verdict 1
# oracle resolution: lag = jump_ns + sampler = 500us, so >=500us spans are
# judgeable. Sampler costs ~48% throughput -- accuracy run only.
set_ ivh_tks_sampler_ns 200000; set_ ivh_vact_jump_ns 300000
set_ ivh_vact_min_preempt_ns 2000
R="python3 /root/ivh_tools/read_ivh_counters.py"
g(){ $R ivh_evict_requeued ivh_evict_v 2>/dev/null | awk '/^ivh_/{print $NF}' | tr '\n' ' '; }
printf "%8s %10s %8s %10s %10s %11s %9s\n" "thr" "requeued" "no-jump" "PREEMPTED" "ambiguous" "unknowable" "TARGET%"
for t in 220000 1100000; do
  set_ ivh_pv_beat_threshold $t; sleep 1
  A=($(g)); timeout 130 hackbench -T -g1 -f8 -l250000 >/dev/null 2>&1; B=($(g))
  python3 - "$t" "${A[*]}" "${B[*]}" <<'PY'
import sys
t=int(sys.argv[1]); a=[int(x) for x in sys.argv[2].split()]; b=[int(x) for x in sys.argv[3].split()]
d=[b[i]-a[i] for i in range(min(len(a),len(b)))]
req=d[0]; v=d[1:5]+[0,0,0,0]
judg=v[0]+v[1]+v[2]; tgt=v[1]+v[2]
pct=f"{100.0*tgt/judg:.1f}%" if judg else "n/a"
print(f"{t//2200:6d}us {req:10d} {v[0]:8d} {v[1]:10d} {v[2]:10d} {v[3]:11d} {pct:>9}")
PY
done
set_ ivh_tks_sampler_ns 0; set_ ivh_cs_verdict 0; set_ ivh_pv_evict_enable 0
set_ ivh_pv_tier2_enable 1; set_ ivh_pv_beat_threshold 11000000
echo EVICTAUDIT-DONE
