#!/bin/bash
# DECOMPOSE the lock-skipping regression into (a) walk entry cost and
# (b) eviction cost. Production config: evict_debug=0, so pv_evict_can_skip()
# gates the walk exactly as it would in the field.
#   base  evict_enable=0            -- walk never called
#   gate  evict_enable=1, cap=0     -- walk runs on every qualifying handoff,
#                                      refuses at the requeues>=cap check
#                                      (:2556 "cap == 0 disables eviction")
#                                      => ZERO evictions, full walk cost
#   skip  evict_enable=1, cap=4     -- the real thing
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict; R=/root/ivh_tools/read_ivh_counters.py
BLOCKS=${BLOCKS:-10}; DUR=${DUR:-10}; T=${T:-72}
OUT=$D/stepgate_$(date +%H%M%S).csv; META=${OUT%.csv}.meta
{ echo "kernel=$(uname -r) ncpu=$(nproc) threads=$T blocks=$BLOCKS dur=$DUR"
  for f in $S/ivh_*; do echo "$(basename $f)=$(cat $f 2>/dev/null)"; done; } > $META
echo "blk,arm,ops,iters,hit,p50,p99,p999,p9999,max,over1ms,marked" > $OUT
ctr(){ timeout -k 5 90 python3 $R ivh_evict_marked 2>/dev/null|awk '{print $3}'; }
setarm(){
  $D/arm.sh nt1_only >/dev/null || exit 1
  echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_head_bypass_probe
  echo 0 > $S/ivh_pv_trylock_relaxed; echo 0 > $S/ivh_pv_rot_probe
  echo 0 > $S/ivh_pv_rot_enable;      echo 0 > $S/ivh_pv_evict_node_stamp
  echo 0 > $S/ivh_pv_evict_debug
  echo 4095 > $S/ivh_pv_beat_publish_mask; echo 220000 > $S/ivh_pv_beat_threshold
  echo 2 > $S/ivh_pv_preempt_src; echo 4 > $S/ivh_pv_evict_hop_cap
  case $1 in
    base) echo 4 > $S/ivh_pv_requeue_max; echo 0 > $S/ivh_pv_evict_enable ;;
    gate) echo 0 > $S/ivh_pv_requeue_max; echo 1 > $S/ivh_pv_evict_enable ;;
    skip) echo 4 > $S/ivh_pv_requeue_max; echo 1 > $S/ivh_pv_evict_enable ;;
  esac
  local t1 by; t1=$(cat $S/ivh_pv_tier1_enable); by=$(cat $S/ivh_head_bypass_enable)
  [ "$t1" = 0 ] && [ "$by" = 0 ] || { echo "ARM FAIL $1"; exit 1; }
}
for b in $(seq 1 $BLOCKS); do
  for a in $(printf "base\ngate\nskip\n"|shuf); do
    setarm "$a"; B0=$(ctr)
    Q=$(timeout -k 5 $((DUR+50)) /root/linux-6.17/qlockbench -t $T -d $DUR -Q 2>&1|tail -1)
    A0=$(ctr)
    echo "$b,$a,$Q,$((A0-B0))" >> $OUT
  done
  printf "  blk%-3s done\n" "$b"
done
echo 1 > $S/ivh_pv_evict_hop_cap; echo 4 > $S/ivh_pv_requeue_max
/root/spin_mode 1 >/dev/null 2>&1; echo 0 > $S/ivh_pv_evict_enable
echo "DONE -> $OUT"
