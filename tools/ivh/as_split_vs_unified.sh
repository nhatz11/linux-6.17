#!/bin/bash
# as_split_vs_unified.sh -- is the SPLIT config (beat HIGH / evict LOW) the
# reason AS "always helps spintime", and did forcing one number break it?
#
#   split  : beat_threshold 5 ms (11,000,000 cyc)  evict_threshold 500 us (1,100,000 cyc)
#            = the shipped G-LOCK-47 values, which the kernel says are the only
#              pair that satisfies both signals' own floors.
#   uni500 : BOTH at 500 us   -- what p11 swept (beat far below its 3.2 ms floor)
#   uni5m  : BOTH at 5 ms     -- eviction known-dead there (0 fires at 5 ms)
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
exec 9>/var/lock/ivh_clean_check.lock; flock -n 9 || { echo "FATAL: lock held"; exit 1; }
REPS="${1:-2}"
C="ivh_slowpath_wait_ns ivh_slowpath_halt_ns ivh_halt_from_node ivh_beat_tier2_checked ivh_beat_tier2_fired ivh_cs_head_bailed ivh_evict_marked"
OUT=/root/ivh_logs/as_split_$(date +%m%d-%H%M%S).tsv
printf "workload\tmetric\tarm\trep\tvalue\tspin_ns\thalt_ns\tnodehalt\tt2chk\tt2fire\tcsbail\tevict\n" > "$OUT"
snap(){ python3 $T/read_ivh_counters.py $C 2>/dev/null | awk -F= '{gsub(/ /,"",$1);gsub(/ /,"",$2);printf "%s=%s ",$1,$2}'; }
gv(){ echo "$1" | grep -oP "$2=\K[0-9]+" | head -1; }
arm(){ case $1 in
  pv)     bash $T/pvbase.sh >/dev/null 2>&1 ;;
  split)  bash $T/p11_arm.sh 500 >/dev/null 2>&1; echo 11000000 > $S/ivh_pv_beat_threshold ;;
  uni500) bash $T/p11_arm.sh 500 >/dev/null 2>&1 ;;
  uni5m)  bash $T/p11_arm.sh 5000 >/dev/null 2>&1 ;;
esac; sleep 1; }
W=(
"memtier_memcached|THROUGHPUT|/root|MT|grep -oP 'Totals\s+\K[0-9.]+'"
"ebizzy_mmap|THROUGHPUT|/root|/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304|grep -oP '^\K[0-9]+(?= records/s)'"
"dbench_16_noF|THROUGHPUT|/root|dbench -t 15 16 -D /root/dbench_test|grep -oP 'Throughput\s+\K[0-9.]+'"
"hackbench_pipe_thr|TIME|/root|hackbench -T -g1 -f8 -l150000|grep -oP '^Time:\s*\K[0-9.]+'"
)
MT="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=100 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"
source $T/suite12.sh
for e in "${W[@]}"; do
  IFS='|' read -r n m d c x <<< "$e"
  [ "$c" = MT ] && c="$MT"
  echo "########## $n ##########"
  for rep in $(seq 1 $REPS); do
    for a in pv split uni500 uni5m; do
      arm $a
      case "$n" in
        dbench_16_noF) rm -rf /root/dbench_test; mkdir -p /root/dbench_test ;;
        memtier_memcached) prep12 memtier_memcached >/dev/null 2>&1 9>&- || continue ;;
      esac
      sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      b=$(snap)
      out=$( ( cd "$d" && timeout 600 bash -c "$c" ) 2>&1 9>&- )
      f=$(snap)
      v=$(echo "$out" | eval $x | head -1)
      D(){ echo $(( $(gv "$f" $1) - $(gv "$b" $1) )); }
      printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$n" "$m" "$a" "$rep" "${v:-NA}" \
        "$(( $(D ivh_slowpath_wait_ns) - $(D ivh_slowpath_halt_ns) ))" "$(D ivh_slowpath_halt_ns)" \
        "$(D ivh_halt_from_node)" "$(D ivh_beat_tier2_checked)" "$(D ivh_beat_tier2_fired)" \
        "$(D ivh_cs_head_bailed)" "$(D ivh_evict_marked)" >> "$OUT"
      echo "  rep$rep $a = ${v:-NA}  spin=$(python3 -c "print(f'{$(D ivh_slowpath_wait_ns)-$(D ivh_slowpath_halt_ns):.0f}')")ns t2=$(D ivh_beat_tier2_fired)/$(D ivh_beat_tier2_checked) evict=$(D ivh_evict_marked)"
    done
  done
done
echo "AS_SPLIT_DONE $OUT"
