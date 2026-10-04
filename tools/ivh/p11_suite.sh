#!/bin/bash
# p11_suite.sh [reps] -- point 11: ONE staleness threshold for all mechanisms,
# swept 50/100/200/400/800/1500 us, vs a stock-PV reference, over six workloads.
#
# Six workloads per the user's 2026-10-03 choice (NOT the old 15.3 set):
#   memtier, hackbench, ebizzy, dbench (NO -F), vips -- FIVE kernel-lock
#   workloads. nhextend-fin is DEFERRED: its userspace AFL lock has its own
#   spin-before-sleep threshold, so it needs an extra swept parameter and the
#   IVH_AFL_DISABLE=1 control is invalid for an AS arm.
# dbench drops -F deliberately: with -F it is 46% iowait and migration-inert.
#
# ivh_time_left_threshold_ns is HELD at 2.5 ms for every arm so the ONLY thing
# varying is the staleness number.
#
# Fire counters are captured per run. Per ivh_bypass_tier2_share_dead_threshold,
# enable=1 proves nothing -- a mechanism can be on and fire zero.
set -u
T=/root/ivh_tools
exec 9>/var/lock/ivh_clean_check.lock; flock -n 9 || { echo "FATAL: lock held"; exit 1; }
source $T/suite12.sh
REPS="${1:-2}"
ARMS="pv 50 100 200 400 800 1500"
OUT=/root/ivh_logs/p11_$(date +%m%d-%H%M%S).tsv
CTRS="ivh_beat_tier1_fired ivh_beat_tier2_checked ivh_beat_tier2_fired ivh_cs_head_bailed ivh_evict_marked ivh_evict_requeued ivh_rot_splice_done ivh_slowpath_wait_ns ivh_slowpath_halt_ns"
printf "workload\tmetric\tarm_us\trep\tvalue\tt1_fired\tt2_checked\tt2_fired\tcs_bail\tevict_mark\tevict_req\tsplice\tspin_ns\twait_ns\thalt_ns\n" > "$OUT"

W6=(
"memtier_memcached|/root|THROUGHPUT|MEMTIER_CMD|grep -oP 'Totals\s+\K[0-9.]+'"
"hackbench_pipe_thr|/root|TIME|hackbench -T -g1 -f8 -l150000|grep -oP '^Time:\s*\K[0-9.]+'"
"ebizzy_mmap|/root|THROUGHPUT|/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304|grep -oP '^\K[0-9]+(?= records/s)'"
"dbench_16_noF|/root|THROUGHPUT|dbench -t 15 16 -D /root/dbench_test|grep -oP 'Throughput\s+\K[0-9.]+'"
"parsec_vips|/root/parsec-benchmark/pkgs/apps/vips/run|TIME|IM_CONCURRENCY=16 /root/parsec-benchmark/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v|x"
)
snap(){ python3 $T/read_ivh_counters.py $CTRS 2>/dev/null | awk -F= '{gsub(/ /,"",$1);gsub(/ /,"",$2);printf "%s=%s ",$1,$2}'; }
gv(){ echo "$1" | grep -oP "$2=\K[0-9]+" | head -1; }

echo "### p11: arms=[$ARMS] us  reps=$REPS  -> $OUT"
for e in "${W6[@]}"; do
  IFS='|' read -r n d m c x <<< "$e"
  [ "$c" = MEMTIER_CMD ] && c="$MEMTIER_CMD"
  echo "########## $n [$m] ##########"
  for rep in $(seq 1 $REPS); do
    for a in $ARMS; do
      if [ "$a" = pv ]; then bash $T/pvbase.sh >/dev/null 2>&1 || { echo "ARMFAIL pv"; continue; }
      else bash $T/p11_arm.sh "$a" >/dev/null 2>&1 || { echo "ARMFAIL $a"; continue; }; fi
      sleep 1
      case "$n" in
        dbench_16_noF) rm -rf /root/dbench_test; mkdir -p /root/dbench_test ;;
        memtier_memcached) prep12 memtier_memcached >/dev/null 2>&1 9>&- || { echo " PREPFAIL"; continue; } ;;
      esac
      sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      b4=$(snap); t0=$(date +%s%N)
      out=$( ( cd "$d" && timeout 900 bash -c "$c" ) 2>&1 9>&- ); t1=$(date +%s%N)
      af=$(snap)
      if [ "$x" = x ]; then v=$(python3 -c "print(f'{($t1-$t0)/1e9:.3f}')"); else v=$(echo "$out" | eval $x | head -1); fi
      D(){ echo $(( $(gv "$af" $1) - $(gv "$b4" $1) )); }
      printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$n" "$m" "$a" "$rep" "${v:-NA}" \
        "$(D ivh_beat_tier1_fired)" "$(D ivh_beat_tier2_checked)" "$(D ivh_beat_tier2_fired)" \
        "$(D ivh_cs_head_bailed)" "$(D ivh_evict_marked)" "$(D ivh_evict_requeued)" "$(D ivh_rot_splice_done)" \
        "$(( $(D ivh_slowpath_wait_ns) - $(D ivh_slowpath_halt_ns) ))" \
        "$(D ivh_slowpath_wait_ns)" "$(D ivh_slowpath_halt_ns)" >> "$OUT"
      echo "  rep$rep ${a}us = ${v:-NA}  t2fire=$(D ivh_beat_tier2_fired)/$(D ivh_beat_tier2_checked) csbail=$(D ivh_cs_head_bailed) evict=$(D ivh_evict_marked)"
    done
  done
  python3 $T/p11_report.py "$OUT" "$n" 2>/dev/null || true
done
echo "P11_DONE $OUT"
