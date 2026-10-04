#!/bin/bash
# point7_v2.sh -- point 7 rerun: MIGRATION ONLY, no adaptive spinning.
#
# Arms: pv + 0.25/0.5/1/1.5/2/3/4 ms time-left thresholds.
# Capacity + active time from vcap; Gate 2 reads the EWMA (source=2).
# Kernel AS off (spin_mode 1); NHextend's userspace AFL off (IVH_AFL_DISABLE=1,
# set in suite6.sh) -- migration is the only mechanism under test.
#
# Collects per run: throughput, Gate-2 eval/reject, migrations, and BOTH spin
# definitions from spinsave.sh's counter set:
#   A = (node_iters + node_success_iters + head_iters + head_bail_iters) x 23.5ns
#       exact, never negative.
#   B = (slowpath_wait_ns - slowpath_halt_ns) / 1e9        [G-LOCK-53, FIXED]
#       Both terms are sched_clock ns behind the SAME
#       ivh_slowpath_wait_measure + !in_interrupt() gate, so halt is a strict
#       subset of wait and B cannot go negative. Replaces the withdrawn
#       (wait - (node+head)_halt_cycles/2.2) form, whose halt/wall ratio
#       spanned 0.030..1.746 over 243 runs (spin_time_measurement.md).
#       The old raw components are still stored for cross-checking.
set -u
S=/proc/sys/kernel
source /root/ivh_tools/suite6.sh
OUT=${OUT:-/root/ivh_logs/p7v2_$(date +%m%d-%H%M%S)}; mkdir -p "$OUT"
REPS=${REPS:-3}
ARMS=${ARMS:-"pv 250000 500000 1000000 1500000 2000000 3000000 4000000"}

pgrep -x vcap >/dev/null || { echo "FATAL: vcap not running"; exit 1; }
pgrep -x vcap_probe >/dev/null && { echo "FATAL: vcap_probe running (should be deleted)"; exit 1; }
echo "kernel=$(uname -r)  reps=$REPS"
echo "arms: $ARMS"
[ -f "$OUT/raw.tsv" ] || printf "arm\tbench\trep\tvalue\tg2_eval\tg2_fired\tmigs\tnode_i\tnode_si\thead_i\thead_bi\twait_ns\thalt_ns\tnode_halt_c\thead_halt_c\n" > "$OUT/raw.tsv"

plain(){ python3 /root/ivh_tools/read_ivh_counters.py "$1" 2>/dev/null | awk '{print $NF}'; }
tot(){ python3 /root/ivh_tools/read_ivh_counters.py "$1" 2>/dev/null | grep -oP 'TOTAL\s*\] = \K[0-9]+'; }
snap(){ echo "$(plain ivh_node_spin_iters_sum) $(plain ivh_node_spin_success_iters_sum) \
$(plain ivh_head_spin_iters_sum) $(plain ivh_head_spin_iters_bail_sum) \
$(plain ivh_slowpath_wait_ns) $(plain ivh_slowpath_halt_ns) $(tot ivh_node_halt_cycles) $(tot ivh_head_halt_cycles) \
$(plain ivh_g2_eval) $(plain ivh_steal_imminent_time_left_reject)"; }
migs(){ python3 /root/ivh_tools/migcount.py 2>/dev/null || echo 0; }

for arm in $ARMS; do
  grep -q "^$arm	" "$OUT/raw.tsv" 2>/dev/null && { echo "skip $arm (done)"; continue; }
  bash /root/ivh_tools/p7v2_arm.sh "$arm" || exit 1
  sleep 3
  cat /proc/ivh_cpu_stats > "$OUT/stats_$arm.txt"
  ew=$(awk 'NR>2 && $1<8 {s+=$10;n++} END{printf "%.2f",s/n/1e6}' "$OUT/stats_$arm.txt")
  echo "--- arm $arm  (ewma cpu0-7 = ${ew}ms) ---"
  for entry in "${SUITE6[@]}"; do
    IFS='|' read -r name dir kind cmd ext <<< "$entry"
    [ "$cmd" = MEMTIER_CMD ] && cmd="$MEMTIER_CMD"
    for r in $(seq "$REPS"); do
      prep6 "$name" >/dev/null 2>&1 || true
      read n0 ns0 h0 hb0 w0 hm0 nh0 hh0 e0 f0 <<< "$(snap)"; m0=$(migs)
      t0=$(date +%s.%N)
      raw=$(cd "$dir" && timeout 400 bash -c "$cmd" 2>&1)
      t1=$(date +%s.%N)
      read n1 ns1 h1 hb1 w1 hm1 nh1 hh1 e1 f1 <<< "$(snap)"; m1=$(migs)
      if [ "$kind" = TIME ] && [ "$ext" = x ]; then v=$(echo "$t1 - $t0" | bc)
      else v=$(echo "$raw" | eval "$ext" | head -1); fi
      printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
        "$arm" "$name" "$r" "${v:-NA}" "$((e1-e0))" "$((f1-f0))" "$((m1-m0))" \
        "$((n1-n0))" "$((ns1-ns0))" "$((h1-h0))" "$((hb1-hb0))" \
        "$((w1-w0))" "$((hm1-hm0))" "$((nh1-nh0))" "$((hh1-hh0))" >> "$OUT/raw.tsv"
      fr=$(python3 -c "d=$((e1-e0)); print(f'{100*$((f1-f0))/d:.1f}%' if d else 'n/a')")
      echo "  $name rep$r = ${v:-NA}   fire=$fr  migs=$((m1-m0))"
    done
  done
done
echo; echo "======== POINT 7 v2 ========"
python3 /root/ivh_tools/point7_v2_report.py "$OUT" | tee "$OUT/report.txt"
echo; echo "artifacts: $OUT"
