#!/bin/bash
# PHASE 1b: WERE THE VICTIMS ACTUALLY PREEMPTED?
# ivh_evict_gap_hist[b] = log2(cycles) from the evictor's commit stamp to the
# victim noticing in pv_requeue_node() (qspinlock_paravirt.h:1464-1483).
#   genuinely host-preempted -> ~1ms = 2.2M cycles -> bucket ~21
#   false positive (running)  -> ~us = thousands   -> bucket ~11
# Also dumps ivh_evict_age_used_hist / _true_hist / _cpubeat_hist: the age of
# the EVIDENCE each eviction acted on.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict; Q=/root/linux-6.17/qlockbench
DUR=${DUR:-20}; T=${T:-$(nproc)}; HOP=${HOP:-1}
OUT=$D/dissect2_$(date +%m%d-%H%M%S).txt
$D/arm.sh nt1_only >/dev/null || exit 1
for k in ivh_head_bypass_enable ivh_head_bypass_probe ivh_pv_trylock_relaxed \
         ivh_pv_rot_probe ivh_pv_rot_enable ivh_pv_evict_node_stamp ivh_pv_evict_debug; do echo 0 > $S/$k; done
echo 2 > $S/ivh_pv_preempt_src; echo 220000 > $S/ivh_pv_beat_threshold
echo 4095 > $S/ivh_pv_beat_publish_mask; echo 32768 > $S/ivh_pv_spin_threshold
echo $HOP > $S/ivh_pv_evict_hop_cap; echo 4 > $S/ivh_pv_requeue_max
echo 1 > $S/ivh_pv_evict_gap_hist; echo 1 > $S/ivh_pv_evict_age_hist
echo 1 > $S/ivh_pv_evict_enable
{
  echo "hop_cap=$HOP dur=$DUR T=$T thr=$(cat $S/ivh_pv_beat_threshold) tsc~2.2GHz"
  echo "--- before ---"
  timeout -k 5 150 python3 /root/ivh_tools/read_ivh_counters.py \
    ivh_evict_gap_hist ivh_evict_age_used_hist ivh_evict_age_true_hist \
    ivh_evict_cpubeat_hist ivh_evict_marked ivh_evict_requeued \
    ivh_evict_steal_ok ivh_evict_gap_negative 2>/dev/null
  echo "--- running qlockbench ---"
  timeout -k 5 $((DUR+60)) $Q -t $T -d $DUR -Q 2>&1 | tail -1
  echo "--- after ---"
  timeout -k 5 150 python3 /root/ivh_tools/read_ivh_counters.py \
    ivh_evict_gap_hist ivh_evict_age_used_hist ivh_evict_age_true_hist \
    ivh_evict_cpubeat_hist ivh_evict_marked ivh_evict_requeued \
    ivh_evict_steal_ok ivh_evict_gap_negative 2>/dev/null
} > $OUT 2>&1
/root/spin_mode 1 >/dev/null 2>&1
echo 0 > $S/ivh_pv_evict_enable; echo 0 > $S/ivh_pv_evict_gap_hist
echo 0 > $S/ivh_pv_evict_age_hist; echo 1 > $S/ivh_pv_evict_hop_cap
echo "DONE -> $OUT"
