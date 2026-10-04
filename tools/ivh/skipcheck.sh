#!/bin/bash
# G-LOCK-48: "a waiter comes back from being preempted, sees its own stamp is
# stale, checks its state -- is it VCPU_SKIPPED?"
#
#   ivh_skipcheck[was_SKIPPED][gap_bucket]
#   recall(bucket) = [1][b] / ([0][b] + [1][b])
#
# Gap is from the node's OWN stamp, written by this cpu. Self-observation.
# Buckets at/above the evict threshold are the ones that matter: below it the
# evictor is not supposed to have marked anybody, so a low rate there is
# CORRECT, not a miss.
set -u
S=/proc/sys/kernel
set_(){ echo "$2" > $S/$1 2>/dev/null; [ "$(cat $S/$1 2>/dev/null)" = "$2" ] || echo "  !! $1 rejected ($2)"; }
set_ ivh_tks_sampler_ns 0
set_ ivh_adaptive_mode 2;      set_ ivh_pv_tier1_enable 1
set_ ivh_pv_tier2_enable 0     # tier 2 is inert; keep it out of the way
set_ ivh_pv_preempt_src 2      # REQUIRED before evict_node_stamp (G-47 interlock)
set_ ivh_pv_evict_enable 1
set_ ivh_pv_evict_node_stamp 1
set_ ivh_pv_evict_hop_cap 2
set_ ivh_pv_evict_lookahead 1
set_ ivh_pv_requeue_nosteal 1
set_ ivh_pv_evict_threshold "${ETHR:-1100000}"
set_ ivh_skipcheck_hist 1
echo "evict threshold = $(( $(cat $S/ivh_pv_evict_threshold) / 2200 ))us"
R="python3 /root/ivh_tools/read_ivh_counters.py"
g(){ $R ivh_skipcheck 2>/dev/null | grep -oE 'nonzero_buckets=\[.*\]' | tr '\n' '|'; }
e(){ $R ivh_evict_marked ivh_evict_requeued 2>/dev/null | awk '/^ivh_/{print $NF}' | tr '\n' ' '; }
A=$(g); EA=($(e))
timeout 150 hackbench -T ${WL:--g1} -f8 -l250000 >/dev/null 2>&1
B=$(g); EB=($(e))
set_ ivh_skipcheck_hist 0; set_ ivh_pv_evict_enable 0
echo "evictions this run: marked=$(( EB[0]-EA[0] )) requeued=$(( EB[1]-EA[1] ))"
python3 - "$A" "$B" "$(cat $S/ivh_pv_evict_threshold)" <<'PY'
import re,sys
MHZ=2200.0
def rows(s):
    return [{int(x):int(y) for x,y in re.findall(r'\((\d+),\s*(\d+)\)',p)} for p in s.split('|') if p.strip()]
a,b=rows(sys.argv[1]),rows(sys.argv[2]); thr=int(sys.argv[3])/MHZ
while len(a)<2: a.append({})
while len(b)<2: b.append({})
d=[{k:b[i].get(k,0)-a[i].get(k,0) for k in set(a[i])|set(b[i])} for i in range(2)]
print(f"\n{'bucket':>7} {'gap >=':>10} {'returns':>9} {'SKIPPED':>9} {'marked%':>9}")
tn=tm=0
for k in sorted(set(d[0])|set(d[1])):
    miss=max(d[0].get(k,0),0); hit=max(d[1].get(k,0),0); n=miss+hit
    if n<=0: continue
    lo=(2.0**k)/MHZ
    mark="  <-- at/above evict threshold" if lo>=thr else ""
    if lo>=thr: tn+=n; tm+=hit
    print(f"  b{k:<4} {lo:10.1f} {n:9d} {hit:9d} {100.0*hit/n:8.1f}%{mark}")
print(f"\n  ABOVE THRESHOLD: {tm}/{tn} = " + (f"{100.0*tm/tn:.1f}%" if tn else "no samples"))
print("  (below threshold the evictor is not meant to mark anyone, so a low")
print("   rate there is correct behaviour, not a miss.)")
PY
echo SKIPCHECK-DONE
