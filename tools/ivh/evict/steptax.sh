#!/bin/bash
# What does the TSC heartbeat itself cost? Same arm throughout (tier1 on, tier2
# off, no mechanism) -- only the publish configuration changes:
#   src0      preempt_src=0: heartbeat never published. The zero-tax baseline.
#   m511      publish every 512 spin iterations (~6us)   <- what we have been running
#   m4095     every 4096 (~48us)                          <- compiled default
#   m65535    every 65536 (~780us)                        <- tick-dominated
# Host absences are ~953us median, so the 1000Hz tick alone may resolve them.
# If the coarse masks match src0, the tax is removable and every mechanism in
# the project gets cheaper.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict
BLOCKS=${BLOCKS:-8}; DUR=${DUR:-10}; T=${T:-72}
OUT=$D/steptax_$(date +%H%M%S).csv
echo "blk,cfg,ops,iters,hit,p50,p99,p999,p9999,max,over1ms" > $OUT
setarm(){ $D/arm.sh control >/dev/null || exit 1
  echo 0 > $S/ivh_pv_rot_probe; echo 0 > $S/ivh_head_bypass_probe
  echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_pv_trylock_relaxed
  case $1 in
    src0)   echo 0 > $S/ivh_pv_preempt_src ;;
    m511)   echo 2 > $S/ivh_pv_preempt_src; echo 511    > $S/ivh_pv_beat_publish_mask ;;
    m4095)  echo 2 > $S/ivh_pv_preempt_src; echo 4095   > $S/ivh_pv_beat_publish_mask ;;
    m65535) echo 2 > $S/ivh_pv_preempt_src; echo 65535  > $S/ivh_pv_beat_publish_mask ;;
  esac; }
for b in $(seq 1 $BLOCKS); do
  for c in $(printf "src0\nm511\nm4095\nm65535\n"|shuf); do
    setarm $c
    Q=$(timeout -k 5 $((DUR+50)) /root/linux-6.17/qlockbench -t $T -d $DUR -Q 2>&1|tail -1)
    echo "$b,$c,$Q" >> $OUT
  done
  printf "  blk%-3s done\n" "$b"
done
echo 2 > $S/ivh_pv_preempt_src; echo 4095 > $S/ivh_pv_beat_publish_mask
echo "DONE -> $OUT"
