#!/bin/bash
# floor_test.sh -- bring the mig+AS FLOOR down to PV's.
#
# THE PROBLEM (measured on vips, n=21 each arm):
#   PV   min  3.2 ms   median 12.7   max 116.0   spread 36x
#   AS   min 10.4 ms   median 22.2   max  42.6   spread 4.1x
# AS already wins the CEILING -- worst case 116 -> 42.6 ms, variance ratio 8.2x,
# F-test significant. That is the preemption response working. But AS never
# reaches PV's floor, and halting cannot RAISE spin, so the floor gap is a fixed
# cost paid on every run.
#
# THE CAUSE: ivh_node_stamp_set() writes pn->head_ctl (offset 24) while the
# successor polls prev->state (offset 20) -- 4 bytes apart in a 32-byte struct,
# same 64-byte line. Every publish invalidates the line the next waiter spins on.
# Measured: publishing with ALL AS mechanisms DISABLED costs vips +74.6% more
# spin than stock PV (3/3 reps), so the floor cost is the publish, not the halt.
#
# THE FIX UNDER TEST: the publish fires when (loop & mask)==0 with loop counting
# down from ivh_pv_spin_threshold, so the first publish lands (T mod 256)+1
# iterations in. T=32768 is a power of two -> iteration 1, every entry. And that
# publish is REDUNDANT: pv_init_node() stamped the node at enqueue microseconds
# earlier. T=32767 (== 255 mod 256) pushes it to iteration 256, which vips (138
# iters/entry) and ebizzy (43) never reach, while hackbench/memtier/dbench keep
# theirs. Budget changes 0.003%; applied identically to BOTH arms.
#
# ARMS: stock PV | mig+AS @ budget 32768 | mig+AS @ budget 32767
# Migration is ON in the AS arms, per the goal (pv vs mig+AS).
# SUCCESS = the AS floor (min / p10) falls toward PV's while the ceiling stays low.
set -u
T=/root/ivh_tools
S=/proc/sys/kernel
P=/root/parsec-benchmark
REPS="${REPS:-12}"
OUT=/root/ivh_logs/floortest_$(date +%m%d-%H%M%S).tsv
C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_head_spin_iters_sum ivh_head_spin_iters_bail_sum ivh_slowpath_wait_events ivh_beat_tier2_fired"

snap() { python3 "$T/read_ivh_counters.py" $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2); printf "%s ", $2}'; }
capm() { awk 'NR>2 {s+=$11; n++} END {printf "%.0f", s/n}' /proc/ivh_cpu_stats; }

arm() {   # $1 = pv | as32768 | as32767
	case "$1" in
	pv)
		bash "$T/pvbase.sh" >/dev/null 2>&1 || return 1
		echo 32768 > $S/ivh_pv_spin_threshold
		[ "$(cat $S/ivh_pv_tier2_enable)" = 0 ] || return 1
		;;
	as*)
		# p11_arm.sh leaves migration ON -- that is what the goal asks for.
		IVH_MASK=255 bash "$T/p11_arm.sh" 50 >/dev/null 2>&1 || return 1
		local B=${1#as}
		echo "$B" > $S/ivh_pv_spin_threshold
		[ "$(cat $S/ivh_pv_spin_threshold)" = "$B" ] || { echo "  BUDGETFAIL"; return 1; }
		[ "$(cat $S/ivh_universal_eligible)" = 1 ] || { echo "  MIG GATE SHUT"; return 1; }
		[ "$(cat $S/ivh_pv_tier2_enable)" = 1 ] || return 1
		;;
	esac
	sleep 1
}

runvips() {
	local t0 t1
	t0=$(date +%s%N)
	( cd "$P/pkgs/apps/vips/run" && IM_CONCURRENCY=16 \
	  "$P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips" im_benchmark \
	  orion_18000x18000.v output.v >/dev/null 2>&1 )
	t1=$(date +%s%N)
	python3 -c "print(f'{($t1-$t0)/1e9:.3f}')"
}

exec 9>/var/lock/ivh_clean_check.lock
flock -w 3600 9 || { echo "FATAL: bench lock"; exit 1; }
printf "arm\trep\tsec\tnode\thead\tentries\tt2f\tcap\n" > "$OUT"
echo "### floor_test: pv vs mig+AS@32768 vs mig+AS@32767, vips, reps=$REPS"
echo "### success = AS floor falls toward PV's while the ceiling stays low"
echo "### data $OUT   cap=$(capm)"
runvips >/dev/null 2>&1

for rep in $(seq 1 "$REPS"); do
	case $((rep % 3)) in
	1) ORD="pv as32768 as32767" ;;
	2) ORD="as32768 as32767 pv" ;;
	0) ORD="as32767 pv as32768" ;;
	esac
	for a in $ORD; do
		arm "$a" || { echo "  ARMFAIL $a"; continue; }
		sync; sleep 1
		CM=$(capm); b=($(snap))
		v=$(runvips)
		f=($(snap))
		printf "%s\t%s\t%s\t%d\t%d\t%d\t%d\t%s\n" "$a" "$rep" "$v" \
			"$(( (${f[0]} - ${b[0]}) + (${f[1]} - ${b[1]}) ))" \
			"$(( (${f[2]} - ${b[2]}) + (${f[3]} - ${b[3]}) ))" \
			"$(( ${f[4]} - ${b[4]} ))" "$(( ${f[5]} - ${b[5]} ))" "$CM" >> "$OUT"
		echo "  rep$rep $a ${v}s"
	done
	[ $((rep % 4)) -eq 0 ] && python3 "$T/floor_report.py" "$OUT"
done

bash "$T/pvbase.sh" >/dev/null 2>&1
echo 32768 > $S/ivh_pv_spin_threshold
echo 22000 > $S/ivh_cs_noise_cycles
echo
echo "########## FINAL ##########"
python3 "$T/floor_report.py" "$OUT"
echo "FLOORTEST_DONE $OUT"
