#!/bin/bash
# Where is the "head halted" cliff, and does IVH's window open or close?
#
# Stock PV can only halt the head by exhausting ivh_pv_spin_threshold, which
# lands at roughly threshold x cycles-per-iteration. Measured at the default
# 32768 the cliff sits at ~953us: NOTHING below b21 ever had a halted head
# (0 of 1449 holds), then 48.5% above it.
#
# The CS predicate fires at ivh_cs_noise_cycles (250us). So IVH's addressable
# window is [250us, exhaustion time]:
#   LOWER threshold -> exhaustion moves toward 250us -> window CLOSES
#   RAISE threshold -> window WIDENS, and stock PV wastes more spinning
#
# Per threshold: stock PV (mode=0) vs IVH with the CS predicate acting.
# CLIFF = lowest bucket whose halted% exceeds 20%; it should track the
# threshold in the PV arm and stay pinned near the floor in the IVH arm.
set -u
S=/proc/sys/kernel
REPS="${REPS:-2}"
WL="${WL:--g1}"
set_(){ echo "$2" > $S/$1 2>/dev/null; }
for kv in ivh_cs_react_hist:1 ivh_cs_owner_enable:1 ivh_cs_owner_clear:1 \
          ivh_cs_track_enabled:1 ivh_cs_head_probe:1 ivh_pv_rot_enable:0 \
          ivh_tks_sampler_ns:0 ivh_cs_verdict:0 ivh_cs_criterion:1 \
          ivh_cs_noise_cycles:550000 ivh_pv_beat_threshold:11000000; do
	set_ "${kv%%:*}" "${kv#*:}"
done
echo "CS floor = 250us fixed.  Sweeping ivh_pv_spin_threshold (default 32768)."
echo
printf "%8s %6s %10s %8s %9s %8s\n" "spin_thr" "arm" "long+queue" "halted" "halted%" "cliff"

R="python3 /root/ivh_tools/read_ivh_counters.py"
rc(){ $R ivh_cs_react 2>/dev/null | grep -oE 'nonzero_buckets=\[.*\]' | tr '\n' '|'; }

run(){ # $1=threshold $2=arm
	case "$2" in
	PV)  set_ ivh_adaptive_mode 0 ;;
	IVH) set_ ivh_adaptive_mode 2; set_ ivh_cs_head_bail 1 ;;
	esac
	set_ ivh_pv_spin_threshold "$1"
	[ "$(cat $S/ivh_pv_spin_threshold)" = "$1" ] || { echo "  !! thr $1 rejected"; return; }
	sleep 1
	local A B
	A=$(rc)
	timeout 150 hackbench -T $WL -f8 -l250000 >/dev/null 2>&1
	B=$(rc)
	python3 /root/ivh_tools/spin_report.py "$1" "$2" "$A" "$B"
}

for t in 8192 16384 32768 65536 131072; do
	for r in $(seq 1 "$REPS"); do
		run "$t" PV
		run "$t" IVH
	done
done
set_ ivh_pv_spin_threshold 32768
set_ ivh_cs_react_hist 0
set_ ivh_cs_head_bail 0
set_ ivh_adaptive_mode 2
echo SPINSWEEP-DONE
