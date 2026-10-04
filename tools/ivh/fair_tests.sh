#!/bin/bash
# Tests 1-3 run INSIDE the detectors' design envelope.
#   T3: publish interval must be SHORTER than the staleness threshold.
#       mask=4095 -> 99-369us per publish vs a 100us threshold = guaranteed
#       false positives. mask=255 -> ~6-23us, comfortably inside.
#       cheap_now=0 so the walker ages against a fresh rdtsc, not a stale beat.
#   T2: criterion 0 -- the only criterion that consults the heartbeat at all.
#   T1: report jumps and headroom vs the 1000/s/cpu ceiling.
set -u
S=/proc/sys/kernel
source /root/ivh_tools/bench_guard.sh
rd(){ python3 /root/ivh_tools/read_ivh_counters.py "$@" 2>/dev/null; }

echo "############ TEST 3: LW eviction, publish interval INSIDE threshold ############"
echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1
echo 2 > $S/ivh_pv_preempt_src
echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_lookahead
echo 1 > $S/ivh_pv_requeue_nosteal; echo 2 > $S/ivh_pv_evict_hop_cap
echo 1 > $S/ivh_pv_evict_node_stamp 2>/dev/null
echo 1 > $S/ivh_pv_evict_age_hist   2>/dev/null
echo 0 > $S/ivh_pv_evict_cheap_now  2>/dev/null
for MASK in 4095 255; do
  echo "$MASK" > $S/ivh_pv_beat_publish_mask
  echo "--- publish_mask=$MASK (publish every $((MASK+1)) iters), threshold=$(cat $S/ivh_pv_beat_threshold) cyc"
  A=$(rd ivh_evict_requeued ivh_evict_halt_averted 2>/dev/null | tr '\n' ' ')
  timeout 60 hackbench -T -g1 -f8 -l300000 >/dev/null 2>&1
  B=$(rd ivh_evict_requeued ivh_evict_halt_averted 2>/dev/null | tr '\n' ' ')
  echo "    before: $A"
  echo "    after : $B"
done
echo 4095 > $S/ivh_pv_beat_publish_mask

echo; echo "############ TEST 2: LH detector, criterion 0 (uses the heartbeat) ############"
echo 1 > $S/ivh_cs_track_enabled; echo 0 > $S/ivh_cs_criterion
echo 1 > $S/ivh_cs_owner_enable; echo 1 > $S/ivh_cs_owner_clear
echo 1 > $S/ivh_cs_scan; echo 1 > $S/ivh_cs_head_probe
for f in ivh_cs_criterion ivh_cs_owner_enable ivh_cs_scan ivh_cs_head_probe ivh_cs_owed_ticks; do
  printf "    %-22s %s\n" $f "$(cat $S/$f)"; done
timeout 90 hackbench -T -g1 -f8 -l400000 >/dev/null 2>&1
rd ivh_cs_check_calls ivh_cs_long_hold ivh_cs_healthy_long ivh_cs_fired ivh_cs_abstain_nohz ivh_cs_abstain_retag 2>/dev/null

echo; echo "############ TEST 1: vact jumps + ceiling headroom ############"
(timeout 40 hackbench -T -g1 -f8 -l400000 >/dev/null 2>&1 &); sleep 3
./preempt_detect_guest.sh 20 2>&1 | tail -13
echo FAIR-TESTS-DONE
