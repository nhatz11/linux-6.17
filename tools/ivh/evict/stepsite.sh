#!/bin/bash
# Is the +4% a DELAY AT THE ARRIVAL POINT?
# The in-spin dose sweep was flat, but those publishes happen AFTER joining the
# queue -- they cannot stagger arrivals. The only knob that adds work inside
# pv_init_node() itself (before queued_spin_trylock() and xchg_tail()) is
# ivh_pv_evict_node_stamp=1, which adds ivh_node_stamp_set(): a read + RMW of
# head_ctl. With evict_enable=0 its only other readers are dead code.
#   src0   no publish at all
#   base   per-entry publish, mask 65535 (0 in-spin)
#   stamp  base + node_stamp=1  <- MORE work at the arrival site
# If stamp > base, delay-at-arrival is confirmed. If flat, the mechanism is
# something other than delay.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict
BLOCKS=${BLOCKS:-12}; DUR=${DUR:-10}; T=${T:-72}
OUT=$D/stepsite_$(date +%H%M%S).csv
echo "blk,arm,ops,iters,hit,p50,p99,p999,p9999,max,over1ms" > $OUT
setarm(){
  case $1 in pv) /root/spin_mode 1 >/dev/null 2>&1; return 0 ;; *) $D/arm.sh control >/dev/null||exit 1 ;; esac
  echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_head_bypass_probe
  echo 0 > $S/ivh_pv_trylock_relaxed; echo 0 > $S/ivh_pv_rot_probe
  echo 0 > $S/ivh_pv_evict_node_stamp; echo 220000 > $S/ivh_pv_beat_threshold
  case $1 in
    src0)  echo 0 > $S/ivh_pv_preempt_src ;;
    base)  echo 2 > $S/ivh_pv_preempt_src; echo 65535 > $S/ivh_pv_beat_publish_mask ;;
    stamp) echo 2 > $S/ivh_pv_preempt_src; echo 65535 > $S/ivh_pv_beat_publish_mask
           echo 1 > $S/ivh_pv_evict_node_stamp
           [ "$(cat $S/ivh_pv_evict_node_stamp)" = 1 ] || { echo "STAMP FAIL"; exit 1; } ;;
  esac
}
for b in $(seq 1 $BLOCKS); do
  for a in $(printf "pv\nsrc0\nbase\nstamp\n"|shuf); do
    setarm "$a"
    Q=$(timeout -k 5 $((DUR+50)) /root/linux-6.17/qlockbench -t $T -d $DUR -Q 2>&1|tail -1)
    echo "$b,$a,$Q" >> $OUT
  done
  printf "  blk%-3s done\n" "$b"
done
/root/spin_mode 1 >/dev/null 2>&1; echo 2 > $S/ivh_pv_preempt_src
echo 4095 > $S/ivh_pv_beat_publish_mask; echo 0 > $S/ivh_pv_evict_node_stamp
echo "DONE -> $OUT"
