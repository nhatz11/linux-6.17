#!/bin/bash
# migas_vs_pv.sh -- FULL IVH STACK (migration + adaptive spinning) vs STOCK PV.
#
# Baseline: pvbase.sh = stock PV (every AS knob zeroed AND asserted, migration
# off). Tier 1 stays enabled there because it IS upstream pv_wait_early().
# Arm:      p11_arm.sh 50 @ mask 255, migration LEFT ON (universal_eligible=1).
#           This is the configuration the professor approved reporting against PV.
#
# Written fresh rather than edited from an existing script: editing a running
# script killed overnight.sh at line 162 an hour after the edit (bash reads the
# file incrementally).
#
# WAITS FOR THE CORUNNER TO REACH STEADY STATE. It was restarted and was still
# ramping (811 -> 915 -> 995 -> 1056% CPU, guest cap_mean 805 -> 781). Measuring
# through a ramp would put the early arms under lighter contention than the late
# ones, and AS's perf delta is contention-dependent.
#
# SPIN: node_spin_iters * 26ns (tools/bpf/docs/spin_time_measurement.md); the
# ratio between arms is exact regardless of the constant. NODE spin only -- head
# is ~20-27% of total and uninstrumented.
#   THROUGHPUT: saved = spin_PV * (ops_AS/ops_PV) - spin_AS
#   TIME:       saved = spin_PV - spin_AS
set -u
T=/root/ivh_tools
S=/proc/sys/kernel
P=/root/parsec-benchmark
HOST="sshpass -p "$IVH_HOST_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8 "$IVH_HOST""
REPS="${REPS:-4}"
THR="${THR:-50}"
MASK="${MASK:-255}"
OUT=/root/ivh_logs/migas_$(date +%m%d-%H%M%S).tsv
C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_slowpath_wait_events ivh_beat_tier2_fired ivh_cs_head_bailed ivh_evict_marked"

snap() { python3 "$T/read_ivh_counters.py" $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2); printf "%s ", $2}'; }
capmean() { awk 'NR>2 {s+=$11; n++} END {printf "%.0f", s/n}' /proc/ivh_cpu_stats; }
migs() { python3 "$T/migcount.py" 2>/dev/null || echo 0; }

# --- wait for the corunner to stop ramping -------------------------------------
echo "### waiting for corunner steady state (cap_mean stable within 2% over 3 samples)"
prev=0; stable=0
for i in $(seq 1 40); do
	cur=$(capmean)
	if [ "$prev" -gt 0 ]; then
		d=$(python3 -c "print(1 if abs($cur-$prev)/max($prev,1) < 0.02 else 0)")
		[ "$d" = 1 ] && stable=$((stable+1)) || stable=0
	fi
	echo "  sample $i: cap_mean=$cur  stable_run=$stable"
	[ "$stable" -ge 3 ] && break
	prev=$cur
	sleep 30
done
echo "### proceeding at cap_mean=$(capmean)"

arm() {
	if [ "$1" = pv ]; then
		bash "$T/pvbase.sh" >/dev/null 2>&1 || return 1
		[ "$(cat $S/ivh_pv_tier2_enable)" = 0 ] || return 1
		[ "$(cat $S/ivh_universal_eligible)" = 0 ] || return 1
	else
		# p11_arm.sh turns migration ON and sets all the AS knobs; we LEAVE
		# migration on -- that is the point of this arm.
		IVH_MASK=$MASK bash "$T/p11_arm.sh" "$THR" >/dev/null 2>&1 || return 1
		local cyc; cyc=$(python3 -c "print(int(round($THR*2200)))")
		[ "$(cat $S/ivh_universal_eligible)" = 1 ] || { echo "  MIG GATE SHUT"; return 1; }
		[ "$(cat $S/ivh_preempt_event_source)" = 2 ] || return 1
		[ "$(cat $S/ivh_pv_tier2_enable)" = 1 ] || return 1
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
		pkill -9 -x memcached 2>/dev/null; sleep 1
		memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=15 2>/dev/null 9>&-
		sleep 2 ;;
	ebizzy_mmap) ( cd /root && /home/nick/Desktop/ebizzy -S 3 -t 16 -m -s 4194304 >/dev/null 2>&1 ) ;;
	esac
}

MT="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=100 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"
W=(
"hackbench_pipe_thr|TIME|/root|hackbench -T -g1 -f8 -l150000|WALL"
"memtier_memcached|THROUGHPUT|/root|$MT|grep -oP 'Totals\s+\K[0-9.]+'"
"dbench_16_noF|THROUGHPUT|/root|dbench -t 15 16 -D /root/dbench_test|grep -oP 'Throughput\s+\K[0-9.]+'"
"parsec_vips|TIME|$P/pkgs/apps/vips/run|IM_CONCURRENCY=16 $P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v|WALL"
"ebizzy_mmap|THROUGHPUT|/root|/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304|grep -oP '^\K[0-9]+(?= records/s)'"
)

exec 9>/var/lock/ivh_clean_check.lock
flock -w 3600 9 || { echo "FATAL: bench lock"; exit 1; }
printf "workload\ttype\tarm\trep\tvalue\titers\tentries\tt2f\tcsb\tev\tmigs\tcap\n" > "$OUT"
echo "### migration+AS(${THR}us, mask $MASK) vs stock PV, reps=$REPS -> $OUT"

for e in "${W[@]}"; do
	IFS='|' read -r n ty wd cmd ex <<< "$e"
	echo "########## $n [$ty] ##########"
	for rep in $(seq 1 "$REPS"); do
		case $((rep % 2)) in 1) ORD="pv as" ;; 0) ORD="as pv" ;; esac
		for a in $ORD; do
			arm "$a" || { echo "  ARMFAIL $a"; continue; }
			prep "$n"
			sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
			CM=$(capmean); M0=$(migs); b=($(snap)); t0=$(date +%s%N)
			out=$( ( cd "$wd" && timeout 900 bash -c "$cmd" ) 2>&1 9>&- )
			t1=$(date +%s%N); f=($(snap)); M1=$(migs)
			if [ "$ex" = WALL ]; then
				v=$(python3 -c "print(f'{($t1-$t0)/1e9:.3f}')")
			else
				v=$(echo "$out" | eval "$ex" 2>/dev/null | head -1)
			fi
			IT=$(( (${f[0]} - ${b[0]}) + (${f[1]} - ${b[1]}) ))
			EN=$(( ${f[2]} - ${b[2]} )); T2=$(( ${f[3]} - ${b[3]} ))
			CS=$(( ${f[4]} - ${b[4]} )); EV=$(( ${f[5]} - ${b[5]} )); MG=$(( M1 - M0 ))
			[ "$a" = as ] && [ "$T2" -eq 0 ] && echo "  *** DEAD AS: zero tier2"
			[ "$a" = as ] && [ "$MG" -eq 0 ] && echo "  *** NO MIGRATIONS LANDED"
			[ "$a" = pv ] && [ "$T2" -ne 0 ] && echo "  *** PV CONTAMINATED t2f=$T2"
			printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
				"$n" "$ty" "$a" "$rep" "${v:-NA}" "$IT" "$EN" "$T2" "$CS" "$EV" "$MG" "$CM" >> "$OUT"
			echo "  rep$rep $a val=${v:-NA} spin=$(python3 -c "print('%.2fs' % ($IT*26e-9))") t2f=$T2 migs=$MG cap=$CM"
		done
	done
done

bash "$T/pvbase.sh" >/dev/null 2>&1
echo 22000 > $S/ivh_cs_noise_cycles
python3 "$T/migas_report.py" "$OUT"
echo "MIGAS_DONE $OUT"
