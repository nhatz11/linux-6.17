#!/bin/bash
# SIZE THE PRIZE for professor's pointer 1, BEFORE building anything.
# ivh_evict_hop_cap (qspinlock_paravirt.h:2629, UNGATED) counts walks that
# burned every hop without finding a live waiter. Those walks marked `hopcap`
# victims and then promoted a node they never classified -- pure no-gain cost.
#   no-gain marked  = hop_cap_count * hopcap
#   gain    marked  = marked - no-gain
# If no-gain dominates, pointer 1 has real upside.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict; R=/root/ivh_tools/read_ivh_counters.py
BLOCKS=${BLOCKS:-6}; DUR=${DUR:-10}; T=${T:-$(nproc)}
C="ivh_evict_marked ivh_evict_requeued ivh_evict_hop_cap ivh_evict_tail_stop ivh_evict_cap_refused"
OUT=$D/nogain_$(date +%m%d-%H%M%S).csv
echo "blk,hopcap,iters,marked,requeued,hopexh,tailstop,caprefused" > $OUT
ctr(){ timeout -k 5 90 python3 $R $C 2>/dev/null|awk '{printf "%s ",$3}'; }
arm(){
  $D/arm.sh nt1_only >/dev/null || exit 1
  for k in ivh_head_bypass_enable ivh_head_bypass_probe ivh_pv_trylock_relaxed \
           ivh_pv_rot_probe ivh_pv_rot_enable ivh_pv_evict_node_stamp ivh_pv_evict_debug; do echo 0 > $S/$k; done
  echo 2 > $S/ivh_pv_preempt_src; echo 220000 > $S/ivh_pv_beat_threshold
  echo 4095 > $S/ivh_pv_beat_publish_mask
  echo "$1" > $S/ivh_pv_evict_hop_cap; echo 4 > $S/ivh_pv_requeue_max
  echo 1 > $S/ivh_pv_evict_enable
}
for hc in 1 2 4; do
  for b in $(seq 1 $BLOCKS); do
    arm $hc; read -r m0 r0 h0 t0 c0 <<< "$(ctr)"
    Q=$(timeout -k 5 $((DUR+50)) /root/linux-6.17/qlockbench -t $T -d $DUR -Q 2>&1|tail -1|cut -d, -f2)
    read -r m1 r1 h1 t1 c1 <<< "$(ctr)"
    echo "$hc,$b,${Q:-0},$((m1-m0)),$((r1-r0)),$((h1-h0)),$((t1-t0)),$((c1-c0))" >> $OUT
  done
  printf "  hopcap=%s done\n" "$hc"
done
/root/spin_mode 1 >/dev/null 2>&1; echo 0 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_hop_cap
echo "DONE -> $OUT"
