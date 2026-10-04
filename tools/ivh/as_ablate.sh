#!/bin/bash
# as_ablate.sh -- which mechanism causes the spin REGRESSION on ebizzy/memtier?
# Build up from migration-only, one mechanism at a time, all TSC knobs at 500us.
#
# Suspect: ivh_cs_head_bail (head early halt). It halts the QUEUE HEAD -- the
# thread about to acquire. A false positive means the lock frees while the head
# sleeps and the whole queue waits for a wakeup. On ebizzy it is the only
# mechanism firing in volume (1240 bails; tier2 0.14%, evict 0) and ebizzy has
# the worst regression (-18.9% spin). Its precision figure is WITHDRAWN per
# ivh_cs_stamp_is_the_detector.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
exec 9>/var/lock/ivh_clean_check.lock; flock -n 9 || { echo "FATAL: lock held"; exit 1; }
REPS="${1:-2}"; US=500; CYC=$((US*2200))
C="ivh_slowpath_wait_ns ivh_slowpath_halt_ns ivh_beat_tier2_fired ivh_beat_tier2_checked ivh_cs_head_bailed ivh_evict_marked"
OUT=/root/ivh_logs/as_abl_$(date +%m%d-%H%M%S).tsv
printf "workload\tmetric\tarm\trep\tvalue\tspin_ns\thalt_ns\tt2fire\tt2chk\tcsbail\tevict\n" > "$OUT"
snap(){ python3 $T/read_ivh_counters.py $C 2>/dev/null | awk -F= '{gsub(/ /,"",$1);gsub(/ /,"",$2);printf "%s=%s ",$1,$2}'; }
gv(){ echo "$1" | grep -oP "$2=\K[0-9]+" | head -1; }
# every arm starts from the FULL stack then switches ONE thing off
arm(){
  case $1 in
    mig) bash $T/p7v2_arm.sh 2500000 >/dev/null 2>&1; sleep 1; return;;
  esac
  bash $T/p11_arm.sh $US >/dev/null 2>&1
  case $1 in
    full)      : ;;                                          # t1+t2+hb+heh+skip
    no_heh)    echo 0 > $S/ivh_cs_head_bail ;;               # drop head early halt
    no_hb)     echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_head_bypass_probe ;;
    no_skip)   echo 0 > $S/ivh_pv_evict_enable ;;
    no_t2)     echo 0 > $S/ivh_pv_tier2_enable ;;
    only_t1)   echo 0 > $S/ivh_cs_head_bail; echo 0 > $S/ivh_head_bypass_enable
               echo 0 > $S/ivh_head_bypass_probe; echo 0 > $S/ivh_pv_evict_enable
               echo 0 > $S/ivh_pv_tier2_enable ;;
  esac; sleep 1
}
MT="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=100 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"
W=(
"ebizzy_mmap|THROUGHPUT|/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304|grep -oP '^\K[0-9]+(?= records/s)'"
"memtier_memcached|THROUGHPUT|MT|grep -oP 'Totals\s+\K[0-9.]+'"
"hackbench_pipe_thr|TIME|hackbench -T -g1 -f8 -l150000|grep -oP '^Time:\s*\K[0-9.]+'"
)
for e in "${W[@]}"; do
  IFS='|' read -r n m c x <<< "$e"
  [ "$c" = MT ] && c="$MT"
  echo "########## $n ##########"
  for rep in $(seq 1 $REPS); do
    for a in mig full no_heh no_hb no_skip no_t2 only_t1; do
      arm $a
      [ "$n" = memtier_memcached ] && { systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null; sleep 1
        memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=15 2>/dev/null 9>&-; sleep 2; }
      sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      b=$(snap); out=$( timeout 600 bash -c "$c" 2>&1 9>&- ); f=$(snap)
      v=$(echo "$out" | eval $x | head -1)
      D(){ echo $(( $(gv "$f" $1) - $(gv "$b" $1) )); }
      printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$n" "$m" "$a" "$rep" "${v:-NA}" \
        "$(( $(D ivh_slowpath_wait_ns) - $(D ivh_slowpath_halt_ns) ))" "$(D ivh_slowpath_halt_ns)" \
        "$(D ivh_beat_tier2_fired)" "$(D ivh_beat_tier2_checked)" "$(D ivh_cs_head_bailed)" "$(D ivh_evict_marked)" >> "$OUT"
      echo "  rep$rep $a = ${v:-NA}  csbail=$(D ivh_cs_head_bailed) t2=$(D ivh_beat_tier2_fired)"
    done
  done
done
echo "AS_ABL_DONE $OUT"
