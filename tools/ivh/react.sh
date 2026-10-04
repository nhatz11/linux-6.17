#!/bin/bash
# G-LOCK-46b: "the holder ran long, and by the time it released its head had
# stopped spinning on it."
#
#   ivh_cs_react[had_tail][state][bucket]
#     state 0 = head still spinning
#     state 1 = head halted on its own account          <-- the only evidence
#     state 2 = WE latched _Q_SLOW_VAL ourselves, via pv_kick_node() on our
#               own cpu a few lines after the stamp, for a successor that was
#               halted AT HANDOFF. That successor is then WOKEN by the same
#               HALTED->HASHED cmpxchg and spins on us for the whole hold
#               while the bit stays set. Counting it would invert the claim.
#     had_tail = a queue still existed at release. An A4 acquirer has no MCS
#               successor and is never anyone's prev (qspinlock.c:576), so
#               without this its holds land in "head not halted" when there
#               was no head, biasing recall DOWN by tens of percent.
#
# Read at ONE site on the holder's own cpu, from cachelines it already owns,
# STRICTLY BEFORE the releasing store. No cross-cpu deposit, so none of the
# attribution loss that made G-LOCK-45 credit 33 of 104 detections.
#
# THE CONTROL IS THE POINT. _Q_SLOW_VAL says halted, not why: a head that
# merely exhausted ivh_pv_spin_threshold sets it too, and that is stock PV
# behaviour which happens with none of this machinery. So:
#
#   bail=0  head can only halt by exhausting its spin budget   -> BASELINE
#   bail=1  the CS predicate can also halt it                  -> +detector
#
# The DELTA is this detector's contribution. The bail=1 absolute number
# credits upstream's work and must not be quoted on its own.
set -u
S=/proc/sys/kernel
FLOOR="${FLOOR:-550000}"          # 250us, the operating point from the sweep
WL="${WL:--g1}"

set_() {
	echo "$2" > $S/$1 2>/dev/null
	[ "$(cat $S/$1 2>/dev/null)" = "$2" ] || echo "  !! $1 rejected ($2)"
}

# Interlock: ivh_pv_skip_point=1 makes pv_defer_promote() write _Q_SLOW_VAL on
# the holder's cpu as well. state 2 catches it, but it also perturbs handoff,
# so refuse rather than measure through it.
if [ "$(cat $S/ivh_pv_skip_point 2>/dev/null || echo 0)" != "0" ]; then
	echo "*** ivh_pv_skip_point is ON -- pv_defer_promote perturbs this. ABORT ***"
	exit 1
fi

for kv in ivh_cs_track_enabled:1 ivh_cs_owner_enable:1 ivh_cs_owner_clear:1 \
          ivh_cs_head_probe:1 ivh_cs_criterion:1 ivh_pv_rot_enable:0 \
          ivh_tks_sampler_ns:0 ivh_cs_verdict:0 ivh_cs_react_hist:1 \
          ivh_adaptive_mode:2 ivh_pv_beat_threshold:11000000; do
	set_ "${kv%%:*}" "${kv#*:}"
done
set_ ivh_cs_noise_cycles "$FLOOR"
echo "floor=$(( FLOOR / 2200 ))us   workload: hackbench -T $WL -f8"
echo

R="python3 /root/ivh_tools/read_ivh_counters.py"
rc() { $R ivh_cs_react 2>/dev/null | grep -oE 'nonzero_buckets=\[.*\]' | tr '\n' '|'; }
xt() { $R ivh_head_halt_events ivh_cs_bail_suppressed ivh_cs_stamp_overwrote 2>/dev/null \
	| awk '/^ivh_/{print $NF}' | tr '\n' ' '; }

arm() {
	set_ ivh_cs_head_bail "$1"
	sleep 1
	local A SA B SB
	A=$(rc); SA=$(xt)
	timeout 150 hackbench -T $WL -f8 -l250000 >/dev/null 2>&1
	B=$(rc); SB=$(xt)
	python3 /root/ivh_tools/react_report.py "$2" "$A" "$B" "$SA" "$SB"
}

echo "                          ---- holds >= 477us, queue present ----"
arm 0 "bail=0 BASELINE"
arm 1 "bail=1 +detector"
arm 0 "bail=0 repeat"
arm 1 "bail=1 repeat"
set_ ivh_cs_react_hist 0
set_ ivh_cs_head_bail 0
echo REACT-DONE
