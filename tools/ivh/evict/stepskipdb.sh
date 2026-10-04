#!/bin/bash
# LOCK SKIPPING, ISOLATED -- dbench. Arm vector identical to stepskip.sh;
# only the workload differs (dbench $CLIENTS instead of qlockbench -t $T).
#   pv     stock PV reference
#   base   adaptive_mode=2, tier1 OFF, tier2 OFF, bypass OFF, rot OFF,
#          trylock_relaxed OFF, preempt_src=2 (REQUIRED)
#   skip   base + ivh_pv_evict_enable=1     <- the ONLY difference
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict; R=/root/ivh_tools/read_ivh_counters.py
BLOCKS=${BLOCKS:-8}; DUR=${DUR:-10}; CLIENTS=${CLIENTS:-72}
OUT=$D/stepskipdb_$(date +%H%M%S).csv; META=${OUT%.csv}.meta
{ echo "kernel=$(uname -r) ncpu=$(nproc) clients=$CLIENTS blocks=$BLOCKS dur=$DUR"
  for f in $S/ivh_*; do echo "$(basename $f)=$(cat $f 2>/dev/null)"; done; } > $META
echo "blk,arm,mbps,evicts" > $OUT
ctr(){ timeout -k 5 90 python3 $R ivh_evict_marked 2>/dev/null|awk '{print $3}'; }
setarm(){
  case $1 in
    pv) /root/spin_mode 1 >/dev/null 2>&1; return 0 ;;
    *)  $D/arm.sh nt1_only >/dev/null || exit 1 ;;
  esac
  echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_head_bypass_probe
  echo 0 > $S/ivh_pv_trylock_relaxed; echo 0 > $S/ivh_pv_rot_probe
  echo 0 > $S/ivh_pv_rot_enable;      echo 0 > $S/ivh_pv_evict_node_stamp
  echo 4095 > $S/ivh_pv_beat_publish_mask; echo 220000 > $S/ivh_pv_beat_threshold
  echo 2 > $S/ivh_pv_preempt_src
  if [ "$1" = skip ]; then
      echo 4 > $S/ivh_pv_evict_hop_cap; echo 4 > $S/ivh_pv_requeue_max
      echo 1 > $S/ivh_pv_evict_enable
  else
      echo 0 > $S/ivh_pv_evict_enable
  fi
  local t1 ev by; t1=$(cat $S/ivh_pv_tier1_enable); ev=$(cat $S/ivh_pv_evict_enable); by=$(cat $S/ivh_head_bypass_enable)
  [ "$t1" = 0 ] && [ "$by" = 0 ] || { echo "ARM FAIL $1 t1=$t1 by=$by"; exit 1; }
  case $1 in base) [ "$ev" = 0 ] || exit 1 ;; skip) [ "$ev" = 1 ] || exit 1 ;; esac
}
cd /root/dbench_test 2>/dev/null || cd /root
for b in $(seq 1 $BLOCKS); do
  for a in $(printf "pv\nbase\nskip\n"|shuf); do
    setarm "$a"; B0=$(ctr)
    T=$(timeout -k 5 $((DUR+90)) dbench -t $DUR $CLIENTS 2>&1|awk '/^Throughput/{print $2}')
    A0=$(ctr)
    echo "$b,$a,${T:-0},$((A0-B0))" >> $OUT
  done
  printf "  blk%-3s done\n" "$b"
done
/root/spin_mode 1 >/dev/null 2>&1; echo 0 > $S/ivh_pv_evict_enable
echo 1 > $S/ivh_pv_evict_hop_cap; echo 4 > $S/ivh_pv_requeue_max
echo "DONE -> $OUT"
