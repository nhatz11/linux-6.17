#!/bin/bash
# xchg_tail INFLATION. evict_debug=1 forces pv_evict_walk() on every handoff,
# so ivh_evict_walks == total handoffs ~= total queue entries ~= total xchg_tail.
# ivh_evict_requeued == extra xchg_tail (1 per requeue, qspinlock.c:437->:367->:404).
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict; R=/root/ivh_tools/read_ivh_counters.py
BLOCKS=${BLOCKS:-5}; DUR=${DUR:-10}; T=${T:-72}
OUT=$D/stepxchg_$(date +%H%M%S).csv
C="ivh_evict_walks ivh_evict_walks_acted ivh_evict_marked ivh_evict_requeued"
echo "blk,arm,iters,walks,acted,marked,requeued" > $OUT
ctr(){ timeout -k 5 90 python3 $R $C 2>/dev/null|awk '{printf "%s ",$3}'; }
setarm(){
  $D/arm.sh nt1_only >/dev/null || exit 1
  echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_head_bypass_probe
  echo 0 > $S/ivh_pv_trylock_relaxed; echo 0 > $S/ivh_pv_rot_probe
  echo 0 > $S/ivh_pv_rot_enable;      echo 0 > $S/ivh_pv_evict_node_stamp
  echo 4095 > $S/ivh_pv_beat_publish_mask; echo 220000 > $S/ivh_pv_beat_threshold
  echo 2 > $S/ivh_pv_preempt_src; echo 1 > $S/ivh_pv_evict_debug
  echo 4 > $S/ivh_pv_evict_hop_cap; echo 4 > $S/ivh_pv_requeue_max
  [ "$1" = skip ] && echo 1 > $S/ivh_pv_evict_enable || echo 0 > $S/ivh_pv_evict_enable
}
for b in $(seq 1 $BLOCKS); do
  for a in $(printf "base\nskip\n"|shuf); do
    setarm "$a"; read -r w0 a0 m0 r0 <<< "$(ctr)"
    Q=$(timeout -k 5 $((DUR+50)) /root/linux-6.17/qlockbench -t $T -d $DUR -Q 2>&1|tail -1)
    read -r w1 a1 m1 r1 <<< "$(ctr)"
    IT=$(echo "$Q"|cut -d, -f2)
    echo "$b,$a,$IT,$((w1-w0)),$((a1-a0)),$((m1-m0)),$((r1-r0))" >> $OUT
  done
  printf "  blk%-3s done\n" "$b"
done
echo 0 > $S/ivh_pv_evict_debug; echo 1 > $S/ivh_pv_evict_hop_cap
/root/spin_mode 1 >/dev/null 2>&1; echo 0 > $S/ivh_pv_evict_enable
echo "DONE -> $OUT"
