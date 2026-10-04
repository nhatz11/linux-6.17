#!/bin/bash
# Self-validating accuracy, no host needed.
#  TEST A (holder): at unlock, the holder asks its OWN rq whether its TSC
#    jumped during the critical section -> ivh_cs_v_flagged[cause][verdict].
#    "flagged" = a detector fired on this hold. verdict 0=no preempt,
#    1=preempted, 2=ambiguous, 3=unknowable.
#  TEST B (waiter): a waiter that finds itself VCPU_SKIPPED checks, on its
#    own cpu, whether its TSC jumped since the eviction stamp -> ivh_evict_v.
# Self-observation is reliable (own TSC jump); remote observation is not.
# NOTE: sampler is ARMED here for detection resolution -- costs ~48% on
# throughput, so NO throughput number from this run is valid.
S=/proc/sys/kernel
set_(){ echo "$2" > $S/$1 2>/dev/null; [ "$(cat $S/$1 2>/dev/null)" = "$2" ] || echo "  !! $1 rejected ($2)"; }
echo 0 > /proc/sys/kernel/ivh_universal_eligible   # migration off: it removes the preemptions we must observe
set_ ivh_cs_verdict 1
set_ ivh_cs_track_enabled 1
set_ ivh_cs_criterion 0
set_ ivh_cs_owner_enable 1
set_ ivh_cs_head_probe 1
set_ ivh_pv_rot_enable 0
set_ ivh_cs_owner_clear 1          # else 100% of stamps read as "overwrote"
set_ ivh_tks_sampler_ns 0          # TEST A ran on the tick clock in the validated recipe
set_ ivh_vact_jump_ns 1500000
set_ ivh_vact_min_preempt_ns 2000
set_ ivh_pv_evict_enable 1         # needed for TEST B (VCPU_SKIPPED)
set_ ivh_pv_evict_hop_cap 2
set_ ivh_pv_evict_node_stamp 1
set_ ivh_pv_evict_gap_hist 1
set_ ivh_pv_beat_threshold 11000000
set_ ivh_pv_tier1_halt_min "${1:-0}"
echo "halt_min=$(cat $S/ivh_pv_tier1_halt_min)  threshold=$(cat $S/ivh_pv_beat_threshold)  sampler=$(cat $S/ivh_tks_sampler_ns)"
R="python3 /root/ivh_tools/read_ivh_counters.py"
N="ivh_cs_v_flagged ivh_cs_v_unflagged ivh_evict_v ivh_evict_requeued ivh_evict_marked"
$R $N > /tmp/sa_a.txt 2>&1
for i in 1 2 3 4 5; do timeout 130 hackbench -T -g1 -f8 -l400000 >/dev/null 2>&1; done; true >/dev/null 2>&1
$R $N > /tmp/sa_b.txt 2>&1
python3 - <<'PY'
import re
V=["no-preempt","PREEMPTED","ambiguous","unknowable"]
def p(f):
    s={};arr={}
    for ln in open(f):
        m=re.match(r'^(ivh_\w+)\s+\[(\S+)\s*\]\s+sum=(\d+)\s+nonzero_buckets=\[(.*)\]',ln)
        if m:
            arr[(m.group(1),m.group(2))]={int(a):int(b) for a,b in re.findall(r'\((\d+),\s*(\d+)\)',m.group(4))}
            continue
        m=re.match(r'^(ivh_\w+)\s+\[(\S+)\s*\]\s+=\s+(\d+)',ln)
        if m: s[(m.group(1),m.group(2))]=int(m.group(3)); continue
        m=re.match(r'^(ivh_\w+)\s+=\s+(\d+)',ln)
        if m: s[(m.group(1),'')]=int(m.group(2))
    return s,arr
sa,aa=p('/tmp/sa_a.txt'); sb,ab=p('/tmp/sa_b.txt')
print("\n=== TEST A: holder self-report at unlock (ivh_cs_v_flagged) ===")
print("  a 'flagged' hold is one a detector fired on; verdict is the holder's OWN TSC")
tot=[0]*4
for cause in ("NONE","TIER1","TIER2","EXHAUST","TIER1_AGREED","TIER1_DISAGREED"):
    A=aa.get(('ivh_cs_v_flagged',cause),{}); B=ab.get(('ivh_cs_v_flagged',cause),{})
    row=[B.get(i,0)-A.get(i,0) for i in range(4)]
    if sum(row)<=0: continue
    for i in range(4): tot[i]+=row[i]
    print(f"  {cause:16s} " + "  ".join(f"{V[i]}={row[i]}" for i in range(4)))
n=sum(tot); judg=tot[0]+tot[1]+tot[2]
if n:
    print(f"  TOTAL flagged={n}  judgeable={judg}  " +
          (f"PRECISION=(preempted+ambig)/judgeable = {100.0*(tot[1]+tot[2])/judg:.1f}%" if judg else "no judgeable"))
print("\n=== TEST B: evicted waiter self-report on requeue (ivh_evict_v) ===")
ev=sb.get(('ivh_evict_requeued',''),0)-sa.get(('ivh_evict_requeued',''),0)
row=[sb.get(('ivh_evict_v',k),0)-sa.get(('ivh_evict_v',k),0) for k in ("NONE","TIER1","TIER1_AGREED","TIER1_DISAGREED")]
print(f"  requeued={ev}   " + "  ".join(f"{V[i]}={row[i]}" for i in range(4)))
j=row[0]+row[1]+row[2]
if j: print(f"  judgeable={j}  PRECISION=(preempted+ambig)/judgeable = {100.0*(row[1]+row[2])/j:.1f}%")
PY
echo 0 > $S/ivh_tks_sampler_ns; echo 0 > $S/ivh_cs_verdict
echo SELFAUDIT-DONE
