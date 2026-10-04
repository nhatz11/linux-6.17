#!/bin/bash
# p78.sh -- EVAL POINTS 7 and 8, post-audit build (2026-09-29).
#
#   MODE=p7  sweeps ivh_time_left_threshold_ns  (Gate 2 time-left sensitivity)
#            arms: pv 500us 1ms 2ms 4ms 8ms 10ms
#   MODE=p8  sweeps ivh_max_concurrent          (migration concurrency cap)
#            arms: pv 1 2 4 8 12 16
#
# Baseline is stock PV (pvbase.sh). Every other arm is the FULL STACK
# (mig + t1 + HEH + t2 + skip + head bypass) with ONE parameter changed.
#
# WHAT THE 2026-09-29 AUDIT CHANGED IN THIS HARNESS:
#  * bpftrace attaches in EVERY arm INCLUDING pv. It used to attach only on IVH
#    arms, which taxed them ~0.3-0.6 CPU-s per wall second and handed the
#    baseline a free win. In pv it simply records zero migrations.
#  * ivh_cs_track_enabled=1 in EVERY arm including pv. cs_exit() is the only
#    writer of current->last_cs_ns, which is Gate 2's critical-section term
#    (fair.c:13829,13852). At 0 the term is dead and point 7 sweeps a truncated
#    formula; and if it differs between arms the cs_enter/cs_exit cost is
#    asymmetric.
#  * The swept value is written LAST by p78_arm.sh and asserted, so a later
#    spin_mode/feature write cannot silently revert it.
#  * The GATE'S OWN counter is logged per arm: ivh_steal_imminent_time_left_reject
#    for point 7. An arm whose gate counter does not move from its neighbour is
#    degenerate, not a null -- this is the ivh_pv_beat_threshold=5ms failure.
#  * Gate 4 has NO in-kernel rejection counter (fair.c:13942 is a bare return).
#    Occupancy of ivh_in_schedule is sampled instead, giving sc_max and the
#    at-cap fraction. Note the cap is ADVISORY: the gate reads the counter at
#    :13942 but increments at :14039, with a sleeping GFP_KERNEL alloc between,
#    so arrivals can overshoot. sc_max < cap means the arm never bound.
#  * The BPF selector link is asserted present before every run. It is NOT
#    pinned (/sys/fs/bpf is empty); if it drops, the hook stub returns 0 and
#    fair.c:13972 accepts CPU 0 as a target -- every thread migrates to CPU 0.
#  * SPIN TIME is ivh_slowpath_wait_ns (a real sched_clock ns accumulator).
#    Iteration counters are reported separately AS COUNTS, and now include the
#    HEAD pair, which the ladder's formula omitted.
set -u
S=/proc/sys/kernel
T=/root/ivh_tools
source $T/suite14.sh
# DROP workloads whose PV arm exceeds 30 s (user rule, 2026-09-29):
#   parsec_canneal 85.5s, parsec_bodytrack 68.0s, parsec_dedup 52-144s
#   (PV CV 44%), psearchy 33.3-34.5s.  tinyconfig kept: median 29.87 s.
if [ -n "${DROP:-}" ]; then
  _keep=(); for _e in "${SUITE14[@]}"; do
    _n="${_e%%|*}"; case " $DROP " in *" $_n "*) ;; *) _keep+=("$_e");; esac
  done; SUITE14=("${_keep[@]}")
fi
R="python3 $T/read_ivh_counters.py"
MODE="${MODE:-p7}"
REPS="${REPS:-3}"
BTF_ID="${BTF_ID:-66718}"
MT="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=10 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"
STAMP=$(date +%m%d-%H%M%S)
OUT="${OUT:-$T/${MODE}_$STAMP.csv}"
HIST="${OUT%.csv}_hist.txt"

case "$MODE" in
  p7) ARMS=(pv 500000 1000000 2000000 4000000 8000000 10000000); SET="tlt";;
  p8) ARMS=(pv 1 2 4 8 12 16);                                   SET="mc";;
  *) echo "FATAL: MODE must be p7 or p8"; exit 1;;
esac

CTRS="ivh_slowpath_wait_ns ivh_slowpath_wait_events ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_head_spin_iters_sum ivh_head_spin_iters_bail_sum ivh_steal_imminent_time_left_reject ivh_steal_imminent_capacity_reject ivh_prelock_calls ivh_beat_tier1_fired ivh_beat_tier2_fired ivh_cs_head_bailed ivh_head_bypass_fired ivh_evict_marked"

preflight(){   # the selector must be attached or every migration targets CPU 0
  bpftool link list 2>/dev/null | grep -q "target_btf_id $BTF_ID" || {
    echo "FATAL: BPF selector link (btf_id $BTF_ID) is GONE -- aborting"; exit 1; }
}
setarm78(){ [ "$1" = pv ] && bash $T/p78_arm.sh pv >/dev/null || bash $T/p78_arm.sh "$SET" "$1" >/dev/null; }

echo "workload,arm,rep,perf,dur_s,mig_n,cost_sum_ns,cost_mean_us,delay_sum_ns,delay_mean_us,mig_total_ns,wait_ns,wait_events,node_iters,head_iters,g2_reject,g1_reject,prelock,migs_kernel,sc_max,sc_atcap_pct,t1,t2,heh,hb,evict" > "$OUT"
: > "$HIST"
echo "== $MODE : ${#ARMS[@]} arms x ${#SUITE14[@]} workloads x $REPS reps -> $OUT"

for e in "${SUITE14[@]}"; do
  IFS='|' read -r wl dir met cmd ext <<< "$e"
  [ "$cmd" = "MEMTIER_CMD" ] && cmd="$MT"
  echo "########## $wl ##########"
  for r in $(seq 1 "$REPS"); do
    off=$(( (r-1) % ${#ARMS[@]} ))
    for i in $(seq 0 $(( ${#ARMS[@]} - 1 )) ); do
      a=${ARMS[$(( (i+off) % ${#ARMS[@]} ))]}
      setarm78 "$a" || { echo "  !! arm $a failed"; continue; }
      preflight
      prep14 "$wl" >/dev/null 2>&1 || { echo "  !! prep $wl failed"; continue; }
      sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1

      $R $CTRS > /tmp/p0.$$ 2>/dev/null
      m0=$(python3 $T/migcount.py 2>/dev/null || echo 0)
      : > /tmp/bt.$$
      timeout 400 bpftrace $T/migtime.bt > /tmp/bt.$$ 2>&1 &
      BT=$!
      for _ in $(seq 1 100); do grep -q "Attaching" /tmp/bt.$$ && break; sleep 0.2; done
      timeout 300 python3 $T/sample_atomic.py ivh_in_schedule 300 0.001 > /tmp/sc.$$ 2>&1 &
      SC=$!

      t0=$(date +%s.%N)
      if [ "$met" = TIME ]; then ( cd "$dir" && eval "$cmd" ) >/dev/null 2>&1; v=""
      else v=$( ( cd "$dir" && eval "$cmd" 2>&1 ) | eval "$ext" | tail -1 ); fi
      t1=$(date +%s.%N)

      kill -INT $SC 2>/dev/null; wait $SC 2>/dev/null
      kill -INT $BT 2>/dev/null; wait $BT 2>/dev/null
      m1=$(python3 $T/migcount.py 2>/dev/null || echo 0)
      $R $CTRS > /tmp/p1.$$ 2>/dev/null
      { echo "### $wl arm=$a rep=$r"; cat /tmp/bt.$$; } >> "$HIST"

      python3 $T/p78_row.py "$wl" "$a" "$r" "${v:-}" "$met" "$t0" "$t1" \
              "$((m1-m0))" /tmp/p0.$$ /tmp/p1.$$ /tmp/bt.$$ /tmp/sc.$$ "$a" "$MODE" "$OUT"
      rm -f /tmp/p0.$$ /tmp/p1.$$ /tmp/bt.$$ /tmp/sc.$$
    done
  done
  python3 $T/p78_report.py "$OUT" "$wl" 2>/dev/null
done
bash $T/pvbase.sh >/dev/null 2>&1
echo "WROTE $OUT  (histograms: $HIST)"; echo "P78-DONE-$MODE"
