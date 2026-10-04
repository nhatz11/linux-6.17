#!/bin/bash
# cleanspin.sh -- measure NODE + COUNTED-HEAD spin iterations: the only metric
# available today that covers both loops AND excludes stolen time.
#
# The problem it solves: `wait_ns - halt_ns` has complete scope but is
# contaminated -- it charges to "spin" every nanosecond the vCPU is preempted
# inside the spin loop, and AS raises host-side runqueue wait +9.35% (8/8 reps)
# because each halt yields a pinned core and must requeue behind the corunner.
# So that metric is biased AGAINST AS by construction. `node_spin_iters` is
# unbiased but blind to the head loop, which is 1.4-3.2x node attempts.
#
# This metric: node_spin_iters (both exits, GLOCK-11 complete)
#            + ivh_head_spin_iters_sum      (head exhaustion, :3680)
#            + ivh_head_spin_iters_bail_sum (head AS bails,   :3677)
# All exact iteration counts. The only omission is the head `goto gotlock`
# success path (:3846), excluded from BOTH arms, so the comparison is a
# conservative LOWER BOUND on the true reduction.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
REPS="${REPS:-5}"
OUT=/root/ivh_logs/cleanspin_$(date +%m%d-%H%M%S).tsv
C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_head_spin_iters_sum ivh_head_spin_iters_bail_sum ivh_slowpath_wait_events ivh_head_spin_enter ivh_beat_tier2_fired ivh_slowpath_wait_ns ivh_slowpath_halt_ns"
snap(){ python3 $T/read_ivh_counters.py $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2);printf "%s ",$2}'; }
arm(){ if [ "$1" = pv ]; then bash $T/pvbase.sh >/dev/null 2>&1 || return 1
       else IVH_MASK=255 bash $T/p11_arm.sh 50 >/dev/null 2>&1 || return 1
            echo 0 > $S/ivh_universal_eligible
            [ "$(cat $S/ivh_universal_eligible)" = 0 ] || return 1; fi; sleep 1; }
P=/root/parsec-benchmark
exec 9>/var/lock/ivh_clean_check.lock; flock -w 3600 9 || { echo FATAL; exit 1; }
printf "wl\tarm\trep\tval\tnode\thead_cnt\tentries\thead_ten\tt2f\twait\thalt\n" > "$OUT"
echo "### cleanspin n=$REPS -> $OUT"
for wl in hackbench dbench ebizzy vips memtier; do
 echo "########## $wl ##########"
 [ "$wl" = ebizzy ] && ( cd /root && /home/nick/Desktop/ebizzy -S 3 -t 16 -m -s 4194304 >/dev/null 2>&1 )
 [ "$wl" = vips ] && ( cd $P/pkgs/apps/vips/run && IM_CONCURRENCY=16 $P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v >/dev/null 2>&1 )
 for rep in $(seq 1 $REPS); do
  case $((rep % 2)) in 1) O="pv as";; 0) O="as pv";; esac
  for a in $O; do
   arm "$a" || { echo "  ARMFAIL"; continue; }
   # A memcached left running by the memtier block adds load to every later
   # workload: it dropped cap_mean 746 -> 565 and doubled hackbench (19s -> 41s).
   # Stop it for every workload that is not memtier.
   [ "$wl" != memtier ] && { systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null; }
   case $wl in
    memtier) systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null; sleep 1
      memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=15 2>/dev/null 9>&-; sleep 2 ;;
    dbench) rm -rf /root/dbench_test; mkdir -p /root/dbench_test ;;
   esac
   sync; case $wl in hackbench|memtier|ebizzy) echo 3 > /proc/sys/vm/drop_caches 2>/dev/null;; esac; sleep 1
   b=($(snap))
   case $wl in
    hackbench) v=$( timeout 180 hackbench -T -g1 -f8 -l150000 2>&1 | grep -oP '^Time:\s*\K[0-9.]+' | head -1 ) ;;
    memtier) v=$( timeout 120 /root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=100 --ratio=1:0 -d 32 --test-time=10 --hide-histogram 2>&1 | grep -oP 'Totals\s+\K[0-9.]+' | head -1 ) ;;
    dbench) v=$( timeout 180 dbench -t 15 16 -D /root/dbench_test 2>&1 | grep -oP 'Throughput\s+\K[0-9.]+' | head -1 ) ;;
    ebizzy) v=$( cd /root && timeout 120 /home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304 2>&1 | grep -oP '^\K[0-9]+(?= records/s)' | head -1 ) ;;
    vips) t0=$(date +%s%N); ( cd $P/pkgs/apps/vips/run && IM_CONCURRENCY=16 $P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v >/dev/null 2>&1 ); t1=$(date +%s%N); v=$(python3 -c "print(f'{($t1-$t0)/1e9:.3f}')") ;;
   esac
   f=($(snap))
   printf "%s\t%s\t%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n" "$wl" "$a" "$rep" "${v:-NA}" \
     "$(( (${f[0]}-${b[0]}) + (${f[1]}-${b[1]}) ))" "$(( (${f[2]}-${b[2]}) + (${f[3]}-${b[3]}) ))" \
     "$(( ${f[4]}-${b[4]} ))" "$(( ${f[5]}-${b[5]} ))" "$(( ${f[6]}-${b[6]} ))" \
     "$(( ${f[7]}-${b[7]} ))" "$(( ${f[8]}-${b[8]} ))" >> "$OUT"
   echo "  rep$rep $a $wl val=${v:-NA}"
  done
 done
done
systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null
bash $T/pvbase.sh >/dev/null 2>&1; echo 22000 > $S/ivh_cs_noise_cycles
python3 $T/cleanspin_report.py "$OUT"
echo "CLEANSPIN_DONE $OUT"
