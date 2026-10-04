#!/bin/bash
# Make the WAITER detector accurate. Hypothesis: it fires on its own publish
# interval, not on host behaviour. A waiter is falsely stale whenever its
# time-since-publish exceeds the threshold, which is guaranteed when
#   publish interval P  >=  staleness threshold T.
# Measured P at mask=4095: 99-369us.  Shipped T: 220000 cyc = 100us.  P >= T.
# Fix = make P << T, from either side. Both are runtime sysctls.
#
# Scored with the G-LOCK-43 audit armed, so precision is per-event against
# host-validated evidence rather than inferred.
set -u
S=/proc/sys/kernel
echo 0 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1
echo 2 > $S/ivh_pv_preempt_src; echo 1 > $S/ivh_cs_verdict
echo 0 > $S/ivh_cs_owner_clear
# sampler clock so the windows are judgeable (99.5% at 600us lag)
echo 0 > $S/ivh_tks_phase_pct; echo 200000 > $S/ivh_tks_sampler_ns
echo 400000 > $S/ivh_vact_jump_ns
for k in ivh_pv_evict_enable ivh_pv_evict_gap_hist ivh_pv_evict_age_hist \
         ivh_pv_evict_node_stamp ivh_pv_evict_lookahead ivh_pv_requeue_nosteal; do echo 1 > $S/$k; done
echo 2 > $S/ivh_pv_evict_hop_cap
snap(){ python3 /root/ivh_tools/read_ivh_counters.py ivh_evict_requeued ivh_evict_v 2>/dev/null \
        | grep -oE "= *[0-9]+|sum=[0-9]+" | grep -oE "[0-9]+" | tr '\n' ' '; }
printf "%-8s %-9s %9s %9s %9s %9s %9s\n" mask thresh evicts neg pos ambig unjudg
for SPEC in "4095 220000" "1023 220000" "255 220000" "63 220000" "4095 2200000" "255 1100000"; do
  set -- $SPEC; M=$1; T=$2
  echo "$M" > $S/ivh_pv_beat_publish_mask; echo "$T" > $S/ivh_pv_beat_threshold
  [ "$(cat $S/ivh_pv_beat_publish_mask)" = "$M" ] || { echo "mask $M rejected"; continue; }
  A=($(snap)); timeout 150 hackbench -T -g1 -f8 -l500000 >/dev/null 2>&1; B=($(snap))
  printf "%-8s %-9s %9d %9d %9d %9d %9d\n" "$M" "$((T/2200))us" \
    $(( ${B[0]}-${A[0]} )) $(( ${B[1]}-${A[1]} )) $(( ${B[2]}-${A[2]} )) \
    $(( ${B[3]}-${A[3]} )) $(( ${B[4]}-${A[4]} ))
done
echo 4095 > $S/ivh_pv_beat_publish_mask; echo 11000000 > $S/ivh_pv_beat_threshold
echo 0 > $S/ivh_tks_sampler_ns; echo 1500000 > $S/ivh_vact_jump_ns
echo FIX-WAITER-DONE
