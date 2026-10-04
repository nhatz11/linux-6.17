#!/bin/bash
S=/proc/sys/kernel
echo 0 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1
echo 2 > $S/ivh_pv_preempt_src; echo 1 > $S/ivh_cs_verdict; echo 0 > $S/ivh_cs_owner_clear
echo 0 > $S/ivh_tks_phase_pct; echo 200000 > $S/ivh_tks_sampler_ns; echo 400000 > $S/ivh_vact_jump_ns
for k in ivh_pv_evict_enable ivh_pv_evict_gap_hist ivh_pv_evict_age_hist \
         ivh_pv_evict_node_stamp ivh_pv_evict_lookahead ivh_pv_requeue_nosteal; do echo 1 > $S/$k; done
echo 2 > $S/ivh_pv_evict_hop_cap
vals(){ python3 /root/ivh_tools/read_ivh_counters.py ivh_evict_requeued ivh_evict_v 2>/dev/null \
        | grep -oE "[0-9]+$" | tr '\n' ' '; }
run_arm(){
  local M=$1 T=$2
  echo "$M" > $S/ivh_pv_beat_publish_mask
  echo "$T" > $S/ivh_pv_beat_threshold
  local a=($(vals))
  timeout 150 hackbench -T -g1 -f8 -l500000 >/dev/null 2>&1
  local b=($(vals))
  local ev=$(( ${b[0]} - ${a[0]} )) neg=$(( ${b[1]} - ${a[1]} ))
  local pos=$(( ${b[2]} - ${a[2]} )) amb=$(( ${b[3]} - ${a[3]} )) unj=$(( ${b[4]} - ${a[4]} ))
  local judg=$(( neg + pos + amb ))
  local prec=0
  [ $judg -gt 0 ] && prec=$(( 100 * (pos + amb) / judg ))
  printf "mask=%-5s T=%-7s evicts=%-6d neg=%-6d pos=%-4d amb=%-4d unjudg=%-5d  PRECISION=%d%%\n" \
    "$M" "$((T/2200))us" "$ev" "$neg" "$pos" "$amb" "$unj" "$prec"
}
echo "publish interval vs staleness threshold -- precision = (pos+ambiguous)/judgeable"
run_arm 4095 220000
run_arm 1023 220000
run_arm 255  220000
run_arm 63   220000
run_arm 4095 2200000
run_arm 255  1100000
echo 4095 > $S/ivh_pv_beat_publish_mask; echo 11000000 > $S/ivh_pv_beat_threshold
echo 0 > $S/ivh_tks_sampler_ns; echo 1500000 > $S/ivh_vact_jump_ns
echo SWEEP2-DONE
