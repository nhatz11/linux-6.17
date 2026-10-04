#!/bin/bash
# Lock skipping on dbench, third attempt, on G-LOCK-37 with the gap histogram.
# Hypothesis (user's): dbench has few stealers at handoff, so a preempted
# successor really does idle the lock -- unlike qlockbench where a stealer
# always covers it. Prior result: null at n=20 and n=14, but the binding
# constraint was event RATE (9.5 and 3.9 fires/run), not the steal valve.
# Records eviction rate + absence distribution so a third null is explainable
# rather than just repeated.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict; R=/root/ivh_tools/read_ivh_counters.py
SP=/tmp/claude-0/-root-linux-6-17/de075ceb-d4a7-4f0d-9a01-886ca9e713bf/scratchpad
BLOCKS=${BLOCKS:-12}; DUR=${DUR:-10}
OUT=$D/step4_$(date +%H%M%S).csv
echo "blk,arm,mbps,marked,requeued,gap_n,gap_long,steals,node_att" > $OUT
C="ivh_evict_marked ivh_evict_requeued ivh_rot_steals ivh_node_spin_attempts"
ctr(){ timeout -k 5 60 python3 $R $C 2>/dev/null|awk '{printf "%s ",$3}'; }
setarm(){ case $1 in
    pv)      /root/spin_mode 1 >/dev/null 2>&1 ;;
    control) $D/arm.sh control >/dev/null || exit 1 ;;
    evict)   HOPCAP=4 REQMAX=4 $D/arm.sh evict >/dev/null || exit 1 ;;
  esac
  echo 1 > $S/ivh_pv_rot_probe
  [ "$1" = pv ] && return 0
  echo 511 > $S/ivh_pv_beat_publish_mask; echo 220000 > $S/ivh_pv_beat_threshold
  echo 1 > $S/ivh_pv_evict_gap_hist; echo 0 > $S/ivh_head_bypass_probe; }
for b in $(seq 1 $BLOCKS); do
  for a in $(printf "pv\ncontrol\nevict\n"|shuf); do
    setarm $a
    python3 $SP/hgrab.py ivh_evict_gap_hist > $SP/s4b.json 2>/dev/null || echo '{}' > $SP/s4b.json
    B=$(ctr); T=$(timeout -k 5 120 dbench -t $DUR 16 2>&1|awk '/^Throughput/{print $2}'); A=$(ctr)
    python3 $SP/hgrab.py ivh_evict_gap_hist > $SP/s4a.json 2>/dev/null || echo '{}' > $SP/s4a.json
    python3 - "$b" "$a" "${T:-0}" "$B" "$A" "$OUT" "$SP" <<'PY'
import json,sys
blk,arm,tp,B,A,OUT,SP=sys.argv[1],sys.argv[2],sys.argv[3],sys.argv[4].split(),sys.argv[5].split(),sys.argv[6],sys.argv[7]
x=json.load(open(SP+"/s4b.json")).get("ivh_evict_gap_hist",{}); y=json.load(open(SP+"/s4a.json")).get("ivh_evict_gap_hist",{})
d={int(k):y.get(k,0)-x.get(k,0) for k in set(x)|set(y)}; d={k:v for k,v in d.items() if v>0}
n=sum(d.values()); lng=sum(v for k,v in d.items() if k>=19)
dd=[int(A[i])-int(B[i]) for i in range(4)]
open(OUT,'a').write(f"{blk},{arm},{tp},{dd[0]},{dd[1]},{n},{lng},{dd[2]},{dd[3]}\n")
sa=100*dd[2]/(dd[2]+dd[3]) if dd[2]+dd[3] else 0
print(f"  blk{blk:<3s} {arm:<8s} {float(tp):8.1f} MB/s  evicts={dd[0]:<5d} gap_long={lng:<4d} steal_avail={sa:5.1f}%")
PY
  done
done
echo 0 > $S/ivh_pv_evict_gap_hist; echo 0 > $S/ivh_pv_rot_probe; /root/spin_mode 1 >/dev/null 2>&1
echo "DONE -> $OUT"
