#!/bin/bash
# overnight.sh -- finish POINT 11 (best shared staleness threshold, all 6
# benchmarks) then POINT 10 (does that threshold help the wider suite?).
#
# Baseline is STOCK PV in every arm pair (pvbase.sh: all AS knobs zeroed AND
# asserted). Tier 1 stays enabled there because it IS upstream pv_wait_early(),
# i.e. part of PV, not part of AS. Migration is OFF everywhere
# (ivh_universal_eligible=0) so this measures adaptive spinning alone.
#
# SPIN METRIC: node_spin_iters * 26ns (tools/bpf/docs/spin_time_measurement.md).
# Ratio between arms is exact regardless of the constant. NODE spin only -- head
# spin is ~20-27% of total and uninstrumented.
#
# SPIN REDUCTION, per the convention required:
#   THROUGHPUT workloads: saved = spin_PV * (ops_AS / ops_PV) - spin_AS
#       (both arms run a FIXED WALL TIME, so the faster arm mechanically accrues
#        more total spin; without normalising to the work actually done the
#        comparison is invalid -- eval_final.md sec 9)
#   TIME workloads:       saved = spin_PV - spin_AS
#       (fixed work, so totals are directly comparable and must NOT be scaled)
#
# Each arm asserts its threshold, its publish mask, the shut migration gate, and
# a non-zero tier2 fire count. A dead arm announces itself instead of returning
# a confident null.
set -u
T=/root/ivh_tools
S=/proc/sys/kernel
P=/root/parsec-benchmark
LOG=/root/ivh_logs
MASK=255
BREPS="${BREPS:-4}"
CREPS="${CREPS:-3}"

STAMP=$(date +%m%d-%H%M%S)
BOUT=$LOG/ovn_p11_$STAMP.tsv
COUT=$LOG/ovn_p10_$STAMP.tsv
C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_slowpath_wait_events ivh_beat_tier2_fired ivh_cs_head_bailed ivh_evict_marked"

snap() { python3 "$T/read_ivh_counters.py" $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2); printf "%s ", $2}'; }
capmean() { awk 'NR>2 {s+=$11; n++} END {printf "%.0f", s/n}' /proc/ivh_cpu_stats; }

arm() {
	if [ "$1" = pv ]; then
		bash "$T/pvbase.sh" >/dev/null 2>&1 || return 1
		[ "$(cat $S/ivh_pv_tier2_enable)" = 0 ] || return 1
		[ "$(cat $S/ivh_universal_eligible)" = 0 ] || return 1
	else
		IVH_MASK=$MASK bash "$T/p11_arm.sh" "$1" >/dev/null 2>&1 || return 1
		echo 0 > $S/ivh_universal_eligible
		local cyc
		cyc=$(python3 -c "print(int(round($1*2200)))")
		[ "$(cat $S/ivh_universal_eligible)" = 0 ] || return 1
		[ "$(cat $S/ivh_pv_beat_threshold)" = "$cyc" ] || return 1
		[ "$(cat $S/ivh_cs_noise_cycles)" = "$cyc" ] || return 1
		[ "$(cat $S/ivh_pv_beat_publish_mask)" = "$MASK" ] || return 1
	fi
	sleep 1
}

prep() {
	case "$1" in
	dbench_16_noF) rm -rf /root/dbench_test; mkdir -p /root/dbench_test ;;
	memtier_memcached)
		systemctl stop memcached >/dev/null 2>&1
		pkill -9 -x memcached 2>/dev/null
		sleep 1
		memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=15 2>/dev/null 9>&-
		sleep 2 ;;
	ebizzy_mmap)
		( cd /root && /home/nick/Desktop/ebizzy -S 3 -t 16 -m -s 4194304 >/dev/null 2>&1 ) ;;
	fsmark_tmpfs) rm -rf /dev/shm/fsmark; mkdir -p /dev/shm/fsmark ;;
	esac
}

# name|TYPE|dir|command|extractor   (extractor "WALL" = use wall-clock seconds)
MT="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=100 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"
SET_P11=(
"hackbench_pipe_thr|TIME|/root|hackbench -T -g1 -f8 -l150000|WALL"
"memtier_memcached|THROUGHPUT|/root|$MT|grep -oP 'Totals\s+\K[0-9.]+'"
"dbench_16_noF|THROUGHPUT|/root|dbench -t 15 16 -D /root/dbench_test|grep -oP 'Throughput\s+\K[0-9.]+'"
"parsec_vips|TIME|$P/pkgs/apps/vips/run|IM_CONCURRENCY=16 $P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v|WALL"
"ebizzy_mmap|THROUGHPUT|/root|/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304|grep -oP '^\K[0-9]+(?= records/s)'"
"nhextend_fin|THROUGHPUT|/root/linux-6.17|NHEXTEND_LOOP_SPIN=600000 ./NHextend-fin -l -n 16|grep -oP 'Ran for \K[0-9]+'"
)

SET_P10=(
"parsec_bodytrack|TIME|$P/pkgs/apps/bodytrack/run|$P/pkgs/apps/bodytrack/inst/amd64-linux.gcc/bin/bodytrack sequenceB_261 4 261 4000 5 0 16|WALL"
"parsec_freqmine|TIME|$P/pkgs/apps/freqmine/run|OMP_NUM_THREADS=16 $P/pkgs/apps/freqmine/inst/amd64-linux.gcc/bin/freqmine webdocs_250k.dat 11000|WALL"
"parsec_blackscholes|TIME|$P/pkgs/apps/blackscholes/run|$P/pkgs/apps/blackscholes/inst/amd64-linux.gcc/bin/blackscholes 16 in_10M.txt prices.txt|WALL"
"parsec_ferret|TIME|$P/pkgs/apps/ferret/run|$P/pkgs/apps/ferret/inst/amd64-linux.gcc/bin/ferret corel lsh queries 50 20 16 output.txt|WALL"
"parsec_swaptions|TIME|$P/pkgs/apps/swaptions/run|$P/pkgs/apps/swaptions/inst/amd64-linux.gcc/bin/swaptions -ns 128 -sm 1000000 -nt 16|WALL"
"parsec_dedup|TIME|$P/pkgs/kernels/dedup/run|$P/pkgs/kernels/dedup/inst/amd64-linux.gcc/bin/dedup -c -p -v -t 16 -i media.dat -o output.dat.ddp|WALL"
"parsec_canneal|TIME|$P/pkgs/kernels/canneal/run|$P/pkgs/kernels/canneal/inst/amd64-linux.gcc/bin/canneal 16 15000 2000 400000.nets 128|WALL"
"parsec_streamcluster|TIME|$P/pkgs/kernels/streamcluster/run|$P/pkgs/kernels/streamcluster/inst/amd64-linux.gcc/bin/streamcluster 10 20 128 1000000 200000 5000 none output.txt 16|WALL"
"stressng_dentry|THROUGHPUT|/root|stress-ng --dentry 16 -t 15s --metrics-brief|grep -oP 'dentry\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+'"
"sysbench_threads|THROUGHPUT|/root|sysbench threads --threads=16 --thread-locks=4 --time=15 run|grep -oP 'total number of events:\s*\K[0-9]+'"
"schbench|THROUGHPUT|/root|/root/bench/schbench/schbench -m 2 -t 8 -r 15|grep -oP 'average rps:\s*\K[0-9.]+'"
"fsmark_tmpfs|THROUGHPUT|/root|fs_mark -d /dev/shm/fsmark -D 16 -n 2000 -s 4096 -t 16 -L 1|grep -oP '^\s*\d+\s+\d+\s+\d+\s+\K[0-9.]+'"
)

run_set() {
	local outfile="$1" reps="$2" arms="$3"; shift 3
	local -n SET="$1"
	printf "workload\ttype\tarm\trep\tvalue\titers\tentries\tt2f\tcsb\tev\tcap\n" > "$outfile"
	local nar; nar=$(echo "$arms" | wc -w)
	for e in "${SET[@]}"; do
		IFS='|' read -r n ty wd cmd ex <<< "$e"
		echo "########## $n [$ty] ##########"
		for rep in $(seq 1 "$reps"); do
			set -- $arms
			local K=$# ORD="" i
			for i in $(seq 0 $((K-1))); do
				eval "ORD=\"\$ORD \${$(( (i + rep - 1) % K + 1 ))}\""
			done
			for a in $ORD; do
				arm "$a" || { echo "  ARMFAIL $a"; continue; }
				prep "$n"
				sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
				local CM b t0 t1 f out v IT EN T2 CS EV
				CM=$(capmean); b=($(snap)); t0=$(date +%s%N)
				out=$( ( cd "$wd" && timeout 900 bash -c "$cmd" ) 2>&1 9>&- )
				t1=$(date +%s%N); f=($(snap))
				if [ "$ex" = WALL ]; then
					v=$(python3 -c "print(f'{($t1-$t0)/1e9:.3f}')")
				else
					v=$(echo "$out" | eval "$ex" 2>/dev/null | head -1)
				fi
				IT=$(( (${f[0]} - ${b[0]}) + (${f[1]} - ${b[1]}) ))
				EN=$(( ${f[2]} - ${b[2]} )); T2=$(( ${f[3]} - ${b[3]} ))
				CS=$(( ${f[4]} - ${b[4]} )); EV=$(( ${f[5]} - ${b[5]} ))
				[ "$a" != pv ] && [ "$T2" -eq 0 ] && echo "  *** DEAD ARM $a: zero tier2"
				[ "$a" = pv ] && [ "$T2" -ne 0 ] && echo "  *** PV CONTAMINATED: t2f=$T2"
				printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
					"$n" "$ty" "$a" "$rep" "${v:-NA}" "$IT" "$EN" "$T2" "$CS" "$EV" "$CM" >> "$outfile"
				echo "  rep$rep ${a} val=${v:-NA} spin=$(python3 -c "print('%.2fs' % ($IT*26e-9))") t2f=$T2 csb=$CS ev=$EV cap=$CM"
			done
		done
	done
}

echo "############################################################"
echo "# PHASE B -- POINT 11: all 6 benchmarks, pv vs 50us vs 100us"
echo "# reps=$BREPS mask=$MASK  -> $BOUT"
echo "############################################################"
exec 9>/var/lock/ivh_clean_check.lock
flock -w 7200 9 || { echo "FATAL: bench lock"; exit 1; }
run_set "$BOUT" "$BREPS" "pv 50 100" SET_P11
python3 "$T/ovn_report.py" "$BOUT" "POINT 11 -- 6 BENCHMARKS"

BEST=$(python3 "$T/ovn_best.py" "$BOUT")
echo
echo "############################################################"
echo "# PHASE C -- POINT 10: 12 workloads at the Phase B winner"
echo "# threshold=${BEST}us  reps=$CREPS  -> $COUT"
echo "############################################################"
run_set "$COUT" "$CREPS" "pv $BEST" SET_P10
python3 "$T/ovn_report.py" "$COUT" "POINT 10 -- 12 WORKLOADS @ ${BEST}us"

bash "$T/pvbase.sh" >/dev/null 2>&1
echo 22000 > $S/ivh_cs_noise_cycles
echo
echo "OVERNIGHT_DONE  p11=$BOUT  p10=$COUT  best=${BEST}us"
