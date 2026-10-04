#!/bin/bash
# p11_sweep.sh -- POINT 11, FINAL FORM. Staleness threshold sweep vs STOCK PV.
#
# Baseline: stock PV (pvbase.sh, every AS knob zeroed AND asserted). Tier 1 stays
# on there because it IS upstream pv_wait_early(), not AS.
# Arms: AS at 50/100/200/400/800 us, mask 255, MIGRATION OFF in every arm
# (ivh_universal_eligible=0) so this measures AS and nothing else.
#
# All three TSC mechanisms now actually move together: the arm script was
# corrected 2026-10-03 to write ivh_cs_noise_cycles (the real head-early-halt
# gate at criterion=1; ivh_cs_tick_period is dead there), to assert the publish
# mask (the kernel silently refuses <255), and to set evict_cheap_now=0.
#
# ebizzy is DROPPED: its contention is mmap_lock (rwsem), 182s vs 5s of qspinlock
# spin -- 169.8x, per eval_final.md sec 9. The qspinlock counter cannot see it and
# its only effective mechanism is migration, which is off here.
#
# METRIC: node_spin_iters * 26ns (tools/bpf/docs/spin_time_measurement.md).
# Ratio between arms is exact regardless of the constant. NODE spin only.
#
# Expected from the audit's degeneracy analysis (mask 255: I_head 12.3us,
# w_node 412us): T=50..400 clean; T=800 exceeds the node spin budget so tier 2
# becomes unreachable and should fade. Eviction peaks at T~I_node=3.2us so it is
# near-dead throughout -- that is structural, not mistuning.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
exec 9>/var/lock/ivh_clean_check.lock; flock -w 600 9 || { echo "FATAL: lock"; exit 1; }
REPS="${REPS:-3}"; MASK="${MASK:-255}"; THRS="${THRS:-50 100 200 400 800}"
OUT=/root/ivh_logs/p11sweep_$(date +%m%d-%H%M%S).tsv
C="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_node_spin_attempts ivh_node_spin_success_attempts ivh_slowpath_wait_ns ivh_slowpath_halt_ns ivh_slowpath_wait_events ivh_beat_tier2_fired ivh_head_bypass_fired ivh_cs_head_bailed ivh_evict_marked"
printf "workload\tmetric\tarm\tthr\trep\tvalue\titers\tpasses\tspin_ns\tentries\tt2f\thbf\tcsb\tev\tcap\n" > "$OUT"
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
    [ "$(cat $S/ivh_cs_noise_cycles)" = "$CYC" ] || { echo "  NOISE_CYCLES not set"; return 1; }
    [ "$(cat $S/ivh_pv_beat_publish_mask)" = "$MASK" ] || return 1
  fi; sleep 1; }
MT="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=100 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"
P=/root/parsec-benchmark
W=("hackbench_pipe_thr|TIME|/root|hackbench -T -g1 -f8 -l150000"
   "memtier_memcached|THROUGHPUT|/root|MT"
   "dbench_16_noF|THROUGHPUT|/root|dbench -t 15 16 -D /root/dbench_test"
   "parsec_vips|TIME|$P/pkgs/apps/vips/run|IM_CONCURRENCY=16 $P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v")
ARMS="pv $THRS"
echo "### p11_sweep arms=[$ARMS] mask=$MASK reps=$REPS -> $OUT"
echo "### $(echo $ARMS | wc -w) arms x 4 workloads x $REPS reps = $(( $(echo $ARMS|wc -w) * 4 * REPS )) runs"
for e in "${W[@]}"; do
  IFS='|' read -r n m wd c <<< "$e"; [ "$c" = MT ] && c="$MT"
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
      esac
      sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      CM=$(capm); b=($(snap)); t0=$(date +%s%N)
      out=$( ( cd "$wd" && timeout 600 bash -c "$c" ) 2>&1 9>&- ); t1=$(date +%s%N); f=($(snap))
      case "$m" in
        TIME) v=$(python3 -c "print(f'{($t1-$t0)/1e9:.3f}')") ;;   # wall clock: works for vips too
        THROUGHPUT)
          case "$n" in
            memtier_memcached) v=$(echo "$out" | grep -oP 'Totals\s+\K[0-9.]+' | head -1) ;;
            dbench_16_noF)     v=$(echo "$out" | grep -oP 'Throughput\s+\K[0-9.]+' | head -1) ;;
          esac ;;
      esac
      IT=$(( (${f[0]}-${b[0]}) + (${f[1]}-${b[1]}) )); PA=$(( (${f[2]}-${b[2]}) + (${f[3]}-${b[3]}) ))
      SP=$(( (${f[4]}-${b[4]}) - (${f[5]}-${b[5]}) )); EN=$(( ${f[6]}-${b[6]} ))
      T2=$(( ${f[7]}-${b[7]} )); HB=$(( ${f[8]}-${b[8]} )); CS=$(( ${f[9]}-${b[9]} )); EV=$(( ${f[10]}-${b[10]} ))
      [ "$a" != pv ] && [ "$T2" -eq 0 ] && echo "  *** DEAD ARM: $a fired zero tier2"
      [ "$a" = pv ] && [ "$T2" -ne 0 ] && echo "  *** PV CONTAMINATED: t2f=$T2"
      printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$n" "$m" "$a" "$a" "$rep" "${v:-NA}" \
        "$IT" "$PA" "$SP" "$EN" "$T2" "$HB" "$CS" "$EV" "$CM" >> "$OUT"
      echo "  rep$rep ${a}us val=${v:-NA} spin=$(python3 -c "print('%.1fs'%($IT*26e-9))") t2f=$T2 hbf=$HB csb=$CS ev=$EV"
    done
  done
  python3 $T/p11_sweep_report.py "$OUT" "$n" || true
done
bash $T/pvbase.sh >/dev/null 2>&1; echo 22000 > $S/ivh_cs_noise_cycles
python3 $T/p11_sweep_report.py "$OUT" || true
echo "P11SWEEP_DONE $OUT"
