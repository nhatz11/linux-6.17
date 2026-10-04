#!/bin/bash
# as_fire_vs_threshold.sh -- WHICH of the four AS mechanisms actually fires on
# vips, and does lowering the shared threshold bring any of them to life?
#
# WHY THIS EXISTS: floortest_1004-014814 had mig+AS@50us losing to stock PV on
# every statistic (settled reps 5-12: PV min 5.5 med 9.0 max 19.2 ms; AS@32768
# 6.5/16.0/31.2) while ivh_beat_tier2_fired was EXACTLY 0 in all 8 reps. The arm
# was configured correctly -- beat_threshold=110000 cyc = 50us -- so tier 2 was
# enabled and silent. "Enabled" is not "firing", and only tier 2 was logged, so
# HEH and lock-skipping were never checked at all.
#
# THE SUSPECTED CAUSE: vips spends 9.6 ms of node spin over 5872 slowpath
# entries = 1.635 us/entry = ~63 iterations. A 50 us staleness threshold cannot
# fire inside a 1.6 us wait, so AS is structurally unreachable here, not
# mistuned -- and mig+AS degenerates to mig+tier1 plus publish/probe overhead.
#
# WHAT IT MEASURES: a per-mechanism firing census, as RATES, per arm:
#   tier 1  ivh_beat_tier1_fired                 (boolean prev->state, no threshold)
#   tier 2  ivh_beat_tier2_fired / _checked      (reads beat_threshold)
#   HEH     ivh_cs_fired, _head_bailed, _abstain_young  (reads cs_noise_cycles)
#   skip    ivh_evict_marked, _requeued, _lookahead_refused (reads evict_threshold)
# ivh_cs_abstain_young counts exactly the "hold was younger than the threshold"
# abstains -- the direct witness for the structural-unreachability claim.
#
# The threshold sweep goes down to 10 us = 22000 cyc, the ivh_pv_evict_threshold
# clamp floor, which is the most sensitive setting the kernel will accept.
set -u
T=/root/ivh_tools
S=/proc/sys/kernel
P=/root/parsec-benchmark
REPS="${REPS:-5}"
THS="${THS:-10 25 50}"
OUT=/root/ivh_logs/asfire_$(date +%m%d-%H%M%S).tsv
C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_head_spin_iters_sum ivh_head_spin_iters_bail_sum ivh_slowpath_wait_events ivh_beat_tier1_fired ivh_beat_tier2_checked ivh_beat_tier2_fired ivh_cs_check_calls ivh_cs_fired ivh_cs_abstain_young ivh_cs_head_bailed ivh_evict_marked ivh_evict_requeued ivh_evict_lookahead_refused"

snap() { python3 "$T/read_ivh_counters.py" $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2); printf "%s ", $2}'; }
capm() { awk 'NR>2 {s+=$11; n++} END {printf "%.0f", s/n}' /proc/ivh_cpu_stats; }

arm() {
	if [ "$1" = pv ]; then
		bash "$T/pvbase.sh" >/dev/null 2>&1 || return 1
		echo 32768 > $S/ivh_pv_spin_threshold
		[ "$(cat $S/ivh_pv_tier2_enable)" = 0 ] || return 1
	else
		IVH_MASK=255 bash "$T/p11_arm.sh" "$1" >/dev/null 2>&1 || return 1
		local want; want=$(python3 -c "print(int(round($1*2200)))")
		# assert every one of the three thresholds landed -- a silent refusal
		# here is what made a whole campaign run at a stale value before.
		[ "$(cat $S/ivh_pv_beat_threshold)"  = "$want" ] || { echo "  BEATFAIL $1";  return 1; }
		[ "$(cat $S/ivh_cs_noise_cycles)"    = "$want" ] || { echo "  NOISEFAIL $1"; return 1; }
		[ "$(cat $S/ivh_pv_evict_threshold)" = "$want" ] || { echo "  EVICTFAIL $1"; return 1; }
		[ "$(cat $S/ivh_pv_tier2_enable)"    = 1 ] || return 1
		[ "$(cat $S/ivh_head_bypass_probe)"  = 1 ] || return 1
		[ "$(cat $S/ivh_pv_evict_enable)"    = 1 ] || return 1
		[ "$(cat $S/ivh_cs_head_bail)"       = 1 ] || return 1
		[ "$(cat $S/ivh_universal_eligible)" = 1 ] || return 1
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
flock -w 3600 9 || { echo "FATAL: bench lock"; exit 1; }
printf "arm\trep\tsec\tnode\thead\tent\tt1f\tt2chk\tt2f\tcschk\tcsf\tcsyoung\tcsbail\tevmark\tevreq\tevlaref\tcap\n" > "$OUT"
ARMS="pv $THS"
echo "### as_fire_vs_threshold: arms = $ARMS   reps=$REPS   vips, mask 255, migration ON"
echo "### Q: which of tier1/tier2/HEH/skip ever fires, and does a lower threshold wake any?"
echo "### data $OUT   cap=$(capm)"
systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null
runvips >/dev/null 2>&1      # warm-up, unrecorded; the report also drops rep 1

for rep in $(seq 1 "$REPS"); do
	ORD=$(python3 -c "
a='''$ARMS'''.split(); k=($rep-1)%len(a); print(' '.join(a[k:]+a[:k]))")
	for a in $ORD; do
		arm "$a" || { echo "  ARMFAIL $a"; continue; }
		sync; sleep 1
		CM=$(capm); b=($(snap))
		v=$(runvips)
		f=($(snap))
		printf "%s\t%s\t%s" "$a" "$rep" "$v" >> "$OUT"
		printf "\t%d" "$(( (${f[0]}-${b[0]}) + (${f[1]}-${b[1]}) ))" >> "$OUT"   # node iters
		printf "\t%d" "$(( (${f[2]}-${b[2]}) + (${f[3]}-${b[3]}) ))" >> "$OUT"   # head iters
		for i in 4 5 6 7 8 9 10 11 12 13 14; do
			printf "\t%d" "$(( ${f[$i]} - ${b[$i]} ))" >> "$OUT"
		done
		printf "\t%s\n" "$CM" >> "$OUT"
		echo "  rep$rep ${a}: ${v}s t2f=$(( ${f[7]}-${b[7]} )) csf=$(( ${f[9]}-${b[9]} )) young=$(( ${f[10]}-${b[10]} )) evreq=$(( ${f[13]}-${b[13]} ))"
	done
	[ $((rep % 2)) -eq 0 ] && python3 "$T/asfire_report.py" "$OUT"
done

bash "$T/pvbase.sh" >/dev/null 2>&1
echo 32768 > $S/ivh_pv_spin_threshold
echo 22000 > $S/ivh_cs_noise_cycles
echo
echo "########## FINAL ##########"
python3 "$T/asfire_report.py" "$OUT"
echo "ASFIRE_DONE $OUT"
