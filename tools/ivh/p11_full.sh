#!/bin/bash
# p11_full.sh -- POINT 11, full rerun. One shared staleness threshold across all
# three TSC mechanisms, swept 50/100/200/400/800/1600us, all five workloads,
# stock PV vs AS-alone (migration OFF both arms), mask 255 (the kernel's floor).
# Workload inputs and thread counts unchanged.
#
# ADAPTIVE REPS. Start at 3 pairs per cell. After each pair, test whether the
# cell has CONVERGED:
#     spin and perf each either (a) CI excludes zero  -> a clear verdict, or
#                               (b) CI half-width < HW -> tight enough to call neutral
# Keep adding pairs until both converge or MAXREP is hit. The reps actually used
# are recorded per cell, so "how many reps it took to be clean" is an output, not
# a guess.
#
# WHY THIS SWEEP: AS can only ADD iterations through the re-arm penalty --
# `threshold` is re-read inside the outer for(;;) (qspinlock_paravirt.h:1919 and
# :3595), so one false-positive halt costs a full fresh budget. That cost is
# fixed at ivh_pv_spin_threshold iterations, so as a MULTIPLE of normal spinning
# it is 20x on dbench, 60x on hackbench, 237x on vips and 766x on ebizzy. Low-spin
# workloads are the most damaged by misfires, so the threshold that serves them
# is a HIGHER one that fires only on genuinely long waits.
#
# METRIC: node + counted-head iterations. Covers both spin loops and contains no
# wall-clock term, so it cannot be inflated by the stolen time AS's own halts
# cause (measured at +9.35% host runqueue wait, 8/8 reps). The head's
# `goto gotlock` successes are unrecorded and excluded from BOTH arms, making
# this a conservative lower bound.
set -u
T=/root/ivh_tools
S=/proc/sys/kernel
P=/root/parsec-benchmark
MINREP="${MINREP:-3}"
MAXREP="${MAXREP:-12}"
HW="${HW:-5.0}"          # CI half-width (percentage points) that counts as settled
MASK="${MASK:-255}"
THRS="${THRS:-50 100 200 400 800 1600}"
OUT=/root/ivh_logs/p11full_$(date +%m%d-%H%M%S).tsv
SUM=${OUT%.tsv}_summary.txt
C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_head_spin_iters_sum ivh_head_spin_iters_bail_sum ivh_slowpath_wait_events ivh_beat_tier2_fired ivh_cs_head_bailed ivh_evict_marked"

snap() { python3 "$T/read_ivh_counters.py" $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2); printf "%s ", $2}'; }
capm() { awk 'NR>2 {s+=$11; n++} END {printf "%.0f", s/n}' /proc/ivh_cpu_stats; }

arm() {   # $1 = pv|as   $2 = threshold us
	if [ "$1" = pv ]; then
		bash "$T/pvbase.sh" >/dev/null 2>&1 || return 1
		[ "$(cat $S/ivh_pv_tier2_enable)" = 0 ] || return 1
		[ "$(cat $S/ivh_universal_eligible)" = 0 ] || return 1
	else
		IVH_MASK=$MASK bash "$T/p11_arm.sh" "$2" >/dev/null 2>&1 || return 1
		echo 0 > $S/ivh_universal_eligible
		local cyc; cyc=$(python3 -c "print(int(round($2*2200)))")
		[ "$(cat $S/ivh_universal_eligible)" = 0 ] || return 1
		[ "$(cat $S/ivh_pv_beat_threshold)" = "$cyc" ] || return 1
		[ "$(cat $S/ivh_pv_evict_threshold)" = "$cyc" ] || return 1
		[ "$(cat $S/ivh_cs_noise_cycles)" = "$cyc" ] || return 1
		[ "$(cat $S/ivh_pv_beat_publish_mask)" = "$MASK" ] || return 1
	fi
	sleep 1
}

prep() {
	case "$1" in
	dbench) rm -rf /root/dbench_test; mkdir -p /root/dbench_test ;;
	memtier)
		systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null; sleep 1
		memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=15 2>/dev/null 9>&-; sleep 2 ;;
	*) systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null ;;
	esac
}

runwl() {   # $1 = workload  -> echoes "value"
	case "$1" in
	hackbench) timeout 300 hackbench -T -g1 -f8 -l150000 2>&1 | grep -oP '^Time:\s*\K[0-9.]+' | head -1 ;;
	memtier)   timeout 180 /root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 \
	             -t 16 -c 50 --key-maximum=100 --ratio=1:0 -d 32 --test-time=10 --hide-histogram 2>&1 \
	           | grep -oP 'Totals\s+\K[0-9.]+' | head -1 ;;
	dbench)    timeout 180 dbench -t 15 16 -D /root/dbench_test 2>&1 | grep -oP 'Throughput\s+\K[0-9.]+' | head -1 ;;
	ebizzy)    ( cd /root && timeout 180 /home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304 2>&1 ) | grep -oP '^\K[0-9]+(?= records/s)' | head -1 ;;
	vips)      local t0 t1; t0=$(date +%s%N)
	           ( cd "$P/pkgs/apps/vips/run" && IM_CONCURRENCY=16 "$P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips" \
	             im_benchmark orion_18000x18000.v output.v >/dev/null 2>&1 )
	           t1=$(date +%s%N); python3 -c "print(f'{($t1-$t0)/1e9:.3f}')" ;;
	esac
}

exec 9>/var/lock/ivh_clean_check.lock
flock -w 9000 9 || { echo "FATAL: bench lock"; exit 1; }
printf "thr\twl\tarm\trep\tval\tnode\thead\tentries\tt2f\tcsb\tev\tcap\n" > "$OUT"
: > "$SUM"
echo "### p11_full thrs=[$THRS] mask=$MASK reps=$MINREP..$MAXREP (HW<${HW}pp) cap=$(capm)"
echo "### data $OUT   summary $SUM"

for TH in $THRS; do
	echo "################## threshold = ${TH}us ##################"
	for WL in hackbench memtier dbench ebizzy vips; do
		echo "########## ${WL} @ ${TH}us ##########"
		# one unrecorded warm-up for the input-bound ones
		case "$WL" in vips|ebizzy) prep "$WL"; runwl "$WL" >/dev/null 2>&1 ;; esac
		rep=0
		while [ "$rep" -lt "$MAXREP" ]; do
			rep=$((rep + 1))
			case $((rep % 2)) in 1) ORD="pv as" ;; 0) ORD="as pv" ;; esac
			for a in $ORD; do
				arm "$a" "$TH" || { echo "  ARMFAIL $a"; continue; }
				prep "$WL"
				sync
				case "$WL" in hackbench|memtier|ebizzy) echo 3 > /proc/sys/vm/drop_caches 2>/dev/null ;; esac
				sleep 1
				CM=$(capm); b=($(snap))
				v=$(runwl "$WL")
				f=($(snap))
				printf "%s\t%s\t%s\t%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%s\n" \
					"$TH" "$WL" "$a" "$rep" "${v:-NA}" \
					"$(( (${f[0]} - ${b[0]}) + (${f[1]} - ${b[1]}) ))" \
					"$(( (${f[2]} - ${b[2]}) + (${f[3]} - ${b[3]}) ))" \
					"$(( ${f[4]} - ${b[4]} ))" "$(( ${f[5]} - ${b[5]} ))" \
					"$(( ${f[6]} - ${b[6]} ))" "$(( ${f[7]} - ${b[7]} ))" "$CM" >> "$OUT"
			done
			if [ "$rep" -ge "$MINREP" ]; then
				if python3 "$T/p11_full_converged.py" "$OUT" "$TH" "$WL" "$HW"; then
					echo "  CONVERGED at rep $rep"
					break
				fi
			fi
		done
		python3 "$T/p11_full_report.py" "$OUT" "$TH" "$WL" | tee -a "$SUM"
	done
done

bash "$T/pvbase.sh" >/dev/null 2>&1
echo 22000 > $S/ivh_cs_noise_cycles
systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null
echo; echo "################## FULL RESULT ##################"
python3 "$T/p11_full_report.py" "$OUT" | tee -a "$SUM"
echo "P11FULL_DONE $OUT"
