#!/bin/bash
# as_vs_pv.sh -- adaptive spinning vs STOCK PV, migration OFF in both arms.
#
# Baseline is stock PV (pvbase.sh: spin_mode 1, every AS knob zeroed AND asserted).
# Note tier 1 is NOT disabled there and must not be: ivh_pv_tier1_enable=1 is stock
# upstream pv_wait_early(), i.e. part of PV, not part of AS.
#
# AS arm = p11_arm.sh with the MIGRATION GATE SHUT (ivh_universal_eligible=0).
# preempt_src/preempt_event_source stay at 2 because AS needs them: the tier-2
# stamp is only seeded when preempt_src != 0 (qspinlock_paravirt.h:1515) and
# eviction requires preempt_event_source == 2 (kvm.c:2524).
#
# METRIC: the documented one (tools/bpf/docs/spin_time_measurement.md) --
#   node_spin_iters = ivh_node_spin_iters_sum + ivh_node_spin_success_iters_sum
#   seconds = iters * 26e-9   (+/-13%)
# Compared as a RATIO between arms, which is exact regardless of the constant.
# Scope is NODE spin; head spin (~20-27% of total) is uninstrumented.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
exec 9>/var/lock/ivh_clean_check.lock; flock -n 9 || { echo "FATAL: lock held"; exit 1; }
REPS="${REPS:-3}"; THR="${THR:-400}"; MASK="${MASK:-255}"
OUT=/root/ivh_logs/asvspv_$(date +%m%d-%H%M%S).tsv
C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_node_spin_attempts ivh_node_spin_success_attempts ivh_slowpath_wait_ns ivh_slowpath_halt_ns ivh_slowpath_wait_events ivh_beat_tier2_fired ivh_head_bypass_fired ivh_cs_head_bailed ivh_evict_marked"
printf "workload\tmetric\tarm\trep\tvalue\titers\tpasses\tspin_ns\tentries\tt2f\thbf\tcsb\tev\tcap\n" > "$OUT"
snap(){ python3 $T/read_ivh_counters.py $C 2>/dev/null | awk -F= '{gsub(/ /,"",$2);printf "%s ",$2}'; }
capm(){ awk 'NR>2{s+=$11;n++} END{printf "%.0f", s/n}' /proc/ivh_cpu_stats; }
arm(){ case $1 in
  pv) bash $T/pvbase.sh >/dev/null 2>&1 || return 1
      [ "$(cat $S/ivh_universal_eligible)" = 0 ] || return 1
      [ "$(cat $S/ivh_pv_tier2_enable)" = 0 ] || return 1 ;;
  as) IVH_MASK=$MASK bash $T/p11_arm.sh $THR >/dev/null 2>&1 || return 1
      echo 0 > $S/ivh_universal_eligible          # SHUT the migration gate
      [ "$(cat $S/ivh_universal_eligible)" = 0 ] || { echo "  MIGGATE still open"; return 1; }
      [ "$(cat $S/ivh_pv_tier2_enable)" = 1 ] || return 1
      [ "$(cat $S/ivh_pv_preempt_src)" = 2 ] || return 1 ;;
  esac; sleep 1; }
MT="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=100 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"
P=/root/parsec-benchmark
W=("hackbench_pipe_thr|TIME|/root|hackbench -T -g1 -f8 -l150000|grep -oP '^Time:\s*\K[0-9.]+'"
   "memtier_memcached|THROUGHPUT|/root|MT|grep -oP 'Totals\s+\K[0-9.]+'"
   "dbench_16_noF|THROUGHPUT|/root|dbench -t 15 16 -D /root/dbench_test|grep -oP 'Throughput\s+\K[0-9.]+'"
   "ebizzy_mmap|THROUGHPUT|/root|/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304|grep -oP '^\K[0-9]+(?= records/s)'"
   "parsec_vips|TIME|$P/pkgs/apps/vips/run|IM_CONCURRENCY=16 $P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v|x")
echo "### as_vs_pv: stock PV vs PV+AS (${THR}us, mask $MASK), MIGRATION OFF BOTH -> $OUT"
for e in "${W[@]}"; do
  IFS='|' read -r n m wd c x <<< "$e"; [ "$c" = MT ] && c="$MT"
  echo "########## $n ##########"
  for rep in $(seq 1 $REPS); do
    case $((rep % 2)) in 1) O="pv as";; 0) O="as pv";; esac
    for a in $O; do
      arm "$a" || { echo "  ARMFAIL $a"; continue; }
      case "$n" in
        dbench_16_noF) rm -rf /root/dbench_test; mkdir -p /root/dbench_test ;;
        memtier_memcached) systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null; sleep 1
          memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=15 2>/dev/null 9>&-; sleep 2 ;;
        ebizzy_mmap) ( cd /root && /home/nick/Desktop/ebizzy -S 3 -t 16 -m -s 4194304 >/dev/null 2>&1 ) ;;
      esac
      sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      CM=$(capm); b=($(snap))
      out=$( ( cd "$wd" && timeout 600 bash -c "$c" ) 2>&1 9>&- ); f=($(snap))
      if [ "$x" = x ]; then v=NA_TIME; else v=$(echo "$out" | eval $x | head -1); fi
      IT=$(( (${f[0]}-${b[0]}) + (${f[1]}-${b[1]}) )); PA=$(( (${f[2]}-${b[2]}) + (${f[3]}-${b[3]}) ))
      SP=$(( (${f[4]}-${b[4]}) - (${f[5]}-${b[5]}) )); EN=$(( ${f[6]}-${b[6]} ))
      T2=$(( ${f[7]}-${b[7]} )); HB=$(( ${f[8]}-${b[8]} )); CS=$(( ${f[9]}-${b[9]} )); EV=$(( ${f[10]}-${b[10]} ))
      [ "$a" = as ] && [ "$T2" -eq 0 ] && echo "  *** AS ARM FIRED ZERO tier2 -- arm is dead"
      [ "$a" = pv ] && [ "$T2" -ne 0 ] && echo "  *** PV ARM FIRED tier2=$T2 -- contaminated"
      [ "$v" = NA_TIME ] && v=$(python3 -c "print('%.3f'%0)")
      printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$n" "$m" "$a" "$rep" "${v:-NA}" \
        "$IT" "$PA" "$SP" "$EN" "$T2" "$HB" "$CS" "$EV" "$CM" >> "$OUT"
      echo "  rep$rep $a val=${v:-NA} iters=$IT ($(python3 -c "print('%.1fs'%($IT*26e-9))")) t2f=$T2 hbf=$HB csb=$CS ev=$EV"
    done
  done
done
python3 $T/asvspv_report.py "$OUT" || true
echo "ASVSPV_DONE $OUT"
