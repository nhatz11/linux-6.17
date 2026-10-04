#!/bin/bash
# hb_perf_clean.sh -- settle hackbench THROUGHPUT at the candidate thresholds.
#
# WHY: in the p11 sweep one host window contaminated four consecutive runs of
# rep2 (50/100/200/400 all at 47-49s) while the 800us arm and the pv arm, which
# ran LATER in the same rep, were normal at 22.9s and 18.4s. Rotation does not
# protect against an event that outlasts four runs. Paired per rep, rep1 gave
# +26.7% and +32.5% at 50 and 100us; rep2 gave -161% and -157%. n=3 cannot
# separate those, so this runs n=8 on three arms only.
#
# Spin is recorded alongside so the two outcomes come from one sitting.
set -u
T=/root/ivh_tools
S=/proc/sys/kernel
exec 9>/var/lock/ivh_clean_check.lock
flock -w 900 9 || { echo "FATAL: bench lock held"; exit 1; }

REPS="${REPS:-8}"
OUT=/root/ivh_logs/hbperf_$(date +%m%d-%H%M%S).tsv
C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_beat_tier2_fired ivh_cs_head_bailed"

printf "arm\trep\ttime\titers\tt2f\tcsb\tcap\n" > "$OUT"

snap() {
	python3 "$T/read_ivh_counters.py" $C 2>/dev/null |
		awk -F= '{gsub(/ /,"",$2); printf "%s ", $2}'
}

capmean() {
	awk 'NR>2 {s+=$11; n++} END {printf "%.0f", s/n}' /proc/ivh_cpu_stats
}

arm() {
	if [ "$1" = pv ]; then
		bash "$T/pvbase.sh" >/dev/null 2>&1 || return 1
		[ "$(cat $S/ivh_pv_tier2_enable)" = 0 ] || return 1
	else
		IVH_MASK=255 bash "$T/p11_arm.sh" "$1" >/dev/null 2>&1 || return 1
		echo 0 > $S/ivh_universal_eligible
		[ "$(cat $S/ivh_universal_eligible)" = 0 ] || return 1
		[ "$(cat $S/ivh_pv_tier2_enable)" = 1 ] || return 1
	fi
	sleep 1
}

echo "### hb_perf_clean: pv vs 50us vs 100us, mask 255, migration OFF, n=$REPS -> $OUT"

for rep in $(seq 1 "$REPS"); do
	case $((rep % 3)) in
		1) ORD="pv 50 100" ;;
		2) ORD="50 100 pv" ;;
		0) ORD="100 pv 50" ;;
	esac
	for a in $ORD; do
		arm "$a" || { echo "  ARMFAIL $a"; continue; }
		sync
		echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
		sleep 1
		CM=$(capmean)
		b=($(snap))
		t0=$(date +%s%N)
		timeout 300 hackbench -T -g1 -f8 -l150000 >/dev/null 2>&1
		t1=$(date +%s%N)
		f=($(snap))
		IT=$(( (${f[0]} - ${b[0]}) + (${f[1]} - ${b[1]}) ))
		T2=$(( ${f[2]} - ${b[2]} ))
		CS=$(( ${f[3]} - ${b[3]} ))
		TM=$(python3 -c "print(f'{($t1-$t0)/1e9:.3f}')")
		[ "$a" != pv ] && [ "$T2" -eq 0 ] && echo "  *** DEAD ARM $a: zero tier2"
		printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$a" "$rep" "$TM" "$IT" "$T2" "$CS" "$CM" >> "$OUT"
		echo "  rep$rep $a ${TM}s spin=$(python3 -c "print('%.1fs' % ($IT*26e-9))") t2f=$T2 csb=$CS cap=$CM"
	done
done

bash "$T/pvbase.sh" >/dev/null 2>&1
echo 22000 > $S/ivh_cs_noise_cycles

python3 "$T/hb_perf_report.py" "$OUT"
echo "HBPERF_DONE $OUT"
