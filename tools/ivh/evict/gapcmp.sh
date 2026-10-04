#!/bin/bash
# Does tier 1 change the KIND of node eviction catches, or only the count?
#
# Tier 1 on yields 7x more evictions (57 vs 8 per 10s) because a spinning waiter
# republishes its heartbeat every 512 iterations and so never looks stale --
# halting is what makes a waiter detectable. That raises the worry that some
# "host-preempted" detections are really "halted, waiting for a kick", which the
# gap histogram cannot distinguish: it measures eviction->requeue, and a halted
# node's kick latency lands in the same buckets as a preemption.
#
# If tier1=1 and tier1=0 differ only in sample COUNT, the populations are the
# same and the detector is fine. If the SHAPE differs, they are different in
# kind and the "75% genuine absences" claim is contaminated.
#
# Arms alternate to average out drift; 3 reps each.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict
SP=/tmp/claude-0/-root-linux-6-17/de075ceb-d4a7-4f0d-9a01-886ca9e713bf/scratchpad
R=/root/ivh_tools/read_ivh_counters.py; REPS=${REPS:-3}
ctr(){ timeout -k 5 60 python3 $R ivh_evict_marked ivh_evict_stop_halted 2>/dev/null|awk '{printf "%s ",$3}'; }
for rep in $(seq 1 $REPS); do
  for t1 in 1 0; do
    HOPCAP=4 REQMAX=4 $D/arm.sh evict >/dev/null || { echo "ARM FAIL"; exit 1; }
    echo 511 > $S/ivh_pv_beat_publish_mask; echo 220000 > $S/ivh_pv_beat_threshold
    echo 1 > $S/ivh_pv_evict_gap_hist; echo 1 > $S/ivh_pv_evict_debug
    echo $t1 > $S/ivh_pv_tier1_enable
    [ "$(cat $S/ivh_pv_tier1_enable)" = "$t1" ] || { echo "TIER1 SET FAIL"; exit 1; }
    python3 $SP/hgrab.py ivh_evict_gap_hist > $SP/g_b.json
    B=$(ctr); timeout -k 5 120 dbench -t 10 16 >/dev/null 2>&1; A=$(ctr)
    python3 $SP/hgrab.py ivh_evict_gap_hist > $SP/g_a.json
    python3 - "$rep" "$t1" "$B" "$A" "$SP" <<'PY'
import json,sys
rep,t1,B,A,SP=sys.argv[1],sys.argv[2],sys.argv[3].split(),sys.argv[4].split(),sys.argv[5]
x=json.load(open(SP+"/g_b.json")).get("ivh_evict_gap_hist",{})
y=json.load(open(SP+"/g_a.json")).get("ivh_evict_gap_hist",{})
d={int(k):y.get(k,0)-x.get(k,0) for k in set(x)|set(y)}; d={k:v for k,v in d.items() if v>0}
n=sum(d.values())
ev=int(A[0])-int(B[0]); sh=int(A[1])-int(B[1])
if n:
    keys=sorted(d); c=0; med=None
    for k in keys:
        c+=d[k]
        if med is None and c>=n/2: med=k
    short=100*sum(v for k,v in d.items() if k<=15)/n     # <30us: node was never away
    lng  =100*sum(v for k,v in d.items() if k>=19)/n     # >=238us
    vlng =100*sum(v for k,v in d.items() if k>=21)/n     # >=1ms
    print(f"  rep{rep} tier1={t1}  n={n:<5d} ev={ev:<5d} stop_halted={sh:<5d} "
          f"med=b{med} ({2.0**med/2200.0:.0f}us)  <30us={short:5.1f}%  >=238us={lng:5.1f}%  >=1ms={vlng:5.1f}%")
    open(SP+f"/gapcmp_{t1}_{rep}.json","w").write(json.dumps(d))
else:
    print(f"  rep{rep} tier1={t1}  NO SAMPLES (ev={ev} stop_halted={sh})")
PY
  done
done
echo 1 > $S/ivh_pv_tier1_enable; echo 0 > $S/ivh_pv_evict_debug; echo 0 > $S/ivh_pv_evict_gap_hist
echo "DONE"
