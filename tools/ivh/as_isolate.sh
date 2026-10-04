#!/bin/bash
# as_isolate.sh -- ISOLATE adaptive spinning from migration.
#
# THE BUG THIS FIXES: p11_suite.sh and as_split_vs_unified.sh compared
#   pvbase (migration OFF, spin_mode 1)  vs  p11_arm (migration ON + AS ON)
# so every "spin went up 15%" was migration+AS, not AS. On ebizzy migration
# ALONE cuts spin 1.894 -> 0.856 s (-55%), so the reference was wrong by more
# than the effect being measured.
#
# Correct reference for AS is MIGRATION-ONLY:
#   pv      pvbase.sh                      (no mig, no AS)   -- context only
#   mig     p7v2_arm.sh 2500000            (mig only)        <-- THE reference
#   as500   p11_arm.sh 500   (mig + AS, all 3 TSC knobs = 500us)
#   as1500  p11_arm.sh 1500  (mig + AS, all 3 TSC knobs = 1500us)
# Delta attributable to AS = spin(as) - spin(mig).
set -u
T=/root/ivh_tools
exec 9>/var/lock/ivh_clean_check.lock; flock -n 9 || { echo "FATAL: lock held"; exit 1; }
REPS="${1:-2}"
C="ivh_slowpath_wait_ns ivh_slowpath_halt_ns ivh_slowpath_wait_events ivh_halt_from_node ivh_beat_tier2_checked ivh_beat_tier2_fired ivh_cs_head_bailed ivh_evict_marked"
OUT=/root/ivh_logs/as_iso_$(date +%m%d-%H%M%S).tsv
printf "workload\tmetric\tarm\trep\tvalue\tspin_ns\thalt_ns\twait_ev\tnodehalt\tt2chk\tt2fire\tcsbail\tevict\n" > "$OUT"
snap(){ python3 $T/read_ivh_counters.py $C 2>/dev/null | awk -F= '{gsub(/ /,"",$1);gsub(/ /,"",$2);printf "%s=%s ",$1,$2}'; }
gv(){ echo "$1" | grep -oP "$2=\K[0-9]+" | head -1; }
arm(){ case $1 in
  pv)     bash $T/pvbase.sh    >/dev/null 2>&1 ;;
  mig)    bash $T/p7v2_arm.sh 2500000 >/dev/null 2>&1 ;;
  as500)  bash $T/p11_arm.sh 500  >/dev/null 2>&1 ;;
  as1500) bash $T/p11_arm.sh 1500 >/dev/null 2>&1 ;;
esac; sleep 1; }
MT="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=100 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"
W=(
"hackbench_pipe_thr|TIME|hackbench -T -g1 -f8 -l150000|grep -oP '^Time:\s*\K[0-9.]+'"
"ebizzy_mmap|THROUGHPUT|/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304|grep -oP '^\K[0-9]+(?= records/s)'"
"dbench_16_noF|THROUGHPUT|dbench -t 15 16 -D /root/dbench_test|grep -oP 'Throughput\s+\K[0-9.]+'"
"memtier_memcached|THROUGHPUT|MT|grep -oP 'Totals\s+\K[0-9.]+'"
)
for e in "${W[@]}"; do
  IFS='|' read -r n m c x <<< "$e"
  [ "$c" = MT ] && c="$MT"
  echo "########## $n ##########"
  for rep in $(seq 1 $REPS); do
    for a in pv mig as500 as1500; do
      arm $a
      case "$n" in
        dbench_16_noF) rm -rf /root/dbench_test; mkdir -p /root/dbench_test ;;
        memtier_memcached) systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null; sleep 1
                           memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=15 2>/dev/null 9>&-; sleep 2 ;;
      esac
      sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      b=$(snap); out=$( timeout 600 bash -c "$c" 2>&1 9>&- ); f=$(snap)
      v=$(echo "$out" | eval $x | head -1)
      D(){ echo $(( $(gv "$f" $1) - $(gv "$b" $1) )); }
      printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$n" "$m" "$a" "$rep" "${v:-NA}" \
        "$(( $(D ivh_slowpath_wait_ns) - $(D ivh_slowpath_halt_ns) ))" "$(D ivh_slowpath_halt_ns)" \
        "$(D ivh_slowpath_wait_events)" "$(D ivh_halt_from_node)" "$(D ivh_beat_tier2_checked)" \
        "$(D ivh_beat_tier2_fired)" "$(D ivh_cs_head_bailed)" "$(D ivh_evict_marked)" >> "$OUT"
      echo "  rep$rep $a = ${v:-NA}  spin=$(D ivh_slowpath_wait_ns)-$(D ivh_slowpath_halt_ns)  t2=$(D ivh_beat_tier2_fired) evict=$(D ivh_evict_marked)"
    done
  done
done
echo "AS_ISO_DONE $OUT"
