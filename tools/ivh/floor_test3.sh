#!/bin/bash
# floor_test3.sh -- stock PV vs mig+AS @ budget 32767, vips. Two arms only.
#
# WHY TWO ARMS: the 32768 arm is dropped on theory -- T=32768 is a power of two
# so (loop & mask)==0 on iteration 1 and EVERY slowpath entry publishes once,
# while T=32767 pushes the first publish to iteration 256, which vips's ~63-iter
# average entry rarely reaches. 32767 is the low-publish arm, so it is the one
# that can show whether the floor cost is the publish.
#
# THE MASK IS INERT HERE, which is why there is no mask arm: at T=32768 the
# first publish lands on iteration 1 for mask 255 AND 4095, and vips never
# reaches a second publish either way. T's alignment is the only publish lever
# on this workload, and the kernel refuses any mask below 255 anyway.
#
# WHAT WE ALREADY KNOW (floortest_1004-014814, settled reps 5-12):
#   PV 1.741 us/entry, as32768 2.805 (+61%), as32767 3.902 (+124%)
#   tier2 fired 0 in all 8 reps -> mig+AS was really mig+tier1 + overhead
#   corr(spin,us/entry)=+0.94 but corr(SEC,spin)=+0.02
# The low-publish arm being the WORSE one already argues the floor is not the
# publish. This run re-tests it on a freshly restarted corunner (settled
# cap_mean=776, identical to the previous sitting's 777) with every mechanism's
# own fire counter recorded, so an inert arm cannot pass as a live one.
set -u
T=/root/ivh_tools
S=/proc/sys/kernel
P=/root/parsec-benchmark
REPS="${REPS:-12}"
OUT=/root/ivh_logs/floor3_$(date +%m%d-%H%M%S).tsv
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
	as32767)
		IVH_MASK=255 bash "$T/p11_arm.sh" 50 >/dev/null 2>&1 || return 1
		echo 32767 > $S/ivh_pv_spin_threshold
		[ "$(cat $S/ivh_pv_spin_threshold)" = 32767  ] || { echo "  BUDGETFAIL"; return 1; }
		[ "$(cat $S/ivh_pv_beat_threshold)" = 110000 ] || { echo "  BEATFAIL";   return 1; }
		[ "$(cat $S/ivh_cs_noise_cycles)"   = 110000 ] || { echo "  NOISEFAIL";  return 1; }
		[ "$(cat $S/ivh_pv_beat_publish_mask)" = 255 ] || { echo "  MASKFAIL";   return 1; }
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
echo "### floor_test3: stock PV vs mig+AS@32767 (low-publish arm), vips, reps=$REPS"
echo "### fresh corunner 16 threads, settled cap_mean=776; warm-up reps 1-2 dropped by the report"
echo "### data $OUT   cap=$(capm)"
systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null
runvips >/dev/null 2>&1

for rep in $(seq 1 "$REPS"); do
	case $((rep % 2)) in 1) ORD="pv as32767" ;; 0) ORD="as32767 pv" ;; esac
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
	[ $((rep % 4)) -eq 0 ] && WARMUP=2 python3 "$T/floor2_report.py" "$OUT"
done

bash "$T/pvbase.sh" >/dev/null 2>&1
echo 32768 > $S/ivh_pv_spin_threshold
echo 22000 > $S/ivh_cs_noise_cycles
echo
echo "########## FINAL ##########"
WARMUP=2 python3 "$T/floor2_report.py" "$OUT"
echo "FLOOR3_DONE $OUT"
