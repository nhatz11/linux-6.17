#!/bin/bash
# p11_sub1ms.sh -- find a SUB-1ms shared threshold that is spin-neutral-or-better
# and perf-neutral-or-better, referenced to MIGRATION + TIER 1.
#
# WHY SPIN CAN RISE AT ALL (the paradox): pv_wait_node's OUTER for(;;)
# (qspinlock_paravirt.h:1907) re-enters the inner loop with `loop = threshold`
# RESET. So an early bail does not truncate the wait -- it RESTARTS the spin.
# One slowpath entry can contain several spin->halt->wake->spin cycles, so
# spin-per-entry rises even though each individual pass got shorter.
#
# THEREFORE the unconfounded metric is ITERS PER SPIN PASS:
#   (ivh_node_spin_iters_sum + ivh_node_spin_success_iters_sum)
#   / (ivh_node_spin_attempts + ivh_node_spin_success_attempts)
# That is immune to BOTH confounds: the population shift eviction causes
# (entries up) and the survivorship effect (entries down 35% on hackbench).
# mig+t1 reference measures 521 iters/pass.
#
# THE NEW LEVER: ivh_pv_beat_publish_mask. At 4095 the stamp refreshes every
# 4096 spin iters (~50us), so a 200us threshold is only 4 publish intervals of
# margin -- trigger-happy. At 255 it refreshes ~16x more often, making the same
# threshold far more precise. Fewer false positives -> halts are productive ->
# fewer re-spins -> total spin falls. Swept as the second axis.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
exec 9>/var/lock/ivh_clean_check.lock; flock -n 9 || { echo "FATAL: lock held"; exit 1; }
REPS="${REPS:-3}"
OUT=/root/ivh_logs/p11_sub1_$(date +%m%d-%H%M%S).tsv
C="ivh_slowpath_wait_ns ivh_slowpath_halt_ns ivh_slowpath_wait_events ivh_node_spin_attempts ivh_node_spin_success_attempts ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_beat_tier1_fired ivh_beat_tier2_fired ivh_beat_tier2_checked ivh_cs_head_bailed ivh_evict_marked"
printf "workload\tmetric\tarm\trep\tvalue\tspin_ns\thalt_ns\tentries\tpasses\titers\tt1\tt2f\tt2c\tcsb\tev\n" > "$OUT"
snap(){ python3 $T/read_ivh_counters.py $C 2>/dev/null | awk -F= '{gsub(/ /,"",$1);gsub(/ /,"",$2);printf "%s=%s ",$1,$2}'; }
gv(){ echo "$1" | grep -oP "$2=\K[0-9]+" | head -1; }
clean(){ bash $T/pvbase.sh >/dev/null 2>&1 || return 1
  echo 2200000 > $S/ivh_cs_tick_period; echo 2 > $S/ivh_cs_owed_ticks
  echo 3300000 > $S/ivh_pv_beat_threshold; echo 1100000 > $S/ivh_pv_evict_threshold
  echo 4095 > $S/ivh_pv_beat_publish_mask; }
arm(){ clean || return 1
  if [ "$1" = mig ]; then
    bash $T/p7v2_arm.sh 2500000 >/dev/null 2>&1 || return 1
    for k in ivh_pv_tier2_enable ivh_head_bypass_enable ivh_head_bypass_probe ivh_cs_head_bail ivh_pv_evict_enable; do echo 0 > $S/$k; done
    [ "$(cat $S/ivh_pv_tier1_enable)" = 1 ] || return 1      # reference IS mig+t1
  else
    IVH_MASK="${1#*m}" ; US="${1%m*}"
    IVH_MASK=$IVH_MASK bash $T/p11_arm.sh "$US" >/dev/null 2>&1 || return 1
    [ "$(cat $S/ivh_pv_beat_publish_mask)" = "$IVH_MASK" ] || return 1
  fi
  sleep 1; }
MT="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=100 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"
P=/root/parsec-benchmark
W=(
"hackbench_pipe_thr|TIME|/root|hackbench -T -g1 -f8 -l150000|grep -oP '^Time:\s*\K[0-9.]+'"
"memtier_memcached|THROUGHPUT|/root|MT|grep -oP 'Totals\s+\K[0-9.]+'"
"dbench_16_noF|THROUGHPUT|/root|dbench -t 15 16 -D /root/dbench_test|grep -oP 'Throughput\s+\K[0-9.]+'"
"ebizzy_mmap|THROUGHPUT|/root|/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304|grep -oP '^\K[0-9]+(?= records/s)'"
"parsec_vips|TIME|$P/pkgs/apps/vips/run|IM_CONCURRENCY=16 $P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v|x"
)
ARMS="mig 200m4095 400m4095 800m4095 200m255 400m255 800m255"
echo "### p11_sub1ms  arms=[$ARMS]  reps=$REPS  ref=mig+t1  -> $OUT"
for e in "${W[@]}"; do
  IFS='|' read -r n m wd c x <<< "$e"; [ "$c" = MT ] && c="$MT"
  echo "########## $n ##########"
  for rep in $(seq 1 $REPS); do
    for a in $ARMS; do
      arm "$a" || { echo "  ARMFAIL $a"; continue; }
      case "$n" in
        dbench_16_noF) rm -rf /root/dbench_test; mkdir -p /root/dbench_test ;;
        memtier_memcached) systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null; sleep 1
          memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=15 2>/dev/null 9>&-; sleep 2 ;;
      esac
      sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      b=$(snap); t0=$(date +%s%N)
      out=$( ( cd "$wd" && timeout 600 bash -c "$c" ) 2>&1 9>&- ); t1=$(date +%s%N); f=$(snap)
      if [ "$x" = x ]; then v=$(python3 -c "print(f'{($t1-$t0)/1e9:.3f}')"); else v=$(echo "$out" | eval $x | head -1); fi
      D(){ echo $(( $(gv "$f" $1) - $(gv "$b" $1) )); }
      printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$n" "$m" "$a" "$rep" "${v:-NA}" \
        "$(( $(D ivh_slowpath_wait_ns) - $(D ivh_slowpath_halt_ns) ))" "$(D ivh_slowpath_halt_ns)" "$(D ivh_slowpath_wait_events)" \
        "$(( $(D ivh_node_spin_attempts) + $(D ivh_node_spin_success_attempts) ))" \
        "$(( $(D ivh_node_spin_iters_sum) + $(D ivh_node_spin_success_iters_sum) ))" \
        "$(D ivh_beat_tier1_fired)" "$(D ivh_beat_tier2_fired)" "$(D ivh_beat_tier2_checked)" \
        "$(D ivh_cs_head_bailed)" "$(D ivh_evict_marked)" >> "$OUT"
      echo "  rep$rep $a = ${v:-NA}"
    done
  done
  python3 $T/p11_sub1_report.py "$OUT" "$n" || true
done
echo "P11_SUB1_DONE $OUT"
python3 $T/p11_sub1_report.py "$OUT" || true
