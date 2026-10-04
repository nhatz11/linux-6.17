#!/bin/bash
# RECALL of is_cs_preempted() for lock-holder preemption.
#
#   numerator   = distinct holds the predicate flagged
#                 (sum of ivh_cs_v_flagged[][] -- deduplicated, one per hold.
#                  NOT ivh_cs_fired, which counts ~88 re-probes per stall.)
#   denominator = holds landing in the >477us mode of ivh_cs_prev_hold_hist.
#                 Proven to be host preemption by the dose-response test:
#                 75.8 ppm loaded vs 0.3 ppm idle, 253x.
#
# HYPOTHESIS: recall is limited by OBSERVER AVAILABILITY, not by the
# predicate. is_cs_preempted() only runs while a waiter is spinning and
# sampling (every PV_PREV_CHECK_MASK=256 iterations). tier1/tier2 make
# waiters halt after ~197 iterations, so the observer leaves before the
# preemption is over. Arms below remove the early-bail and see if recall
# rises.
#
# Sampler OFF: the verdict is not used here, only the flagged COUNT, so
# there is no reason to pay the 48% sampler tax.
set -u
S=/proc/sys/kernel
set_(){ echo "$2" > $S/$1 2>/dev/null; }
set_ ivh_cs_track_enabled 1; set_ ivh_cs_owner_enable 1; set_ ivh_cs_owner_clear 1
set_ ivh_cs_head_probe 1;    set_ ivh_cs_head_bail 0
set_ ivh_cs_criterion 1;     set_ ivh_cs_noise_cycles 1100000    # 500us floor
set_ ivh_cs_verdict 1        # needed to populate ivh_cs_v_flagged
set_ ivh_tks_sampler_ns 0;   set_ ivh_vact_jump_ns 1500000
set_ ivh_pv_rot_enable 0;    set_ ivh_adaptive_mode 2
set_ ivh_pv_beat_threshold 11000000

R="python3 /root/ivh_tools/read_ivh_counters.py"
flag(){ $R ivh_cs_v_flagged 2>/dev/null | grep -oE 'nonzero_buckets=\[.*\]'; }
hist(){ $R ivh_cs_prev_hold_hist 2>/dev/null | grep -oE 'nonzero_buckets=\[.*\]'; }
spins(){ $R ivh_node_spin_iters_sum ivh_node_spin_attempts 2>/dev/null | awk '/^ivh_node/{print $3}' | tr '\n' ' '; }

arm(){ # $1=label $2=tier1 $3=tier2
	set_ ivh_pv_tier1_enable "$2"; set_ ivh_pv_tier2_enable "$3"; sleep 1
	local FA=$(flag) HA=$(hist) SA=$(spins)
	local t0=$(date +%s%N)
	timeout 120 hackbench -T -g1 -f8 -l250000 >/dev/null 2>&1
	local t1=$(date +%s%N)
	local FB=$(flag) HB=$(hist) SB=$(spins)
	python3 - "$1" "$t0" "$t1" "$FA" "$FB" "$HA" "$HB" "$SA" "$SB" <<'PY'
import re, sys
lab, t0, t1 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
def b(s): return {int(x): int(y) for x, y in re.findall(r'\((\d+),\s*(\d+)\)', s)}
fa, fb, ha, hb = b(sys.argv[4]), b(sys.argv[5]), b(sys.argv[6]), b(sys.argv[7])
sa = [int(x) for x in sys.argv[8].split()]
sb = [int(x) for x in sys.argv[9].split()]
wall = (t1 - t0) / 1e9
flagged = sum(fb.get(k, 0) - fa.get(k, 0) for k in range(0, 64))
preempted = sum(hb.get(k, 0) - ha.get(k, 0) for k in range(20, 32))
iters = (sb[0] - sa[0]) if len(sb) > 1 else 0
att = (sb[1] - sa[1]) if len(sb) > 1 else 0
rec = 100.0 * flagged / preempted if preempted else float('nan')
print(f"{lab:16} wall={wall:5.1f}s  preempted_holds={preempted:6d}  flagged={flagged:6d}  "
      f"RECALL={rec:6.1f}%   | waiter spin/pass={iters/max(att,1):7.0f} iters")
PY
}

echo "RECALL = flagged holds / holds in the >477us (preemption) mode"
echo "observer hypothesis: waiters that halt early stop watching the holder"
arm "t1+t2 (shipped)" 1 1
arm "t1 only"         1 0
arm "neither"         0 0
arm "t1+t2 (repeat)"  1 1
set_ ivh_pv_tier1_enable 1; set_ ivh_pv_tier2_enable 1; set_ ivh_cs_verdict 0
echo RECALL-DONE
