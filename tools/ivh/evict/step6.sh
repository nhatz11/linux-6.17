#!/bin/bash
# Did ivh_rot_probe inflate the +3.17%?
#
# rot_probe was 1 in the winning campaign -- identical in all arms, so it cannot
# CREATE the delta, but ivh_rot_ack_slow() runs on every slowpath acquisition
# (rdtsc + remote per-CPU read + ilog2 + 3 RMWs) and ivh_rot_stamp_release()
# adds rdtsc + remote store on every slow unlock. If that tax falls unevenly on
# the two arms it changes the measured size of the win.
#
# 2x2: bypass {off,on} x rot_probe {0,1}, all at t=16, randomized per block.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict
BLOCKS=${BLOCKS:-12}; DUR=${DUR:-10}
OUT=$D/step6_$(date +%H%M%S).csv
echo "blk,arm,rot,ops,iters,hit,p50,p99,p999,p9999,max,over1ms" > $OUT
setarm(){ $D/arm.sh control >/dev/null || exit 1
  echo 511 > $S/ivh_pv_beat_publish_mask; echo 220000 > $S/ivh_pv_beat_threshold
  echo 1 > $S/ivh_head_bypass_probe; echo "$2" > $S/ivh_pv_rot_probe
  case $1 in
    A) echo 0 > $S/ivh_pv_trylock_relaxed; echo 0 > $S/ivh_head_bypass_enable ;;
    C) echo 1 > $S/ivh_pv_trylock_relaxed; echo 1 > $S/ivh_head_bypass_enable ;;
  esac
  [ "$(cat $S/ivh_pv_rot_probe)" = "$2" ] || exit 1; }
for b in $(seq 1 $BLOCKS); do
  for cfg in $(printf "A:0\nA:1\nC:0\nC:1\n"|shuf); do
    a=${cfg%%:*}; r=${cfg##*:}
    setarm $a $r
    Q=$(timeout -k 5 $((DUR+35)) /root/linux-6.17/qlockbench -t 16 -d $DUR -Q 2>&1|tail -1)
    echo "$b,$a,$r,$Q" >> $OUT
  done
  printf "  blk%-3s done\n" "$b"
done
echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_pv_trylock_relaxed
echo 0 > $S/ivh_head_bypass_probe; echo 0 > $S/ivh_pv_rot_probe
echo "DONE -> $OUT"
