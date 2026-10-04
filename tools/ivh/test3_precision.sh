#!/bin/bash
# Can the LW detector reach 80-90% precision from sysctls alone?
#
# A healthy waiter is falsely marked stale when its PUBLISH INTERVAL exceeds
# the STALENESS THRESHOLD. Measured: mask=4095 -> 99-369us per publish against
# a 100us threshold, so healthy waiters look stale most of the time.
#   publish_mask  lowers P   (255 -> ~6-23us)
#   beat_threshold raises T
# precision = evictions that found the victim ACTUALLY away >= 119us,
#             over all evictions (from ivh_evict_gap_hist buckets 18+).
set -u
S=/proc/sys/kernel
source /root/ivh_tools/bench_guard.sh
echo 0 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1
echo 2 > $S/ivh_pv_preempt_src
for k in ivh_pv_evict_enable ivh_pv_evict_gap_hist ivh_pv_evict_age_hist \
         ivh_pv_evict_node_stamp ivh_pv_evict_lookahead ivh_pv_requeue_nosteal; do echo 1 > $S/$k; done
echo 2 > $S/ivh_pv_evict_hop_cap
snap(){ python3 /root/ivh_tools/read_ivh_counters.py ivh_evict_gap_hist 2>/dev/null | tr '\n' '|'; }
for SPEC in "4095 220000" "1023 220000" "255 220000" "4095 1100000" "4095 2200000" "255 1100000"; do
  set -- $SPEC; M=$1; T=$2
  echo "$M" > $S/ivh_pv_beat_publish_mask; echo "$T" > $S/ivh_pv_beat_threshold
  [ "$(cat $S/ivh_pv_beat_publish_mask)" = "$M" ] || { echo "mask $M rejected"; continue; }
  [ "$(cat $S/ivh_pv_beat_threshold)" = "$T" ] || { echo "thr $T rejected"; continue; }
  A=$(snap)
  timeout 110 hackbench -T -g1 -f8 -l400000 >/dev/null 2>&1
  B=$(snap)
  echo "### mask=$M (publish every $((M+1)) iters)  threshold=$T cyc ($((T/2200))us)"
  echo "BEFORE $A"
  echo "AFTER  $B"
done
echo 4095 > $S/ivh_pv_beat_publish_mask; echo 11000000 > $S/ivh_pv_beat_threshold
echo PRECISION-DONE
