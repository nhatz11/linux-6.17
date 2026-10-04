#!/bin/bash
# STEP 1: eviction on dbench -- the ONLY workload measured with real queueing
# (26.8% stealer availability vs 99.7% on qlockbench). Everywhere else stealers
# cover 87-99.7% of contended acquisitions, so eviction has nothing to do by
# construction and every prior null was structural, not evidence.
#
# control vs evict differ in ivh_pv_evict_enable ONLY. Detector settings
# (mask 511 / thr 220000) are identical in both -- the two-variable delta in
# runreal.sh is what produced the bogus -7.75%.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict
SP=/tmp/claude-0/-root-linux-6-17/de075ceb-d4a7-4f0d-9a01-886ca9e713bf/scratchpad
R=/root/ivh_tools/read_ivh_counters.py
BLOCKS=${BLOCKS:-20}
OUT=$D/step1_$(date +%H%M%S).csv; META=${OUT%.csv}.meta
{ echo "kernel=$(uname -r)"; echo "blocks=$BLOCKS"; echo "post-server-restart regime"
  for f in $S/ivh_*; do echo "$(basename $f)=$(cat $f 2>/dev/null)"; done; } > $META
echo "blk,arm,mbps,evicts,gap_n,gap_long,steals,queued" > $OUT

ctr(){ timeout -k 5 60 python3 $R ivh_evict_marked ivh_rot_steals ivh_node_spin_attempts 2>/dev/null|awk '{printf "%s ",$3}'; }
setarm(){
  case $1 in
    pv)      /root/spin_mode 1 >/dev/null 2>&1 ;;
    control) $D/arm.sh control >/dev/null || return 1 ;;
    evict)   HOPCAP=1 REQMAX=1 $D/arm.sh evict >/dev/null || return 1 ;;
  esac
  echo 1 > $S/ivh_pv_rot_probe
  [ "$1" = pv ] && return 0
  echo 511 > $S/ivh_pv_beat_publish_mask; echo 220000 > $S/ivh_pv_beat_threshold
  echo 1 > $S/ivh_pv_evict_gap_hist; echo 0 > $S/ivh_pv_evict_node_stamp
  echo 0 > $S/ivh_pv_evict_quiet; echo 0 > $S/ivh_pv_evict_age_hist
  [ "$(cat $S/ivh_pv_beat_threshold)" = 220000 ] && [ "$(cat $S/ivh_pv_tier1_enable)" = 1 ]
}
ARMS=(pv control evict)
for b in $(seq 1 $BLOCKS); do
  for a in $(printf "%s\n" "${ARMS[@]}" | shuf); do
    setarm $a || { echo "ARM FAIL $a"; exit 1; }
    python3 $SP/hgrab.py ivh_evict_gap_hist > $SP/s1_b.json
    B=$(ctr)
    TP=$(timeout -k 5 120 dbench -t 10 16 2>&1 | awk '/^Throughput/{print $2}')
    A=$(ctr)
    python3 $SP/hgrab.py ivh_evict_gap_hist > $SP/s1_a.json
    python3 - "$b" "$a" "$TP" "$B" "$A" "$OUT" "$SP" <<'PY'
import json,sys
blk,arm,tp,B,A,OUT,SP=sys.argv[1],sys.argv[2],sys.argv[3],sys.argv[4].split(),sys.argv[5].split(),sys.argv[6],sys.argv[7]
x=json.load(open(SP+"/s1_b.json")).get("ivh_evict_gap_hist",{})
y=json.load(open(SP+"/s1_a.json")).get("ivh_evict_gap_hist",{})
d={int(k):y.get(k,0)-x.get(k,0) for k in set(x)|set(y)}; d={k:v for k,v in d.items() if v>0}
n=sum(d.values()); lng=sum(v for k,v in d.items() if k>=19)
ev=int(A[0])-int(B[0]); st=int(A[1])-int(B[1]); q=int(A[2])-int(B[2])
if not tp: tp="0"
open(OUT,'a').write(f"{blk},{arm},{tp},{ev},{n},{lng},{st},{q}\n")
print(f"  blk{blk:<3s} {arm:<8s} {float(tp):9.2f} MB/s  ev={ev:<5d} gap_long={100*lng/n if n else 0:5.1f}%  "
      f"steal_avail={100*st/(st+q) if st+q else 0:5.1f}%")
PY
  done
done
echo 220000 > $S/ivh_pv_beat_threshold; echo 4095 > $S/ivh_pv_beat_publish_mask
echo 0 > $S/ivh_pv_evict_gap_hist; echo 0 > $S/ivh_pv_rot_probe
echo "DONE -> $OUT"
