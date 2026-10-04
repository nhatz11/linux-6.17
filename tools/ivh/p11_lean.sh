#!/bin/bash
# p11_lean.sh -- LEAN, HIGH-REP, PAIRED test of adaptive spinning under UNIFORM
# starvation (all 16 vCPUs ~70%), which is the condition where migration has no
# healthy target to lean on and AS must carry the benefit.
#
# WHY LEAN: the 7-arm grid was redundant with existing mask-4095 data and could
# not afford enough reps. The question is ONE hypothesis: does a sub-1ms shared
# threshold beat mig+t1 on all five workloads? Three arms, more reps.
#
# WHY PAIRED: under uniform starvation hackbench swung 4.6x on FIXED work
# (5.63s..26.05s) and tier1_fired spanned 46..2,650,954. Absolute means are
# meaningless at that spread. Arms are interleaved WITHIN each rep and scored as
# PER-REP PAIRED DELTAS vs that rep's own mig+t1, which cancels host drift.
# Reported as MEDIAN of paired deltas + sign test, not mean of absolutes.
#
# Arms: mig+t1 reference | 400us @ mask 4095 | 400us @ mask 255
# The two AS arms differ ONLY in heartbeat density, so the mask effect is
# isolated at a fixed threshold within one sitting (the earlier -22% was a
# cross-sitting comparison and is withdrawn).
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
exec 9>/var/lock/ivh_clean_check.lock; flock -n 9 || { echo "FATAL: lock held"; exit 1; }
REPS="${REPS:-5}"
OUT=/root/ivh_logs/p11_lean_$(date +%m%d-%H%M%S).tsv
C="ivh_slowpath_wait_ns ivh_slowpath_halt_ns ivh_slowpath_wait_events ivh_node_spin_attempts ivh_node_spin_success_attempts ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_beat_tier1_fired ivh_beat_tier2_fired ivh_beat_tier2_checked ivh_cs_head_bailed ivh_evict_marked"
printf "workload\tmetric\tarm\trep\tvalue\tspin_ns\thalt_ns\tentries\tpasses\titers\tt1\tt2f\tt2c\tcsb\tev\tcap_mean\n" > "$OUT"
snap(){ python3 $T/read_ivh_counters.py $C 2>/dev/null | awk -F= '{gsub(/ /,"",$1);gsub(/ /,"",$2);printf "%s=%s ",$1,$2}'; }
gv(){ echo "$1" | grep -oP "$2=\K[0-9]+" | head -1; }
capmean(){ awk 'NR>2{s+=$11;n++} END{printf "%.0f", (n?s/n:0)}' /proc/ivh_cpu_stats; }
clean(){ bash $T/pvbase.sh >/dev/null 2>&1 || return 1
  echo 2200000 > $S/ivh_cs_tick_period; echo 2 > $S/ivh_cs_owed_ticks
  echo 3300000 > $S/ivh_pv_beat_threshold; echo 1100000 > $S/ivh_pv_evict_threshold
  echo 4095 > $S/ivh_pv_beat_publish_mask; }
arm(){ clean || return 1
  case $1 in
    mig) bash $T/p7v2_arm.sh 2500000 >/dev/null 2>&1 || return 1
         for k in ivh_pv_tier2_enable ivh_head_bypass_enable ivh_head_bypass_probe ivh_cs_head_bail ivh_pv_evict_enable; do echo 0 > $S/$k; done
         [ "$(cat $S/ivh_pv_tier1_enable)" = 1 ] || return 1 ;;
    as4095) IVH_MASK=4095 bash $T/p11_arm.sh 400 >/dev/null 2>&1 || return 1 ;;
    as255)  IVH_MASK=255  bash $T/p11_arm.sh 400 >/dev/null 2>&1 || return 1
            [ "$(cat $S/ivh_pv_beat_publish_mask)" = 255 ] || return 1 ;;
  esac; sleep 1; }
MT="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=100 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"
P=/root/parsec-benchmark
W=(
"hackbench_pipe_thr|TIME|/root|hackbench -T -g1 -f8 -l150000|grep -oP '^Time:\s*\K[0-9.]+'"
"memtier_memcached|THROUGHPUT|/root|MT|grep -oP 'Totals\s+\K[0-9.]+'"
"dbench_16_noF|THROUGHPUT|/root|dbench -t 15 16 -D /root/dbench_test|grep -oP 'Throughput\s+\K[0-9.]+'"
"ebizzy_mmap|THROUGHPUT|/root|/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304|grep -oP '^\K[0-9]+(?= records/s)'"
"parsec_vips|TIME|$P/pkgs/apps/vips/run|IM_CONCURRENCY=16 $P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v|x"
)
echo "### p11_lean  arms=[mig as4095 as255] thr=400us reps=$REPS  UNIFORM starvation  -> $OUT"
echo "### cap_mean at start: $(capmean) (uniform ~700-800 = no healthy half; migration is weak here BY DESIGN)"
for e in "${W[@]}"; do
  IFS='|' read -r n m wd c x <<< "$e"; [ "$c" = MT ] && c="$MT"
  echo "########## $n ##########"
  for rep in $(seq 1 $REPS); do
    # rotate arm order each rep so position cannot alias onto the arm
    case $((rep % 3)) in 1) O="mig as4095 as255";; 2) O="as4095 as255 mig";; 0) O="as255 mig as4095";; esac
    for a in $O; do
      arm "$a" || { echo "  ARMFAIL $a"; continue; }
      case "$n" in
        dbench_16_noF) rm -rf /root/dbench_test; mkdir -p /root/dbench_test ;;
        memtier_memcached) systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null; sleep 1
          memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=15 2>/dev/null 9>&-; sleep 2 ;;
      esac
      sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      CM=$(capmean); b=$(snap); t0=$(date +%s%N)
      out=$( ( cd "$wd" && timeout 600 bash -c "$c" ) 2>&1 9>&- ); t1=$(date +%s%N); f=$(snap)
      if [ "$x" = x ]; then v=$(python3 -c "print(f'{($t1-$t0)/1e9:.3f}')"); else v=$(echo "$out" | eval $x | head -1); fi
      D(){ echo $(( $(gv "$f" $1) - $(gv "$b" $1) )); }
      printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$n" "$m" "$a" "$rep" "${v:-NA}" \
        "$(( $(D ivh_slowpath_wait_ns) - $(D ivh_slowpath_halt_ns) ))" "$(D ivh_slowpath_halt_ns)" "$(D ivh_slowpath_wait_events)" \
        "$(( $(D ivh_node_spin_attempts) + $(D ivh_node_spin_success_attempts) ))" \
        "$(( $(D ivh_node_spin_iters_sum) + $(D ivh_node_spin_success_iters_sum) ))" \
        "$(D ivh_beat_tier1_fired)" "$(D ivh_beat_tier2_fired)" "$(D ivh_beat_tier2_checked)" \
        "$(D ivh_cs_head_bailed)" "$(D ivh_evict_marked)" "$CM" >> "$OUT"
      echo "  rep$rep $a = ${v:-NA}  cap=$CM"
    done
  done
  python3 $T/p11_lean_report.py "$OUT" "$n" || true
done
echo "P11_LEAN_DONE $OUT"
python3 $T/p11_lean_report.py "$OUT" || true
