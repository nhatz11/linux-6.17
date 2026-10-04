#!/bin/bash
# p11_probe2d.sh -- publish mask x staleness threshold on the three workloads that
# have resisted: memtier, dbench_noF, ebizzy.
#
# POST-AUDIT (2026-10-03). The arm script now sets ivh_cs_noise_cycles (the REAL
# head-early-halt gate at criterion=1; ivh_cs_tick_period is dead there and was
# leaving HEH pinned at 550000 cyc / 250us in every previous arm), asserts the
# publish mask (the kernel silently refuses <255), and sets evict_cheap_now=0 so
# eviction's clock is rdtsc rather than a stamp lagging by one publish interval.
#
# PRIMARY OUTCOME IS PER-MECHANISM FIRE COUNTS, not a single spin number: the
# mechanisms have incompatible threshold requirements (tier2 needs T >> publish
# interval, eviction peaks at T ~ publish interval), so each arm can satisfy at
# most one. Arms chosen to span that: 255:100 (eviction-friendlier),
# 255:400 (tier2-friendly, eviction dead), 1023:200 (middle), 4095:400 (shipped).
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
exec 9>/var/lock/ivh_clean_check.lock; flock -w 600 9 || { echo "FATAL: lock"; exit 1; }
REPS="${REPS:-3}"
ARMS="${ARMS:-mig 255:100 255:400 1023:200 4095:400}"
OUT=/root/ivh_logs/probe2d_$(date +%m%d-%H%M%S).tsv
C="ivh_slowpath_wait_ns ivh_slowpath_halt_ns ivh_beat_tier1_fired ivh_beat_tier2_fired ivh_beat_tier2_checked ivh_head_bypass_fired ivh_cs_head_bailed ivh_evict_marked"
printf "workload\tmetric\tarm\tmask\tthr\trep\tvalue\tspin_ns\tt1\tt2f\tt2c\thbf\tcsb\tev\tcap\n" > "$OUT"
snap(){ python3 $T/read_ivh_counters.py $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2);printf "%s ",$2}'; }
capm(){ awk 'NR>2{s+=$11;n++} END{printf "%.0f", s/n}' /proc/ivh_cpu_stats; }
arm(){ bash $T/pvbase.sh >/dev/null 2>&1
  if [ "$1" = mig ]; then
    bash $T/p7v2_arm.sh 2500000 >/dev/null 2>&1 || return 1
    for k in ivh_pv_tier2_enable ivh_head_bypass_enable ivh_head_bypass_probe ivh_cs_head_bail ivh_pv_evict_enable; do echo 0 > $S/$k; done
    [ "$(cat $S/ivh_pv_tier1_enable)" = 1 ] || return 1
  else
    IVH_MASK=${1%%:*} bash $T/p11_arm.sh ${1##*:} >/dev/null 2>&1 || return 1
  fi; sleep 1; }
MT="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=100 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"
W=("memtier_memcached|THROUGHPUT|MT|grep -oP 'Totals\s+\K[0-9.]+'"
   "dbench_16_noF|THROUGHPUT|dbench -t 15 16 -D /root/dbench_test|grep -oP 'Throughput\s+\K[0-9.]+'"
   "ebizzy_mmap|THROUGHPUT|/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304|grep -oP '^\K[0-9]+(?= records/s)'")
echo "### probe2d arms=[$ARMS] reps=$REPS -> $OUT"
for e in "${W[@]}"; do
  IFS='|' read -r n m c x <<< "$e"; [ "$c" = MT ] && c="$MT"
  echo "########## $n ##########"
  for rep in $(seq 1 $REPS); do
    set -- $ARMS; K=$#; ORD=""
    for i in $(seq 0 $((K-1))); do eval "ORD=\"\$ORD \${$(( (i + rep - 1) % K + 1 ))}\""; done
    for a in $ORD; do
      arm "$a" || { echo "  ARMFAIL $a"; continue; }
      case "$n" in
        dbench_16_noF) rm -rf /root/dbench_test; mkdir -p /root/dbench_test ;;
        memtier_memcached) systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null; sleep 1
          memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=15 2>/dev/null 9>&-; sleep 2 ;;
        ebizzy_mmap) ( /home/nick/Desktop/ebizzy -S 3 -t 16 -m -s 4194304 >/dev/null 2>&1 ) ;;  # warm-up: required
      esac
      sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      CM=$(capm); b=($(snap))
      out=$( bash -c "$c" 2>&1 9>&- ); f=($(snap))
      v=$(echo "$out" | eval $x | head -1)
      MK=mig; TU=0; [ "$a" != mig ] && { MK=${a%%:*}; TU=${a##*:}; }
      printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$n" "$m" "$a" "$MK" "$TU" "$rep" "${v:-NA}" \
        "$(( (${f[0]}-${b[0]}) - (${f[1]}-${b[1]}) ))" "$(( ${f[2]}-${b[2]} ))" "$(( ${f[3]}-${b[3]} ))" \
        "$(( ${f[4]}-${b[4]} ))" "$(( ${f[5]}-${b[5]} ))" "$(( ${f[6]}-${b[6]} ))" "$(( ${f[7]}-${b[7]} ))" "$CM" >> "$OUT"
      echo "  rep$rep $a = ${v:-NA}  t2f=$(( ${f[3]}-${b[3]} )) hbf=$(( ${f[5]}-${b[5]} )) csb=$(( ${f[6]}-${b[6]} )) ev=$(( ${f[7]}-${b[7]} ))"
    done
  done
done
python3 $T/probe2d_report.py "$OUT" || true
echo "PROBE2D_DONE $OUT"
