#!/bin/bash
# point7_ewma.sh -- redo point 7 with vcap capacity + EWMA active time.
#
# THE QUESTION: does thresholding an EWMA of active time make Gate 2's knob
# affect PERFORMANCE, where thresholding last_active did not?
#
# #7's original problem was not that the knob did nothing -- G-LOCK-50
# measured a 7.4x firing swing over the same range -- but that throughput was
# flat regardless, which reads as "the equation is not representative". So
# every arm here records BOTH the firing rate and the throughput. A flat
# throughput curve beside a steep firing curve is a RESULT, not a failure,
# and only the pair distinguishes them.
#
# CONFIG (fixed): vcap owns capacity AND active time; Gate 2 reads the EWMA.
#   ivh_cap_writer=1  ivh_act_writer=1  ivh_time_left_source=2
# vcap_probe is deliberately NOT running: it is obsolete now that vcap
# measures its own demand, and it costs ebizzy 73%.
#
# EWMA measured at 2.5-4.5ms on the contended vCPUs, so the grid is dense
# through 2-6ms where the transition must be, and extends either side.
set -u
S=/proc/sys/kernel
source /root/ivh_tools/suite6.sh
OUT=${OUT:-/root/ivh_logs/p7ewma_$(date +%m%d-%H%M%S)}; mkdir -p "$OUT"
REPS=${REPS:-3}
THRESH=${THRESH:-"250000 1000000 2000000 3000000 4000000 6000000 8000000 16000000"}

pgrep -x vcap >/dev/null || { echo "FATAL: vcap not running"; exit 1; }
pgrep -x vcap_probe >/dev/null && { echo "FATAL: vcap_probe IS running -- kill it"; exit 1; }
pgrep -a vcap | grep -v probe | grep -q -- "-p 200 -s 5000" \
  || { echo "FATAL: vcap not at -p 200 -s 5000"; exit 1; }
echo "kernel : $(uname -r)"
echo "vcap   : $(pgrep -a vcap | grep -v probe)"
echo "reps   : $REPS"
echo "thresh : $THRESH"
[ -f "$OUT/raw.tsv" ] || printf "arm\tbench\trep\tvalue\tg2_eval\tg2_fired\tmigs\n" > "$OUT/raw.tsv"

ctr(){ python3 /root/ivh_tools/read_ivh_counters.py "$1" 2>/dev/null | awk '{print $NF}'; }
migs(){ python3 /root/ivh_tools/migcount.py 2>/dev/null || echo 0; }

set_arm(){
  if [ "$1" = pv ]; then bash /root/ivh_tools/pvbase.sh >/dev/null || return 1; return 0; fi
  bash /root/ivh_tools/p78_arm.sh tlt "$1" >/dev/null || return 1
  echo 1 > $S/ivh_cap_writer; echo 1 > $S/ivh_act_writer
  echo 2 > $S/ivh_time_left_source; echo "$1" > $S/ivh_time_left_threshold_ns
  local a b c d
  a=$(cat $S/ivh_cap_writer); b=$(cat $S/ivh_act_writer)
  c=$(cat $S/ivh_time_left_source); d=$(cat $S/ivh_time_left_threshold_ns)
  [ "$a$b$c" = "112" ] && [ "$d" = "$1" ] || { echo "FATAL: arm $1 did not take ($a$b$c $d)"; return 1; }
}

for arm in pv $THRESH; do
  grep -q "^$arm	" "$OUT/raw.tsv" 2>/dev/null && { echo "skip $arm (done)"; continue; }
  set_arm "$arm" || exit 1
  sleep 3
  cat /proc/ivh_cpu_stats > "$OUT/stats_$arm.txt"
  echo "--- arm $arm ---"
  for entry in "${SUITE6[@]}"; do
    IFS='|' read -r name dir kind cmd ext <<< "$entry"
    [ "$cmd" = MEMTIER_CMD ] && cmd="$MEMTIER_CMD"
    for r in $(seq "$REPS"); do
      prep6 "$name" || { echo "  prep FAILED for $name"; continue; }
      e0=$(ctr ivh_g2_eval); f0=$(ctr ivh_steal_imminent_time_left_reject); m0=$(migs)
      t0=$(date +%s.%N)
      raw=$(cd "$dir" && timeout 400 bash -c "$cmd" 2>&1)
      t1=$(date +%s.%N)
      if [ "$kind" = TIME ] && [ "$ext" = x ]; then
        v=$(echo "$t1 - $t0" | bc)
      else
        v=$(echo "$raw" | eval "$ext" | head -1)
      fi
      e1=$(ctr ivh_g2_eval); f1=$(ctr ivh_steal_imminent_time_left_reject); m1=$(migs)
      printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
        "$arm" "$name" "$r" "${v:-NA}" "$((e1-e0))" "$((f1-f0))" "$((m1-m0))" >> "$OUT/raw.tsv"
      echo "  $name rep$r = ${v:-NA}"
    done
  done
done

echo; echo "======== POINT 7 (EWMA + vcap capacity) ========"
python3 /root/ivh_tools/point7_ewma_report.py "$OUT" | tee "$OUT/report.txt"
echo; echo "artifacts: $OUT"
