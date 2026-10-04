#!/bin/bash
# WHY DON'T THEY WORK? The two funnels, on REAL workloads.
#
# HEAD BYPASS funnel (ivh_head_observe, qspinlock_paravirt.h:1639-1668):
#   samples      -> waiter sampled its predecessor while that pred was HEAD_SPINNING
#   stale        -> ...and the head's heartbeat read stale (head looks preempted)
#   held         -> ...but the lock was HELD  -> nothing a bypass could do
#   free_open    -> ...lock free AND pending clear -> already stealable, bypass adds nothing
#   ACTIONABLE   -> lock free AND pending SET -> the ONLY case bypass exists for
#   fired        -> a bypass actually cleared the stranded pending bit
#
# EVICTION funnel (pv_evict_walk, :2544-2742), with evict_debug=1 so the walk
# runs on EVERY handoff and ivh_evict_walks is the true denominator:
#   walks        -> handoffs examined
#   stop_halted  -> successor deliberately halted -> fairness rule forbids eviction
#   tail_stop    -> successor->next == NULL -> nobody behind to promote
#   cap_refused  -> starvation cap hit
#   la_refused   -> look-ahead found NO live replacement within hop_cap
#   MARKED       -> an eviction actually committed
#   promo_hist   -> promotion -> promoted node's ACQUISITION latency
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict
DUR=${DUR:-15}; T=${T:-$(nproc)}
OUT=$D/postmortem_$(date +%m%d-%H%M%S).txt
HB="ivh_head_obs_samples ivh_head_obs_stale ivh_head_obs_held ivh_head_obs_free_open ivh_head_obs_actionable ivh_head_bypass_fired ivh_head_bypass_raced_locked ivh_head_bypass_raced_clear"
EV="ivh_evict_walks ivh_evict_walks_acted ivh_evict_stop_halted ivh_evict_tail_stop ivh_evict_cap_refused ivh_evict_halt_race ivh_evict_marked ivh_evict_lookahead_refused"
base(){ $D/arm.sh nt1_only >/dev/null || exit 1
  for k in ivh_head_bypass_enable ivh_head_bypass_probe ivh_pv_trylock_relaxed \
           ivh_pv_rot_probe ivh_pv_rot_enable ivh_pv_evict_node_stamp ivh_pv_evict_debug \
           ivh_pv_evict_enable ivh_pv_requeue_nosteal ivh_pv_requeue_none \
           ivh_pv_evict_lookahead ivh_pv_camp_probe ivh_pv_evict_promo_hist; do echo 0 > $S/$k 2>/dev/null; done
  echo 1 > $S/ivh_pv_tier1_enable; echo 1 > $S/ivh_pv_tier2_enable
  echo 2 > $S/ivh_pv_preempt_src; echo 220000 > $S/ivh_pv_beat_threshold
  echo 4095 > $S/ivh_pv_beat_publish_mask; echo 32768 > $S/ivh_pv_spin_threshold; }
wl(){ case $1 in
  hackbench) timeout -k 5 600 hackbench -T -g4 -f8 -l50000 >/dev/null 2>&1 ;;
  dentry)    timeout -k 5 60 stress-ng --dentry $T -t ${DUR}s >/dev/null 2>&1 ;;
  dbench)    (cd /root/dbench_test 2>/dev/null||cd /root; timeout -k 5 $((DUR+60)) dbench -F -t $DUR $T -D /root/dbench_test >/dev/null 2>&1) ;;
 esac; }
snap(){ timeout -k 5 120 python3 /root/ivh_tools/read_ivh_counters.py $1 2>/dev/null; }
{
echo "############ HEAD BYPASS FUNNEL (bypass ARMED) ############"
for w in hackbench dentry dbench; do
  base; echo 1 > $S/ivh_head_bypass_probe; echo 1 > $S/ivh_pv_trylock_relaxed; echo 1 > $S/ivh_head_bypass_enable
  echo "--- $w BEFORE"; snap "$HB"; wl $w; echo "--- $w AFTER"; snap "$HB"
done
echo; echo "############ EVICTION FUNNEL (evict ARMED, debug=1 for true denominator) ############"
for w in hackbench dentry dbench; do
  base; echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_lookahead
  echo 1 > $S/ivh_pv_requeue_nosteal; echo 2 > $S/ivh_pv_evict_hop_cap
  echo 1 > $S/ivh_pv_evict_debug; echo 1 > $S/ivh_pv_evict_promo_hist
  echo "--- $w BEFORE"; snap "$EV ivh_evict_promo_hist ivh_evict_promo_unknown"
  wl $w
  echo "--- $w AFTER"; snap "$EV ivh_evict_promo_hist ivh_evict_promo_unknown"
done
} > $OUT 2>&1
/root/spin_mode 1 >/dev/null 2>&1
for k in ivh_head_bypass_enable ivh_head_bypass_probe ivh_pv_trylock_relaxed ivh_pv_evict_enable \
         ivh_pv_evict_lookahead ivh_pv_requeue_nosteal ivh_pv_evict_debug ivh_pv_evict_promo_hist; do echo 0 > $S/$k; done
echo 1 > $S/ivh_pv_evict_hop_cap
echo "DONE -> $OUT"
