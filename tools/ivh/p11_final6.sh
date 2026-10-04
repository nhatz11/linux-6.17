#!/bin/bash
# p11_final6.sh -- FINAL #11. stock PV vs FULL STACK (migration + AS) at 50us and
# 100us, six workloads, with parsec_dedup added as a candidate replacement for
# ebizzy (ebizzy's contention is mmap_lock, an rwsem the qspinlock counters
# cannot see: ~182s rwsem vs ~5s qspinlock, eval_final.md sec 9).
#
# DEDUP CAVEAT: recorded as unusable under uniform full contention -- PV-arm
# CV 82%, runs 15.2-231.5s. This script reports each workload's PV-arm CV so that
# verdict can be re-checked rather than assumed.
#
# Written as a NEW file; never edit a script with a running instance (that killed
# overnight.sh an hour after the edit).
#
# COLLECTION FIXES vs the previous runs:
#   - drop_caches is SKIPPED for the I/O / large-input workloads (dbench, vips,
#     dedup). Dropping it made every run cold, which is consistent but added
#     variance -- plausibly much of vips's 21% PV CV. memtier/hackbench/ebizzy
#     keep it since they are not input-bound.
#   - vips and dedup get a WARM-UP run before the first measured rep.
#   - hackbench uses its OWN timer (verified to agree with wall clock to 0.02%).
#
# SPIN: node_spin_iters * 26ns (spin_time_measurement.md); ratio exact regardless
# of the constant. NODE spin only; head ~20-27% uninstrumented.
#   THROUGHPUT: saved = spin_PV * (ops_AS/ops_PV) - spin_AS
#   TIME:       saved = spin_PV - spin_AS
set -u
T=/root/ivh_tools
S=/proc/sys/kernel
P=/root/parsec-benchmark
REPS="${REPS:-4}"
MASK="${MASK:-255}"
ARMS="${ARMS:-pv 50 100}"
OUT=/root/ivh_logs/p11final6_$(date +%m%d-%H%M%S).tsv
C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_slowpath_wait_events ivh_beat_tier2_fired ivh_cs_head_bailed ivh_evict_marked"

snap() { python3 "$T/read_ivh_counters.py" $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2); printf "%s ", $2}'; }
capmean() { awk 'NR>2 {s+=$11; n++} END {printf "%.0f", s/n}' /proc/ivh_cpu_stats; }
migs() { python3 "$T/migcount.py" 2>/dev/null || echo 0; }

arm() {
	if [ "$1" = pv ]; then
		bash "$T/pvbase.sh" >/dev/null 2>&1 || return 1
		[ "$(cat $S/ivh_pv_tier2_enable)" = 0 ] || return 1
		[ "$(cat $S/ivh_universal_eligible)" = 0 ] || return 1
	else
		IVH_MASK=$MASK bash "$T/p11_arm.sh" "$1" >/dev/null 2>&1 || return 1
		local cyc; cyc=$(python3 -c "print(int(round($1*2200)))")
		[ "$(cat $S/ivh_universal_eligible)" = 1 ] || return 1
		[ "$(cat $S/ivh_pv_tier2_enable)" = 1 ] || return 1
		[ "$(cat $S/ivh_pv_beat_threshold)" = "$cyc" ] || return 1
		[ "$(cat $S/ivh_cs_noise_cycles)" = "$cyc" ] || return 1
		[ "$(cat $S/ivh_pv_beat_publish_mask)" = "$MASK" ] || return 1
	fi
	sleep 1
}

MT="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=100 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"
DEDUP="$P/pkgs/kernels/dedup/inst/amd64-linux.gcc/bin/dedup -c -p -v -t 16 -i FC-6-x86_64-disc1.iso -o output.dat.ddp"
VIPS="IM_CONCURRENCY=16 $P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v"

# name|TYPE|dir|cmd|extractor|drop_caches(1/0)|warmup(1/0)
W=(
"hackbench_pipe_thr|TIME|/root|hackbench -T -g1 -f8 -l150000|grep -oP '^Time:\s*\K[0-9.]+'|1|0"
"memtier_memcached|THROUGHPUT|/root|$MT|grep -oP 'Totals\s+\K[0-9.]+'|1|0"
"dbench_16_noF|THROUGHPUT|/root|dbench -t 15 16 -D /root/dbench_test|grep -oP 'Throughput\s+\K[0-9.]+'|0|0"
"parsec_vips|TIME|$P/pkgs/apps/vips/run|$VIPS|WALL|0|1"
"ebizzy_mmap|THROUGHPUT|/root|/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304|grep -oP '^\K[0-9]+(?= records/s)'|1|1"
"parsec_dedup|TIME|$P/pkgs/kernels/dedup/run|$DEDUP|WALL|0|1"
)

exec 9>/var/lock/ivh_clean_check.lock
flock -w 3600 9 || { echo "FATAL: bench lock"; exit 1; }
printf "workload\ttype\tarm\trep\tvalue\titers\tentries\tt2f\tcsb\tev\tmigs\tcap\n" > "$OUT"
echo "### p11_final6: arms=[$ARMS] mask=$MASK reps=$REPS  cap_mean=$(capmean) -> $OUT"

for e in "${W[@]}"; do
	IFS='|' read -r n ty wd cmd ex dc wu <<< "$e"
	echo "########## $n [$ty] ##########"
	if [ "$wu" = 1 ]; then
		echo "  (warm-up run, not recorded)"
		( cd "$wd" && timeout 900 bash -c "$cmd" ) >/dev/null 2>&1
	fi
	for rep in $(seq 1 "$REPS"); do
		set -- $ARMS; K=$#; ORD=""
		for i in $(seq 0 $((K-1))); do eval "ORD=\"\$ORD \${$(( (i + rep - 1) % K + 1 ))}\""; done
		for a in $ORD; do
			arm "$a" || { echo "  ARMFAIL $a"; continue; }
			case "$n" in
			dbench_16_noF) rm -rf /root/dbench_test; mkdir -p /root/dbench_test ;;
			memtier_memcached)
				systemctl stop memcached >/dev/null 2>&1
				pkill -9 -x memcached 2>/dev/null; sleep 1
				memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=15 2>/dev/null 9>&-
				sleep 2 ;;
			esac
			sync
			[ "$dc" = 1 ] && echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
			sleep 1
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
			[ "$a" != pv ] && [ "$T2" -eq 0 ] && echo "  *** DEAD AS at $a: zero tier2"
			[ "$a" != pv ] && [ "$MG" -eq 0 ] && echo "  *** no migrations landed at $a"
			[ "$a" = pv ] && [ "$T2" -ne 0 ] && echo "  *** PV CONTAMINATED t2f=$T2"
			printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
				"$n" "$ty" "$a" "$rep" "${v:-NA}" "$IT" "$EN" "$T2" "$CS" "$EV" "$MG" "$CM" >> "$OUT"
			echo "  rep$rep $a val=${v:-NA} spin=$(python3 -c "print('%.2fs' % ($IT*26e-9))") t2f=$T2 migs=$MG cap=$CM"
		done
	done
	python3 "$T/p11_final6_report.py" "$OUT" "$n" || true
done

bash "$T/pvbase.sh" >/dev/null 2>&1
echo 22000 > $S/ivh_cs_noise_cycles
python3 "$T/p11_final6_report.py" "$OUT"
echo "P11FINAL6_DONE $OUT"
