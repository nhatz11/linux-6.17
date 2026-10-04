#!/bin/bash
# TEST 3 -- lock-WAITER eviction, using instrumentation ALREADY IN THE TREE.
# No patch. ivh_evict_gap_hist measures, victim-side at requeue, how long the
# evicted waiter was ACTUALLY away; ivh_evict_cpubeat_hist records the victim
# CPU's heartbeat age at eviction commit.
#
# PASS/FAIL is the tree's own criterion (asm/ivh_tsc_beat.h:1140-1148):
#   gap_hist median in HUNDREDS OF MICROSECONDS -> evictions skipped real
#     stalls, the detector has per-event value.
#   gap_hist median in SINGLE MICROSECONDS -> evicting vCPUs that were about
#     to run anyway; the detector is the problem.
#   cpubeat_hist LOW (fresh CPU beat) alongside a STALE age_used_hist = the
#     CPU was alive doing something else -> that class is the false positives.
set -u
S=/proc/sys/kernel
source /root/ivh_tools/bench_guard.sh
echo 0 > $S/ivh_universal_eligible          # migration OFF: it prevents the stalls
/root/spin_mode 2 >/dev/null 2>&1
echo 2 > $S/ivh_pv_preempt_src
echo 1 > $S/ivh_pv_evict_enable
echo 1 > $S/ivh_pv_evict_gap_hist
echo 1 > $S/ivh_pv_evict_age_hist
echo 1 > $S/ivh_pv_evict_promo_hist
echo 1 > $S/ivh_pv_evict_node_stamp
echo 1 > $S/ivh_pv_evict_lookahead
echo 1 > $S/ivh_pv_requeue_nosteal
echo 2 > $S/ivh_pv_evict_hop_cap
for f in ivh_pv_evict_enable ivh_pv_evict_gap_hist ivh_pv_evict_age_hist \
         ivh_pv_evict_promo_hist ivh_universal_eligible; do
  printf "  %-26s %s\n" $f "$(cat $S/$f)"; done
echo
echo "=== hackbench 120s, migration OFF ==="
timeout 200 hackbench -T -g1 -f8 -l600000 >/dev/null 2>&1
echo
python3 /root/ivh_tools/read_ivh_counters.py ivh_evict_requeued ivh_evict_gap_negative \
  ivh_evict_age_negative ivh_evict_halt_averted ivh_evict_gap_hist \
  ivh_evict_cpubeat_hist ivh_evict_age_used_hist ivh_evict_age_true_hist 2>/dev/null
echo TEST3-DONE
