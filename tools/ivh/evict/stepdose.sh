#!/bin/bash
# DOSE-RESPONSE on the heartbeat publish. Is the ~3-4% win delay-mediated?
#   pv      stock PV (reference)
#   src0    preempt_src=0: NO publishing anywhere        (zero dose)
#   m65535  per-QUEUE-ENTRY publish only; loop counts down from 32768 so
#           (loop & 65535)==0 NEVER fires -> 0 in-spin publishes
#   m4095 / m511 / m255   + 8 / 64 / 128 in-spin publishes per attempt
# src0 -> m65535  = the pv_init_node() per-entry publish alone
# m65535 -> m255  = in-spin dose, 0 -> 128 (mask must be >=0xff, 2^n-1)
# Monotone => delay-mediated. Flat => the delay is NOT the cause.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict
BLOCKS=${BLOCKS:-10}; DUR=${DUR:-10}; T=${T:-72}
OUT=$D/stepdose_$(date +%H%M%S).csv; META=${OUT%.csv}.meta
{ echo "kernel=$(uname -r) ncpu=$(nproc) threads=$T blocks=$BLOCKS dur=$DUR"
  for f in $S/ivh_*; do echo "$(basename $f)=$(cat $f 2>/dev/null)"; done; } > $META
echo "blk,arm,ops,iters,hit,p50,p99,p999,p9999,max,over1ms" > $OUT
setarm(){
  case $1 in
    pv) /root/spin_mode 1 >/dev/null 2>&1; return 0 ;;
    *)  $D/arm.sh control >/dev/null || exit 1 ;;
  esac
  echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_head_bypass_probe
  echo 0 > $S/ivh_pv_trylock_relaxed; echo 0 > $S/ivh_pv_rot_probe
  echo 220000 > $S/ivh_pv_beat_threshold
  case $1 in
    src0)  echo 0 > $S/ivh_pv_preempt_src ;;
    *)     echo 2 > $S/ivh_pv_preempt_src; echo "${1#m}" > $S/ivh_pv_beat_publish_mask
           [ "$(cat $S/ivh_pv_beat_publish_mask)" = "${1#m}" ] || { echo "MASK FAIL $1"; exit 1; } ;;
  esac
}
for b in $(seq 1 $BLOCKS); do
  for a in $(printf "pv\nsrc0\nm65535\nm4095\nm511\nm255\n"|shuf); do
    setarm "$a"
    Q=$(timeout -k 5 $((DUR+50)) /root/linux-6.17/qlockbench -t $T -d $DUR -Q 2>&1|tail -1)
    echo "$b,$a,$Q" >> $OUT
  done
  printf "  blk%-3s done\n" "$b"
done
/root/spin_mode 1 >/dev/null 2>&1; echo 2 > $S/ivh_pv_preempt_src; echo 4095 > $S/ivh_pv_beat_publish_mask
echo "DONE -> $OUT"
