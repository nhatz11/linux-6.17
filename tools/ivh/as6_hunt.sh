#!/bin/bash
# as6_hunt.sh -- hunt for ONE shared threshold where all 6 workloads save spin AND
# are perf neutral-or-positive. AS ONLY (migration OFF both arms) vs stock PV.
#
# Why AS-alone rather than migration+AS: migration alone COSTS ebizzy
# (ivh_ebizzy_migration_alone_is_negative), and the mig+AS run measured ebizzy at
# -16.05%. With migration off, ebizzy measured -0.09% (neutral). memtier is the
# mirror image. So AS-alone is the configuration with a chance of satisfying all
# six at once.
#
# Why 50us first: at cap~650 AS-alone at 50us was perf-POSITIVE on all four
# workloads then tested (hackbench +7.16, memtier +2.31, dbench +0.34, vips +4.99)
# AND spin-positive (ratios 0.694/0.267/0.922/0.620). 50us also won the threshold
# score. ARMS is overridable so the sweep can widen without editing this file.
#
# Threads/inputs are NOT changed from benchmarks.tsv -- a standing constraint.
# Every arm uses ONE value for all three mechanism thresholds (beat/evict/noise).
set -u
T=/root/ivh_tools; S=/proc/sys/kernel; P=/root/parsec-benchmark
REPS="${REPS:-5}"; MASK="${MASK:-255}"; ARMS="${ARMS:-pv 50}"
OUT=/root/ivh_logs/as6hunt_$(date +%m%d-%H%M%S).tsv
C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_slowpath_wait_events ivh_beat_tier2_fired ivh_cs_head_bailed ivh_evict_marked"
snap(){ python3 $T/read_ivh_counters.py $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2);printf "%s ",$2}'; }
capm(){ awk 'NR>2{s+=$11;n++} END{printf "%.0f", s/n}' /proc/ivh_cpu_stats; }
arm(){ if [ "$1" = pv ]; then
    bash $T/pvbase.sh >/dev/null 2>&1 || return 1
    [ "$(cat $S/ivh_pv_tier2_enable)" = 0 ] && [ "$(cat $S/ivh_universal_eligible)" = 0 ] || return 1
  else
    IVH_MASK=$MASK bash $T/p11_arm.sh "$1" >/dev/null 2>&1 || return 1
    echo 0 > $S/ivh_universal_eligible
    CYC=$(python3 -c "print(int(round($1*2200)))")
    [ "$(cat $S/ivh_universal_eligible)" = 0 ] || return 1
    [ "$(cat $S/ivh_pv_beat_threshold)" = "$CYC" ] || return 1
    [ "$(cat $S/ivh_pv_evict_threshold)" = "$CYC" ] || return 1
    [ "$(cat $S/ivh_cs_noise_cycles)" = "$CYC" ] || return 1
    [ "$(cat $S/ivh_pv_beat_publish_mask)" = "$MASK" ] || return 1
  fi; sleep 1; }
MT="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=100 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"
DD="$P/pkgs/kernels/dedup/inst/amd64-linux.gcc/bin/dedup -c -p -v -t 16 -i FC-6-x86_64-disc1.iso -o output.dat.ddp"
VI="IM_CONCURRENCY=16 $P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v"
W=("hackbench_pipe_thr|TIME|/root|hackbench -T -g1 -f8 -l150000|grep -oP '^Time:\s*\K[0-9.]+'|1|0"
   "memtier_memcached|THROUGHPUT|/root|$MT|grep -oP 'Totals\s+\K[0-9.]+'|1|0"
   "dbench_16_noF|THROUGHPUT|/root|dbench -t 15 16 -D /root/dbench_test|grep -oP 'Throughput\s+\K[0-9.]+'|0|0"
   "parsec_vips|TIME|$P/pkgs/apps/vips/run|$VI|WALL|0|1"
   "ebizzy_mmap|THROUGHPUT|/root|/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304|grep -oP '^\K[0-9]+(?= records/s)'|1|1"
   "parsec_dedup|TIME|$P/pkgs/kernels/dedup/run|$DD|WALL|0|1")
exec 9>/var/lock/ivh_clean_check.lock; flock -w 7200 9 || { echo "FATAL lock"; exit 1; }
printf "workload\ttype\tarm\trep\tvalue\titers\tentries\tt2f\tcsb\tev\tcap\n" > "$OUT"
echo "### as6_hunt arms=[$ARMS] mask=$MASK reps=$REPS cap=$(capm) -> $OUT"
for e in "${W[@]}"; do
  IFS='|' read -r n ty wd cmd ex dc wu <<< "$e"
  echo "########## $n [$ty] ##########"
  [ "$wu" = 1 ] && { echo "  (warm-up)"; ( cd "$wd" && timeout 900 bash -c "$cmd" ) >/dev/null 2>&1; }
  for rep in $(seq 1 $REPS); do
    set -- $ARMS; K=$#; ORD=""
    for i in $(seq 0 $((K-1))); do eval "ORD=\"\$ORD \${$(( (i + rep - 1) % K + 1 ))}\""; done
    for a in $ORD; do
      arm "$a" || { echo "  ARMFAIL $a"; continue; }
      case "$n" in
        dbench_16_noF) rm -rf /root/dbench_test; mkdir -p /root/dbench_test ;;
        memtier_memcached) systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null; sleep 1
          memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=15 2>/dev/null 9>&-; sleep 2 ;;
      esac
      sync; [ "$dc" = 1 ] && echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      CM=$(capm); b=($(snap)); t0=$(date +%s%N)
      out=$( ( cd "$wd" && timeout 900 bash -c "$cmd" ) 2>&1 9>&- ); t1=$(date +%s%N); f=($(snap))
      if [ "$ex" = WALL ]; then v=$(python3 -c "print(f'{($t1-$t0)/1e9:.3f}')")
      else v=$(echo "$out" | eval "$ex" 2>/dev/null | head -1); fi
      IT=$(( (${f[0]}-${b[0]}) + (${f[1]}-${b[1]}) )); EN=$(( ${f[2]}-${b[2]} ))
      T2=$(( ${f[3]}-${b[3]} )); CS=$(( ${f[4]}-${b[4]} )); EV=$(( ${f[5]}-${b[5]} ))
      [ "$a" != pv ] && [ "$T2" -eq 0 ] && echo "  *** DEAD $a: zero tier2"
      [ "$a" = pv ] && [ "$T2" -ne 0 ] && echo "  *** PV CONTAMINATED t2f=$T2"
      printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$n" "$ty" "$a" "$rep" "${v:-NA}" "$IT" "$EN" "$T2" "$CS" "$EV" "$CM" >> "$OUT"
      echo "  rep$rep $a val=${v:-NA} spin=$(python3 -c "print('%.2fs'%($IT*26e-9))") t2f=$T2 cap=$CM"
    done
  done
  python3 $T/as6_report.py "$OUT" "$n" || true
done
bash $T/pvbase.sh >/dev/null 2>&1; echo 22000 > $S/ivh_cs_noise_cycles
python3 $T/as6_report.py "$OUT"; echo "AS6HUNT_DONE $OUT"
