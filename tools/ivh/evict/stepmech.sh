#!/bin/bash
# DO ANY MECHANISM BEAT STOCK PV? 36 vCPUs. tier1 OFF, tier2 OFF in every
# non-pv arm (user's constraint). WL=qlock|dbench.
#   pv    stock PV (spin_mode 1)
#   base  adaptive_mode=2, tier1/tier2 OFF, src=2, no mechanism
#   skip  base + evict_enable=1
#   byp   base + head_bypass_enable=1 + trylock_relaxed=1
#   both  base + skip + byp
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict; R=/root/ivh_tools/read_ivh_counters.py
BLOCKS=${BLOCKS:-10}; DUR=${DUR:-10}; T=${T:-36}; WL=${WL:-qlock}; HOP=${HOP:-4}
ARMS=${ARMS:-"pv base skip obs byp both"}
OUT=$D/stepmech_${WL}_$(date +%H%M%S).csv; META=${OUT%.csv}.meta
{ echo "kernel=$(uname -r) ncpu=$(nproc) T=$T wl=$WL blocks=$BLOCKS dur=$DUR hop=$HOP arms=$ARMS"
  for f in $S/ivh_*; do echo "$(basename $f)=$(cat $f 2>/dev/null)"; done; } > $META
echo "blk,arm,metric,ev,fired" > $OUT
ctr(){ timeout -k 5 90 python3 $R ivh_evict_marked ivh_head_bypass_fired 2>/dev/null|awk '{printf "%s ",$3}'; }
setarm(){
  if [ "$1" = pv ]; then /root/spin_mode 1 >/dev/null 2>&1
  else $D/arm.sh nt1_only >/dev/null || exit 1; fi
  echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_head_bypass_probe
  echo 0 > $S/ivh_pv_trylock_relaxed; echo 0 > $S/ivh_pv_rot_probe
  echo 0 > $S/ivh_pv_rot_enable;      echo 0 > $S/ivh_pv_evict_node_stamp
  echo 0 > $S/ivh_pv_evict_debug;     echo 0 > $S/ivh_pv_evict_enable
  [ "$1" = pv ] && return 0
  echo 0 > $S/ivh_pv_tier2_enable
  echo 4095 > $S/ivh_pv_beat_publish_mask; echo 220000 > $S/ivh_pv_beat_threshold
  echo 2 > $S/ivh_pv_preempt_src; echo $HOP > $S/ivh_pv_evict_hop_cap; echo 4 > $S/ivh_pv_requeue_max
  case $1 in
    skip) echo 1 > $S/ivh_pv_evict_enable ;;
    obs)  echo 1 > $S/ivh_head_bypass_probe ;;
    byp)  echo 1 > $S/ivh_head_bypass_probe; echo 1 > $S/ivh_pv_trylock_relaxed
          echo 1 > $S/ivh_head_bypass_enable ;;
    both) echo 1 > $S/ivh_pv_evict_enable
          echo 1 > $S/ivh_head_bypass_probe; echo 1 > $S/ivh_pv_trylock_relaxed
          echo 1 > $S/ivh_head_bypass_enable ;;
  esac
  local t1 t2 pr en; t1=$(cat $S/ivh_pv_tier1_enable); t2=$(cat $S/ivh_pv_tier2_enable)
  pr=$(cat $S/ivh_head_bypass_probe); en=$(cat $S/ivh_head_bypass_enable)
  [ "$t1" = 0 ] && [ "$t2" = 0 ] || { echo "ARM FAIL $1 t1=$t1 t2=$t2"; exit 1; }
  case $1 in
    obs)  [ "$pr" = 1 ] && [ "$en" = 0 ] || { echo "ARM FAIL obs pr=$pr en=$en"; exit 1; } ;;
    byp|both) [ "$pr" = 1 ] && [ "$en" = 1 ] || { echo "ARM FAIL $1 pr=$pr en=$en"; exit 1; } ;;
    base|skip) [ "$pr" = 0 ] && [ "$en" = 0 ] || { echo "ARM FAIL $1 pr=$pr en=$en"; exit 1; } ;;
  esac
}
run_wl(){
  if [ "$WL" = dbench ]; then
    cd /root/dbench_test 2>/dev/null || cd /root
    timeout -k 5 $((DUR+90)) dbench -t $DUR $T 2>&1|awk '/^Throughput/{print $2}'
  else
    timeout -k 5 $((DUR+50)) /root/linux-6.17/qlockbench -t $T -d $DUR -Q 2>&1|tail -1|cut -d, -f2
  fi
}
for b in $(seq 1 $BLOCKS); do
  for a in $(echo $ARMS|tr ' ' '\n'|shuf); do
    setarm "$a"; read -r e0 f0 <<< "$(ctr)"
    M=$(run_wl); read -r e1 f1 <<< "$(ctr)"
    echo "$b,$a,${M:-0},$((e1-e0)),$((f1-f0))" >> $OUT
  done
  printf "  blk%-3s done\n" "$b"
done
/root/spin_mode 1 >/dev/null 2>&1
echo 0 > $S/ivh_pv_evict_enable; echo 0 > $S/ivh_head_bypass_enable
echo 0 > $S/ivh_pv_trylock_relaxed; echo 0 > $S/ivh_head_bypass_probe
echo 1 > $S/ivh_pv_evict_hop_cap
echo "DONE -> $OUT"
