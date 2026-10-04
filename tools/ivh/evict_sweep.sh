#!/bin/bash
# LOCK SKIPPING, isolated. tier2 OFF so ivh_pv_beat_threshold governs ONLY
# eviction. Eviction reads the NODE stamp (pn->head_ctl), refreshed by
# ivh_node_publish_in_spin every 4096 spin iterations (~47-250us) -- far
# fresher than the per-cpu tick heartbeat tier 2 uses (~3ms floor), so it
# can support a much lower threshold.
#
# Victims should be waiters whose predecessor really is away. The check:
# evictions should SCALE WITH HOST CONTENTION and be rare when idle.
set -u
S=/proc/sys/kernel
set_(){ echo "$2" > $S/$1 2>/dev/null; }
set_ ivh_tks_sampler_ns 0
set_ ivh_adaptive_mode 2
set_ ivh_pv_tier1_enable 1
set_ ivh_pv_tier2_enable 0          # isolate: threshold now only drives evict
set_ ivh_pv_evict_enable 1
set_ ivh_pv_evict_node_stamp 1      # use the NODE stamp, not the cpu beat
set_ ivh_pv_evict_gap_hist 1
set_ ivh_pv_evict_hop_cap 2
set_ ivh_pv_evict_lookahead 1
set_ ivh_pv_requeue_nosteal 1
set_ ivh_pv_spin_threshold 32768
R="python3 /root/ivh_tools/read_ivh_counters.py"
g(){ $R ivh_evict_marked ivh_evict_requeued ivh_evict_halt_race ivh_evict_cap_refused 2>/dev/null | awk '/^ivh_/{print $NF}' | tr '\n' ' '; }
printf "%9s %9s %10s %10s %10s\n" "thr" "marked" "requeued" "halt_race" "cap_ref"
for t in 110000 220000 550000 1100000 2200000 11000000; do
  set_ ivh_pv_beat_threshold $t
  [ "$(cat $S/ivh_pv_beat_threshold)" = "$t" ] || { echo "  !! $t rejected"; continue; }
  sleep 1
  A=($(g)); timeout 130 hackbench -T -g1 -f8 -l250000 >/dev/null 2>&1; B=($(g))
  printf "%7dus %9d %10d %10d %10d\n" $((t/2200)) $((B[0]-A[0])) $((B[1]-A[1])) $((B[2]-A[2])) $((B[3]-A[3]))
done
set_ ivh_pv_evict_enable 0; set_ ivh_pv_tier2_enable 1; set_ ivh_pv_beat_threshold 11000000
echo EVICTSWEEP-DONE
