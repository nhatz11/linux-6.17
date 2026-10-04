#!/bin/bash
# ADDITIVE LADDER -- does every mechanism add something on top of migration+tier1?
#
# Single phase: all five arms run interleaved against ONE baseline, so every
# comparison is drift-matched. (The earlier two-phase split cost us arms 5-7:
# the host moved 13-39% between phases and their "vs baseline" numbers were
# uninterpretable.)
#
#   1 mig_t1                BASELINE  migration + tier1
#   2 + HEH                 + head early halt   (the queue HEAD halts when the
#                             lock HOLDER's acquisition stamp is stale)
#   3 + t2                  + tier2             (halt when the PREDECESSOR's
#                             TSC heartbeat is stale)
#   4 + skip                + lock skipping     (holder promotes first LIVE waiter)
#   5 + hb                  + head bypass       (successor clears pending when
#                             the head is stale, reopening the steal path)
#
# METRICS: performance, and SPIN TIME measured by ITERATION COUNT.
#
#   spin_iters = ivh_node_spin_iters_sum + ivh_node_spin_success_iters_sum
#
# Both halves are required: `:2158` records the give-up path, `:1946` the
# acquire-during-spin path, and the GLOCK-11 comment at `:1931` says explicitly
# that the two pairs exist to be SUMMED for a complete unbiased total.
#
# WHY NOT wall - halt: that formula mixes two gates (the wall counter skips
# in_interrupt() and skips when wait_measure is off; the halt sites have no gate)
# and two clocks (sched_clock vs raw TSC). It produced NEGATIVE spin on 3 of 243
# runs. See tools/bpf/docs/spin_time_measurement.md.
#
# The iteration counter was validated by dose-response: sweeping
# ivh_pv_spin_threshold 1024 -> 16777216 moved spin_iters 903M -> 2430M
# monotonically while halt events fell 742,914 -> 0, exactly as it must if the
# counter measures what it claims.
#
# ns/iter = 23.5, from the two zero-halt points of that sweep where wall IS pure
# spin by definition (57.44s/2.368e9 = 24.3; 55.89s/2.430e9 = 23.0). It is NOT a
# universal constant -- an iteration costs more under heavier cacheline
# contention -- so ABSOLUTE seconds carry about +/-20%. Iteration counts are
# exact, so arm-vs-arm RATIOS are exact and need no constant. Both are reported.
#
# SCOPE: node spin only. The head's success path exits via `goto gotlock`
# (`:3589`) past its accounting, leaving 99.6% of head tenures uncounted, so
# head spin is not measurable without a rebuild. It is roughly 20-27% of total
# spin by survival estimate. Do not present this as total spin.
#
# Workloads: the 7 that run in under 30 s. parsec_dedup (median 80 s, baseline
# CV 77-103%) and parsec_bodytrack (median 75 s) are excluded.
set -u
S=/proc/sys/kernel
source /root/ivh_tools/ivh_benchmarks.sh
[ -n "${EXTRA:-}" ] && { source "$EXTRA"; echo "  OVERRIDE: $EXTRA"; }
R="python3 /root/ivh_tools/read_ivh_counters.py"

BENCHES="${BENCHES:-fsmark_tmpfs parsec_vips schbench ebizzy_mmap hackbench_pipe_thr dbench_16 wis_mmap2}"
REPS="${REPS:-3}"
REPS_HI="${REPS_HI:-5}"
CV_MAX="${CV_MAX:-6.0}"
NS_PER_ITER="${NS_PER_ITER:-23.5}"
OUT="${OUT:-/root/ivh_tools/ladder_$(date +%m%d-%H%M%S).csv}"

CTRS="ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_node_spin_attempts ivh_node_spin_success_attempts ivh_slowpath_wait_ns ivh_slowpath_wait_events ivh_beat_tier1_fired ivh_beat_tier2_fired ivh_cs_head_bailed ivh_head_bypass_fired ivh_evict_marked"

lookup(){
  local e n dir dr c x rec
  for e in "${IVH_SCALED[@]}" "${IVH_WORKLOADS[@]}" "${IVH_MIGRATION_WORKLOADS[@]}"; do
    IFS='|' read -r n dir dr c x rec <<< "$e"
    if [ "$n" = "$1" ]; then B_DIR="${dir:-/root}"; B_DR="$dr"; B_CMD="$c"; B_EXT="$x"; return 0; fi
  done
  return 1
}

setarm(){
  local A="$1" heh=0 t2=0 sk=0 hb=0 beat=11000000
  case "$A" in
    mig_t1)                 ;;
    mig_t1_heh)             heh=1 ;;
    mig_t1_heh_t2)          heh=1; t2=1 ;;
    mig_t1_heh_t2_sk)       heh=1; t2=1; sk=1 ;;
    mig_t1_heh_t2_sk_hb)    heh=1; t2=1; sk=1; hb=1 ;;
    *) echo "FATAL: unknown arm $A"; exit 1 ;;
  esac
  # tier2 AND head bypass both read ivh_pv_beat_threshold; at the shipped 5 ms
  # NEITHER fires. 1 ms is where bypass was validated (99.7% taken) and tier2
  # fires ~64k/run.
  if [ "$t2" = 1 ] || [ "$hb" = 1 ]; then beat=2200000; fi

  # spin_mode FIRST: it sets tier1_enable=1, tier2_enable=1 and forces
  # beat_threshold=11000000, so every feature write must follow it.
  /root/spin_mode 2 >/dev/null 2>&1

  # migration ON in every arm -- it is the baseline mechanism, not a variable
  echo 2 > $S/ivh_pv_preempt_src
  echo 2 > $S/ivh_preempt_event_source
  echo 4000000 > $S/ivh_time_left_threshold_ns
  echo 8 > $S/ivh_max_concurrent
  echo 1 > $S/ivh_universal_eligible
  # tier1 ON in every arm (it is the baseline's own mechanism)
  echo 1 > $S/ivh_pv_tier1_enable
  echo 32768 > $S/ivh_pv_spin_threshold

  echo "$beat" > $S/ivh_pv_beat_threshold
  echo "$t2"   > $S/ivh_pv_tier2_enable

  if [ "$heh" = 1 ]; then
    echo 1 > $S/ivh_cs_track_enabled; echo 1 > $S/ivh_cs_owner_enable
    echo 1 > $S/ivh_cs_owner_clear;   echo 0 > $S/ivh_cs_owner_fast
    echo 1 > $S/ivh_cs_scan;          echo 1 > $S/ivh_cs_criterion
    echo 1 > $S/ivh_cs_head_probe;    echo 1 > $S/ivh_cs_head_bail
  else
    echo 0 > $S/ivh_cs_head_bail;  echo 0 > $S/ivh_cs_head_probe
    echo 0 > $S/ivh_cs_criterion;  echo 0 > $S/ivh_cs_owner_enable
    echo 0 > $S/ivh_cs_scan
  fi

  if [ "$sk" = 1 ]; then
    echo 1 > $S/ivh_pv_evict_enable;    echo 1 > $S/ivh_pv_evict_node_stamp
    echo 1 > $S/ivh_pv_evict_lookahead; echo 1 > $S/ivh_pv_requeue_nosteal
    echo 2 > $S/ivh_pv_evict_hop_cap;   echo 4 > $S/ivh_pv_requeue_max
    echo 0 > $S/ivh_pv_skip_point;      echo 1100000 > $S/ivh_pv_evict_threshold
  else
    echo 0 > $S/ivh_pv_evict_enable
  fi

  if [ "$hb" = 1 ]; then
    echo 1 > $S/ivh_head_bypass_enable; echo 1 > $S/ivh_head_bypass_probe
    echo 1 > $S/ivh_head_bypass_runs;   echo 0 > $S/ivh_head_bypass_hold
    echo 4 > $S/ivh_head_bypass_max
  else
    echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_head_bypass_probe
  fi

  echo 1 > $S/ivh_slowpath_wait_measure

  # assert EVERY factor, on AND off -- an off-assert is what catches a leak
  chk(){ [ "$(cat $S/$1)" = "$2" ] || { echo "FATAL[$A]: $1 is $(cat $S/$1) want $2"; exit 1; }; }
  chk ivh_adaptive_mode 2
  chk ivh_universal_eligible 1
  chk ivh_preempt_event_source 2
  chk ivh_pv_tier1_enable 1
  chk ivh_pv_spin_threshold 32768
  chk ivh_pv_tier2_enable "$t2"
  chk ivh_cs_head_bail "$heh"
  chk ivh_cs_head_probe "$heh"
  chk ivh_pv_evict_enable "$sk"
  chk ivh_head_bypass_probe "$hb"
  chk ivh_pv_beat_threshold "$beat"
  chk ivh_slowpath_wait_measure 1
  [ "$sk" = 0 ] || chk ivh_pv_evict_node_stamp 1
}

runbench(){
  lookup "$1" || { echo 0; return; }
  if [ "$B_EXT" = TIME ]; then
    local a b; a=$(date +%s.%N); ( cd "$B_DIR" && eval "$B_CMD" ) >/dev/null 2>&1; b=$(date +%s.%N)
    python3 -c "print(f'{$b-$a:.4f}')"
  else
    ( cd "$B_DIR" && eval "$B_CMD" 2>&1 ) | eval "$B_EXT" | tail -1
  fi
}

ARMS=(mig_t1 mig_t1_heh mig_t1_heh_t2 mig_t1_heh_t2_sk mig_t1_heh_t2_sk_hb)
N=${#ARMS[@]}
echo "workload,arm,rep,perf,dur_s,spin_iters,spin_attempts,spin_ns,wall_ns,wait_events,migs,t1_fired,t2_fired,heh_bailed,bypass_fired,evict_marked" > "$OUT"
echo "LADDER: $N arms x $(echo $BENCHES|wc -w) workloads x $REPS reps -> $OUT"
echo "  baseline = arm 1 (mig_t1).  spin from iteration counters at ${NS_PER_ITER} ns/iter."

for w in $BENCHES; do
  echo "########## $w ##########"
  lookup "$w" || { echo "  !! $w not in registry"; continue; }
  setarm mig_t1; runbench "$w" >/dev/null 2>&1   # warmup, discarded
  for r in $(seq 1 "$REPS"); do
    off=$(( (r-1) % N ))
    for i in $(seq 0 $((N-1))); do
      a=${ARMS[$(( (i+off) % N ))]}
      setarm "$a"; lookup "$w"
      sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      $R $CTRS > /tmp/ld0.$$ 2>/dev/null
      m0=$(python3 /root/ivh_tools/migcount.py 2>/dev/null || echo 0)
      t0=$(date +%s.%N); v=$(runbench "$w"); t1=$(date +%s.%N)
      m1=$(python3 /root/ivh_tools/migcount.py 2>/dev/null || echo 0)
      $R $CTRS > /tmp/ld1.$$ 2>/dev/null
      python3 /root/ivh_tools/ld_row.py "$w" "$a" "$r" "${v:-0}" "$B_DR" "$t0" "$t1" "$((m1-m0))" /tmp/ld0.$$ /tmp/ld1.$$ "$OUT" "$NS_PER_ITER"
      rm -f /tmp/ld0.$$ /tmp/ld1.$$
    done
  done
  # Adaptive reps: 3 by default; extend to REPS_HI only when an arm's spin
  # measurement is erratic (CV above CV_MAX). A stable workload does not need
  # the extra runs, and spending them everywhere would double the sweep.
  EXTRA_R=$(python3 - "$OUT" "$w" "$CV_MAX" <<'PZ'
import csv,sys,collections,statistics as st
rows=[r for r in csv.DictReader(open(sys.argv[1])) if r['workload']==sys.argv[2]]
g=collections.defaultdict(list)
for r in rows: g[r['arm']].append(float(r['spin_ns']))
worst=0.0; who=""
for a,v in g.items():
    if len(v)>=2 and st.mean(v):
        cv=100*st.stdev(v)/st.mean(v)
        if cv>worst: worst,who=cv,a
print(f"{worst:.1f} {who}" if worst>float(sys.argv[3]) else "")
PZ
)
  if [ -n "$EXTRA_R" ]; then
    echo "  >> spin CV ${EXTRA_R%% *}% on ${EXTRA_R##* } exceeds ${CV_MAX}% -- extending to ${REPS_HI} reps"
    for r in $(seq $((REPS+1)) "$REPS_HI"); do
      off=$(( (r-1) % N ))
      for i in $(seq 0 $((N-1))); do
        a=${ARMS[$(( (i+off) % N ))]}
        setarm "$a"; lookup "$w"
        sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
        $R $CTRS > /tmp/ld0.$$ 2>/dev/null
        m0=$(python3 /root/ivh_tools/migcount.py 2>/dev/null || echo 0)
        t0=$(date +%s.%N); v=$(runbench "$w"); t1=$(date +%s.%N)
        m1=$(python3 /root/ivh_tools/migcount.py 2>/dev/null || echo 0)
        $R $CTRS > /tmp/ld1.$$ 2>/dev/null
        python3 /root/ivh_tools/ld_row.py "$w" "$a" "$r" "${v:-0}" "$B_DR" "$t0" "$t1" "$((m1-m0))" /tmp/ld0.$$ /tmp/ld1.$$ "$OUT" "$NS_PER_ITER"
        rm -f /tmp/ld0.$$ /tmp/ld1.$$
      done
    done
  else
    echo "  >> all arms within ${CV_MAX}% spin CV -- ${REPS} reps sufficient"
  fi
  echo "  >> $w done"
done
echo "WROTE $OUT"; echo "LADDER-DONE"
