#!/bin/bash
# G-LOCK-46b, three arms x 6 reps, ROUND-ROBIN so slow host-contention drift
# cancels across arms instead of piling into whichever ran last.
#
#   PV     ivh_adaptive_mode=0  -- stock PV. No IVH adaptive spinning at all.
#                                 The head still halts by exhausting
#                                 ivh_pv_spin_threshold, and pv_kick_node
#                                 still sets _Q_SLOW_VAL, so this is the true
#                                 floor for "head was no longer spinning".
#   bail=0 mode=2, head_bail=0  -- CS predicate DETECT-ONLY. NOT a clean
#                                 "detector off" control: an unstamped
#                                 fastpath acquirer (qspinlock.h:139 takes the
#                                 lock with a plain cmpxchg and never calls
#                                 ivh_cs_owner_stamp) is invisible to
#                                 is_cs_preempted(), so exhaustion is the only
#                                 thing that can save its head. Exhaustion is
#                                 intended coverage here, not a confound.
#   bail=1 mode=2, head_bail=1  -- the CS predicate may halt the head.
#
# Numerator/denominator: ivh_cs_react[had_tail][state][bucket], flattened by
# the reader to 6 rows of 32 (row = had_tail*3 + state). Only [1][1] counts;
# [1][2] is the pv_kick_node self-set case and is excluded (it outnumbered
# real halts 63793:479 in the first run -- 134x -- and would have
# manufactured the result).
set -u
S=/proc/sys/kernel
FLOOR="${FLOOR:-550000}"
WL="${WL:--g1}"
REPS="${REPS:-6}"
OUT=/tmp/react3.raw
: > $OUT

set_() {
	echo "$2" > $S/$1 2>/dev/null
	[ "$(cat $S/$1 2>/dev/null)" = "$2" ] || echo "  !! $1 rejected ($2)"
}

if [ "$(cat $S/ivh_pv_skip_point 2>/dev/null || echo 0)" != "0" ]; then
	echo "*** ivh_pv_skip_point is ON -- pv_defer_promote perturbs this. ABORT ***"
	exit 1
fi

# Stamping and the release hook are gated on ivh_cs_owner_enable /
# ivh_cs_owner_clear, which are independent of ivh_adaptive_mode, so the
# counter works identically in the PV arm.
for kv in ivh_cs_track_enabled:1 ivh_cs_owner_enable:1 ivh_cs_owner_clear:1 \
          ivh_cs_head_probe:1 ivh_cs_criterion:1 ivh_pv_rot_enable:0 \
          ivh_tks_sampler_ns:0 ivh_cs_verdict:0 ivh_cs_react_hist:1 \
          ivh_pv_beat_threshold:11000000; do
	set_ "${kv%%:*}" "${kv#*:}"
done
set_ ivh_cs_noise_cycles "$FLOOR"
echo "floor=$(( FLOOR / 2200 ))us  workload: hackbench -T $WL -f8  reps=$REPS  (round-robin)"
echo

R="python3 /root/ivh_tools/read_ivh_counters.py"
rc() { $R ivh_cs_react 2>/dev/null | grep -oE 'nonzero_buckets=\[.*\]' | tr '\n' '|'; }

arm() {
	case "$1" in
	PV)     set_ ivh_adaptive_mode 0 ;;
	bail0)  set_ ivh_adaptive_mode 2; set_ ivh_cs_head_bail 0 ;;
	bail1)  set_ ivh_adaptive_mode 2; set_ ivh_cs_head_bail 1 ;;
	esac
	sleep 1
	local A B
	A=$(rc)
	timeout 150 hackbench -T $WL -f8 -l250000 >/dev/null 2>&1
	B=$(rc)
	python3 /root/ivh_tools/react_tally.py "$1" "$A" "$B" >> $OUT
	tail -1 $OUT
}

for r in $(seq 1 "$REPS"); do
	echo "--- rep $r ---"
	arm PV
	arm bail0
	arm bail1
done
set_ ivh_cs_react_hist 0
set_ ivh_cs_head_bail 0
set_ ivh_adaptive_mode 2
echo
python3 /root/ivh_tools/react_tally.py --summary < $OUT
echo REACT3-DONE
