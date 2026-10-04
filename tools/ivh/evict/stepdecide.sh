#!/bin/bash
# THE DECISIVE ARM. No dataset has ever contained "tier1 on + heartbeat, NO
# mechanism" measured against stock PV, so the +3.19% headline is the whole IVH
# stack vs PV and cannot be attributed to the bypass.
#   pv        stock PV (spin_mode 1)
#   ctrl      adaptive_mode=2, tier1 ON, tier2 off, src=2, NO mechanism   <- missing arm
#   bypass    ctrl + trylock_relaxed + head_bypass_enable
# ctrl-vs-pv  = what the IVH stack costs/earns on its own
# bypass-vs-ctrl = the mechanism's ACTUAL contribution
# Complete knob vector per arm + verify: nothing resets the bypass knobs.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict
BLOCKS=${BLOCKS:-15}; DUR=${DUR:-10}; T=${T:-72}
OUT=$D/stepdecide_$(date +%H%M%S).csv; META=${OUT%.csv}.meta
{ echo "kernel=$(uname -r) ncpu=$(nproc) threads=$T blocks=$BLOCKS dur=$DUR"
  for f in $S/ivh_*; do echo "$(basename $f)=$(cat $f 2>/dev/null)"; done; } > $META
echo "blk,arm,ops,iters,hit,p50,p99,p999,p9999,max,over1ms,fired" > $OUT
ctr(){ timeout -k 5 90 python3 /root/ivh_tools/read_ivh_counters.py ivh_head_bypass_fired 2>/dev/null|awk '{print $3}'; }
setarm(){
  case $1 in
    pv) /root/spin_mode 1 >/dev/null 2>&1 ;;
    *)  $D/arm.sh control >/dev/null || exit 1 ;;
  esac
  echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_pv_trylock_relaxed
  echo 0 > $S/ivh_pv_rot_probe; echo 0 > $S/ivh_head_bypass_probe
  echo 4095 > $S/ivh_pv_beat_publish_mask; echo 220000 > $S/ivh_pv_beat_threshold
  [ "$1" = bypass ] && { echo 1 > $S/ivh_pv_trylock_relaxed
                         echo 1 > $S/ivh_head_bypass_probe
                         echo 1 > $S/ivh_head_bypass_enable; }
  local by; by=$(cat $S/ivh_head_bypass_enable)
  case $1 in
    pv|ctrl) [ "$by" = 0 ] || { echo "ARM FAIL $1"; exit 1; } ;;
    bypass)  [ "$by" = 1 ] || { echo "ARM FAIL $1"; exit 1; } ;;
  esac
}
for b in $(seq 1 $BLOCKS); do
  for a in $(printf "pv\nctrl\nbypass\n"|shuf); do
    setarm "$a"; B0=$(ctr)
    Q=$(timeout -k 5 $((DUR+50)) /root/linux-6.17/qlockbench -t $T -d $DUR -Q 2>&1|tail -1)
    A0=$(ctr)
    echo "$b,$a,$Q,$((A0-B0))" >> $OUT
  done
  printf "  blk%-3s done\n" "$b"
done
/root/spin_mode 1 >/dev/null 2>&1
echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_head_bypass_probe; echo 0 > $S/ivh_pv_trylock_relaxed
echo "DONE -> $OUT"
