#!/bin/bash
# floor_test2.sh -- floor_test.sh + a per-mechanism FIRING CENSUS.
#
# WHY v2: floor_test.sh logged only ivh_beat_tier2_fired, which read EXACTLY 0
# across all 8 settled reps of floortest_1004-014814 -- so mig+AS was really
# mig+tier1 plus publish/probe overhead, and HEH and lock-skipping were never
# checked at all. "enable=1" is not "fires"; this version records every
# mechanism's own counter so the arm cannot lie about what it is.
#
# Also measured there, and the reason the floor is high: AS costs +61% on the
# per-entry wait (PV 1.741 -> AS 2.805 us/entry) across ~6000 entries/run while
# firing ~0 times. vips's spin noise is real (corr(spin,us/entry)=+0.94, 3.16x
# spread) but it is DRIZZLE -- worst runs average ~5.5 us/entry -- so a 50 us
# staleness test never trips. And corr(SEC,spin)=+0.02: vips's runtime variance
# is not lock spin at all.
#
# Corunner was restarted clean before this run and settled at cap_mean=776,
# identical to the 777 of the previous sitting, so contention is unchanged.
#
# ARMS: stock PV | mig+AS @ budget 32768 | mig+AS @ budget 32767
# Reps 1-4 are warm-up: floortest_1004-014814 inverted its verdict between n=4
# and n=12 (PV mean 18.2 -> 10.2 ms, as32768 132.5 -> 13.5). The report drops them.
set -u
T=/root/ivh_tools
S=/proc/sys/kernel
P=/root/parsec-benchmark
REPS="${REPS:-16}"
OUT=/root/ivh_logs/floor2_$(date +%m%d-%H%M%S).tsv
C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_head_spin_iters_sum ivh_head_spin_iters_bail_sum ivh_slowpath_wait_events ivh_beat_tier1_fired ivh_beat_tier2_checked ivh_beat_tier2_fired ivh_cs_check_calls ivh_cs_fired ivh_cs_abstain_young ivh_cs_head_bailed ivh_evict_marked ivh_evict_requeued ivh_evict_lookahead_refused"

snap() { python3 "$T/read_ivh_counters.py" $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2); printf "%s ", $2}'; }
capm() { awk 'NR>2 {s+=$11; n++} END {printf "%.0f", s/n}' /proc/ivh_cpu_stats; }

arm() {
	case "$1" in
	pv)
		bash "$T/pvbase.sh" >/dev/null 2>&1 || return 1
		echo 32768 > $S/ivh_pv_spin_threshold
		[ "$(cat $S/ivh_pv_tier2_enable)" = 0 ] || return 1
		;;
	as*)
		IVH_MASK=255 bash "$T/p11_arm.sh" 50 >/dev/null 2>&1 || return 1
		local B=${1#as}
		echo "$B" > $S/ivh_pv_spin_threshold
		[ "$(cat $S/ivh_pv_spin_threshold)" = "$B" ] || { echo "  BUDGETFAIL"; return 1; }
		[ "$(cat $S/ivh_pv_beat_threshold)" = 110000 ] || { echo "  BEATFAIL"; return 1; }
		[ "$(cat $S/ivh_cs_noise_cycles)"   = 110000 ] || { echo "  NOISEFAIL"; return 1; }
		[ "$(cat $S/ivh_universal_eligible)" = 1 ] || { echo "  MIG GATE SHUT"; return 1; }
		[ "$(cat $S/ivh_pv_tier2_enable)"   = 1 ] || return 1
		[ "$(cat $S/ivh_pv_evict_enable)"   = 1 ] || return 1
		[ "$(cat $S/ivh_cs_head_bail)"      = 1 ] || return 1
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
printf "arm\trep\tsec\tnode\thead\tent\tt1f\tt2chk\tt2f\tcschk\tcsf\tcsyoung\tcsbail\tevmark\tevreq\tevlaref\tcap\n" > "$OUT"
echo "### floor_test2: pv vs mig+AS@32768 vs mig+AS@32767, vips, reps=$REPS"
echo "### fresh corunner, 16 threads, settled cap_mean=776"
echo "### data $OUT   cap=$(capm)"
systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null
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
		printf "%s\t%s\t%s" "$a" "$rep" "$v" >> "$OUT"
		printf "\t%d" "$(( (${f[0]}-${b[0]}) + (${f[1]}-${b[1]}) ))" >> "$OUT"
		printf "\t%d" "$(( (${f[2]}-${b[2]}) + (${f[3]}-${b[3]}) ))" >> "$OUT"
		for i in 4 5 6 7 8 9 10 11 12 13 14; do
			printf "\t%d" "$(( ${f[$i]} - ${b[$i]} ))" >> "$OUT"
		done
		printf "\t%s\n" "$CM" >> "$OUT"
		echo "  rep$rep $a ${v}s  t2f=$(( ${f[7]}-${b[7]} )) csf=$(( ${f[9]}-${b[9]} )) evreq=$(( ${f[13]}-${b[13]} ))"
	done
	[ $((rep % 4)) -eq 0 ] && python3 "$T/floor2_report.py" "$OUT"
done

bash "$T/pvbase.sh" >/dev/null 2>&1
echo 32768 > $S/ivh_pv_spin_threshold
echo 22000 > $S/ivh_cs_noise_cycles
echo
echo "########## FINAL ##########"
python3 "$T/floor2_report.py" "$OUT"
echo "FLOOR2_DONE $OUT"
