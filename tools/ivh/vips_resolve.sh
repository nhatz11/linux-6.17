#!/bin/bash
# vips_resolve.sh -- settle vips with reps. Dedicated, AS@50us vs stock PV.
#
# PRE-COMMITTED DESIGN (decided before any data was collected):
#   target n   = 200 pairs
#   estimator  = ratio-of-means, (mean(PV)-mean(AS))/mean(PV)
#   interval   = paired bootstrap, 4000 resamples
#   report     = whatever it says, at n=200, including a negative result
# The run does NOT stop early on a favourable result. Power math: vips's CI
# half-width was 62.4pp at n=12 and scales 1/sqrt(n), so n=150 gives ~17.6pp and
# n=200 gives ~15.3pp. Against the pooled point estimate of +18.59% that is
# enough to clear zero if the effect is real, and enough to show it is not if it
# is not.
#
# WHY RATIO-OF-MEANS: vips's PV spin denominator varies 256x across runs. The
# mean of per-rep ratios is dominated by low-PV draws -- measured
# corr(PV draw, per-rep delta) = +0.73 -- which is what produced every earlier
# "vips loses spin" figure (-105.80%, -116.96%, -99.43%, -81.54%). On the same
# data ratio-of-means gives +18.59%.
#
# Progress is printed every 10 pairs so the interval can be watched tightening,
# but the stopping point is fixed at n=200 regardless of what it shows.
set -u
T=/root/ivh_tools
S=/proc/sys/kernel
P=/root/parsec-benchmark
TARGET="${TARGET:-200}"
OUT=/root/ivh_logs/vipsresolve_$(date +%m%d-%H%M%S).tsv
C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_head_spin_iters_sum ivh_head_spin_iters_bail_sum ivh_slowpath_wait_events ivh_beat_tier2_fired"

snap() { python3 "$T/read_ivh_counters.py" $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2); printf "%s ", $2}'; }
capm() { awk 'NR>2 {s+=$11; n++} END {printf "%.0f", s/n}' /proc/ivh_cpu_stats; }

arm() {
	if [ "$1" = pv ]; then
		bash "$T/pvbase.sh" >/dev/null 2>&1 || return 1
		[ "$(cat $S/ivh_pv_tier2_enable)" = 0 ] || return 1
	else
		IVH_MASK=255 bash "$T/p11_arm.sh" 50 >/dev/null 2>&1 || return 1
		echo 0 > $S/ivh_universal_eligible
		[ "$(cat $S/ivh_pv_tier2_enable)" = 1 ] || return 1
		[ "$(cat $S/ivh_universal_eligible)" = 0 ] || return 1
	fi
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
flock -w 600 9 || { echo "FATAL: bench lock"; exit 1; }
printf "arm\trep\tsec\tnode\thead\tentries\tt2f\tcap\n" > "$OUT"
echo "### vips_resolve: PRE-COMMITTED n=$TARGET pairs, AS@50us mask255 vs stock PV"
echo "### estimator = ratio-of-means + bootstrap; no early stop on a good result"
echo "### data $OUT   start cap=$(capm)"

systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null
runvips >/dev/null 2>&1          # warm-up, unrecorded

for rep in $(seq 1 "$TARGET"); do
	case $((rep % 2)) in 1) ORD="pv as" ;; 0) ORD="as pv" ;; esac
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
	done
	if [ $((rep % 10)) -eq 0 ]; then
		echo "--- after $rep pairs ---"
		python3 "$T/vips_resolve_report.py" "$OUT" || true
	fi
done

bash "$T/pvbase.sh" >/dev/null 2>&1
echo 22000 > $S/ivh_cs_noise_cycles
echo
echo "########## FINAL at the pre-committed n=$TARGET ##########"
python3 "$T/vips_resolve_report.py" "$OUT"
echo "VIPSRESOLVE_DONE $OUT"
