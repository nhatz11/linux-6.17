#!/bin/bash
# g52_tlsweep.sh -- the time-left curve, last_active vs EWMA.
#
# WHAT #7 COULD NOT ANSWER: it swept the threshold and saw flat throughput,
# with no measurement of whether the gate's verdict was actually moving. It
# was: G-LOCK-50 measured a 7.4x firing swing across the same range. So
# "flat throughput" meant "rejection does not drive throughput", not "the
# knob is dead" -- two very different findings.
#
# This sweep therefore records BOTH axes per arm: Gate 2's firing rate and
# the throughput. A defensible time-left graph needs both curves; one alone
# is what made #7 unpublishable.
#
# Capacity and active time are held at vcap for BOTH sources, so the only
# thing varying is which quantity Gate 2 thresholds and where the threshold
# sits. Arms at equal threshold are NOT comparable across sources -- the
# inputs live in different ranges (last_active ~1-3ms, EWMA ~3-13ms) -- so
# the grid deliberately spans both.
set -u
S=/proc/sys/kernel
OUT=${OUT:-/root/ivh_logs/tlsweep_$(date +%m%d-%H%M%S)}; mkdir -p "$OUT"
REPS=${REPS:-3}
THRESH=${THRESH:-"250000 500000 1000000 2000000 4000000 8000000 16000000 32000000"}
SOURCES=${SOURCES:-"1 2"}
BENCHES=${BENCHES:-"hackbench dbench ebizzy"}

pgrep -x vcap >/dev/null || { echo "FATAL: vcap not running"; exit 1; }
echo "kernel: $(uname -r)  reps=$REPS"
echo -e "src\tthresh_ns\tbench\trep\tvalue\tg2_eval\tg2_fired\tmigs" > "$OUT/raw.tsv"

ctr() { python3 /root/ivh_tools/read_ivh_counters.py "$1" 2>/dev/null | awk '{print $NF}'; }
migs() { python3 /root/ivh_tools/migcount.py 2>/dev/null || echo 0; }

run_bench() {
  case "$1" in
    hackbench) ( cd /root && timeout 120 hackbench -T -g1 -f8 -l150000 ) 2>&1 | grep -oP '^Time:\s*\K[0-9.]+' ;;
    dbench)    ( cd /root && timeout 120 dbench -F -t 15 16 -D /root/dbench_test ) 2>&1 | grep -oP 'Throughput\s+\K[0-9.]+' ;;
    ebizzy)    ( cd /root && timeout 60 /home/nick/Desktop/ebizzy -S 6 -t 16 -m -s 4194304 ) 2>&1 | grep -oP '^\K[0-9]+(?= records)' ;;
  esac
}

for src in $SOURCES; do
  for th in $THRESH; do
    bash /root/ivh_tools/p78_arm.sh tlt "$th" >/dev/null || exit 1
    echo 1 > $S/ivh_cap_writer; echo 1 > $S/ivh_act_writer
    echo "$src" > $S/ivh_time_left_source; echo "$th" > $S/ivh_time_left_threshold_ns
    a=$(cat $S/ivh_time_left_source); b=$(cat $S/ivh_time_left_threshold_ns)
    [ "$a" = "$src" ] && [ "$b" = "$th" ] || { echo "FATAL: arm did not take (src=$a th=$b)"; exit 1; }
    sleep 3
    echo "--- source=$src threshold=$th ns ---"
    for bch in $BENCHES; do
      for r in $(seq "$REPS"); do
        e0=$(ctr ivh_g2_eval); f0=$(ctr ivh_steal_imminent_time_left_reject); m0=$(migs)
        v=$(run_bench "$bch")
        e1=$(ctr ivh_g2_eval); f1=$(ctr ivh_steal_imminent_time_left_reject); m1=$(migs)
        printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
          "$src" "$th" "$bch" "$r" "$v" "$((e1-e0))" "$((f1-f0))" "$((m1-m0))" >> "$OUT/raw.tsv"
      done
    done
  done
done

echo; echo "============ TIME-LEFT CURVE ============"
python3 /root/ivh_tools/g52_tlplot.py "$OUT" | tee "$OUT/curve.txt"
echo; echo "artifacts: $OUT"
