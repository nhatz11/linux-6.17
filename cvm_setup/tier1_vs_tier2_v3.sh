#!/bin/bash
# v3: the decisive experiment, per the independent review's GLOCK-12 findings.
# v2 found stock beats every mechanism=2 config by ~6%, and traced it to two
# incidental costs riding along on the SAME ivh_pv_wait_mechanism=2 switch as
# tier-2's early-bail addition: (1) an unbounded re-arm-on-exhaustion design
# (~0.5% cost) and (2) a critical-path smp_send_reschedule() IPI in
# pv_kick_node() that stock never sends (~5.7% cost, the dominant one).
# GLOCK-12 adds three sysctls to strip both out independently:
#   ivh_pv_rearm_max=0      -- halt on first exhaustion, exactly like stock
#   ivh_pv_kick_node_ipi=0  -- skip pv_kick_node()'s critical-path IPI
#   ivh_pv_kick_unlock_ipi  -- (not used here; off-critical-path, left at
#                              default 1; a hang-safety proc handler refuses
#                              0 unless the host offers a hypercall fallback)
#
# Arms:
#   D   : mechanism=0, kick_pure_ipi=0, src=0            -- true stock (control)
#   G   : mechanism=2, kick_node_ipi=0, rearm_max=0, src=0  -- mechanism 2
#         with BOTH incidental costs stripped, tier-2 OFF. If GLOCK-12's
#         attribution is right, this should land within noise of D. This is
#         the validation step -- if it does NOT match D, there is a fourth
#         unaccounted difference and nothing below should be trusted yet.
#   H1  : same as G but src=2, threshold=220000 (100us)  -- tier-2's TRUE
#         isolated contribution: stock-equivalent baggage, early-bail added.
#   H2  : same as H1 but threshold=1100000 (500us)
#   B1  : mechanism=2, DEFAULT baggage (kick_node_ipi=1, rearm_max=unbounded),
#         src=2, threshold=220000 -- today's (pre-fix) configuration, kept
#         for a direct before/after comparison against H1.
#
# Order counterbalanced (reversed on even rounds), fresh rmmod/insmod + fresh
# client per arm-execution, same discipline as v2.
set -u

REPS="${REPS:-5}"
THREADS="${THREADS:-32}"
DURATION="${DURATION:-12}"
HOLD_US="${HOLD_US:-100}"
STATE=/root/linux-6.17/cvm_setup/.state
SCRATCH=/tmp/claude-0/-root-linux-6-17/b98a4d93-d606-4bb7-bd13-7031a5eea896/scratchpad
SNAP_BT="$SCRATCH/cycle_snapshot_v2.bt"
TS=$(date +%Y%m%d_%H%M%S)
OUTDIR="$STATE/t1_vs_t2_v3_${TS}"
mkdir -p "$OUTDIR"
SUMMARY="$OUTDIR/SUMMARY.txt"
: > "$SUMMARY"

log() { echo "$@" | tee -a "$SUMMARY"; }

set_sysctl() {
  echo "$2" | sudo tee "/proc/sys/kernel/$1" > /dev/null
  local got; got=$(cat "/proc/sys/kernel/$1")
  if [ "$got" != "$2" ]; then
    log "FATAL: write to $1 rejected: wanted $2, got $got"
    exit 1
  fi
}

set_pv() {
  local mech="$1" kick="$2"
  if [ "$mech" = "0" ]; then
    set_sysctl ivh_pv_kick_pure_ipi 0
    set_sysctl ivh_pv_wait_mechanism 0
  else
    set_sysctl ivh_pv_wait_mechanism "$mech"
    set_sysctl ivh_pv_kick_pure_ipi "$kick"
  fi
}

snap() { sudo bpftrace "$SNAP_BT" 2>/dev/null | grep '^SNAP'; }
field() { echo "$1" | grep -oP "${2}=\K[0-9]+"; }
acquires() { cat /dev/lhp_stress 2>/dev/null || echo 0; }

log "Tier1-vs-Tier2 v3 (GLOCK-12 fix validation) started: $(date)"
log "THREADS=$THREADS DURATION=${DURATION}s HOLD_US=$HOLD_US REPS=$REPS"
log "Arms: D(stock) G(mech2,baggage-stripped,tier2-off) H1(baggage-stripped,tier2-on,100us) H2(...,500us) B1(mech2,DEFAULT-baggage,tier2-on,100us)"

declare -A ITERS_SUM ITERS_N
declare -A ACQ_PER_S_SUM ACQ_PER_S_N
declare -A STEAL_MS_SUM STEAL_MS_N
declare -A REARM_HALTS_SUM REARM_HALTS_N

configure_arm() {
  case "$1" in
    D)  set_pv 0 0
        set_sysctl ivh_pv_preempt_src 0 ;;
    G)  set_pv 2 1
        set_sysctl ivh_pv_kick_node_ipi 0
        set_sysctl ivh_pv_rearm_max 0
        set_sysctl ivh_pv_preempt_src 0 ;;
    H1) set_pv 2 1
        set_sysctl ivh_pv_kick_node_ipi 0
        set_sysctl ivh_pv_rearm_max 0
        set_sysctl ivh_pv_preempt_src 2
        set_sysctl ivh_pv_beat_threshold 220000 ;;
    H2) set_pv 2 1
        set_sysctl ivh_pv_kick_node_ipi 0
        set_sysctl ivh_pv_rearm_max 0
        set_sysctl ivh_pv_preempt_src 2
        set_sysctl ivh_pv_beat_threshold 1100000 ;;
    B1) set_pv 2 1
        set_sysctl ivh_pv_kick_node_ipi 1
        set_sysctl ivh_pv_rearm_max 18446744073709551615
        set_sysctl ivh_pv_preempt_src 2
        set_sysctl ivh_pv_beat_threshold 220000 ;;
  esac
}

run_arm() {
  local label="$1"
  log ""
  log "--- arm: $label ---"

  sudo rmmod lhp_stress 2>/dev/null
  sudo insmod /root/lhp_stress/lhp_stress.ko hold_us=$HOLD_US
  sleep 0.5
  [ -e /dev/lhp_stress ] || { log "FATAL: /dev/lhp_stress did not appear after insmod"; exit 1; }

  configure_arm "$label"
  sysctl -a 2>/dev/null | grep '^kernel\.ivh_pv' | tee -a "$SUMMARY"

  local before after acq_before acq_after
  before=$(snap)
  acq_before=$(acquires)

  local t0 t1_wall
  t0=$(date +%s.%N)
  /root/lhp_stress/stress_client "$THREADS" "$DURATION" >> "$SUMMARY" 2>&1
  t1_wall=$(date +%s.%N)

  after=$(snap)
  acq_after=$(acquires)

  local wall; wall=$(python3 -c "print(f'{$t1_wall-$t0:.3f}')")
  local acq_delta=$((acq_after - acq_before))
  local acq_per_s; acq_per_s=$(python3 -c "print(f'{$acq_delta/$wall:.1f}' if $wall>0 else 'NA')")

  local tier1 tier2f node_it node_at node_ok_it node_ok_at steal_delta
  tier1=$(( $(field "$after" tier1) - $(field "$before" tier1) ))
  tier2f=$(( $(field "$after" t2fire) - $(field "$before" t2fire) ))
  node_it=$(( $(field "$after" node_iters) - $(field "$before" node_iters) ))
  node_at=$(( $(field "$after" node_att) - $(field "$before" node_att) ))
  node_ok_it=$(( $(field "$after" node_ok_iters) - $(field "$before" node_ok_iters) ))
  node_ok_at=$(( $(field "$after" node_ok_att) - $(field "$before" node_ok_att) ))
  steal_delta=$(( $(field "$after" steal_ns) - $(field "$before" steal_ns) ))
  local steal_ms; steal_ms=$(python3 -c "print(f'{$steal_delta/1e6:.2f}')")

  local full_it=$((node_it + node_ok_it)) full_at=$((node_at + node_ok_at))
  local iters_full="NA"
  [ "$full_at" -gt 0 ] && iters_full=$(python3 -c "print(f'{$full_it/$full_at:.4f}')")
  local exhaustions=$((node_at - tier1 - tier2f))

  log "DELTA arm=$label wall=${wall}s acquires=$acq_delta acq_per_s=$acq_per_s steal_ms=$steal_ms tier1=$tier1 tier2_fired=$tier2f node_attempts=$node_at exhaustions=$exhaustions iters_per_attempt_COMPLETE=$iters_full"

  ITERS_SUM[$label]=$(python3 -c "print(${ITERS_SUM[$label]:-0} + ($iters_full if '$iters_full'!='NA' else 0))")
  ITERS_N[$label]=$(( ${ITERS_N[$label]:-0} + 1 ))
  ACQ_PER_S_SUM[$label]=$(python3 -c "print(${ACQ_PER_S_SUM[$label]:-0} + ($acq_per_s if '$acq_per_s'!='NA' else 0))")
  ACQ_PER_S_N[$label]=$(( ${ACQ_PER_S_N[$label]:-0} + 1 ))
  STEAL_MS_SUM[$label]=$(python3 -c "print(${STEAL_MS_SUM[$label]:-0} + $steal_ms)")
  STEAL_MS_N[$label]=$(( ${STEAL_MS_N[$label]:-0} + 1 ))
  REARM_HALTS_SUM[$label]=$(( ${REARM_HALTS_SUM[$label]:-0} + exhaustions ))
  REARM_HALTS_N[$label]=$(( ${REARM_HALTS_N[$label]:-0} + 1 ))
}

FORWARD=(D G H1 H2 B1)
REVERSE=(B1 H2 H1 G D)

for rep in $(seq "$REPS"); do
  log ""
  log "=== round $rep/$REPS ==="
  if (( rep % 2 == 1 )); then order=("${FORWARD[@]}"); else order=("${REVERSE[@]}"); fi
  log "order this round: ${order[*]}"
  for arm in "${order[@]}"; do
    run_arm "$arm"
  done
done

log ""
log "=================================================================="
log "=== FINAL: acquisitions/s (the throughput metric that matters here) ==="
log "=================================================================="
for label in D G H1 H2 B1; do
  n=${ACQ_PER_S_N[$label]:-0}
  [ "$n" -gt 0 ] && log "$label: mean = $(python3 -c "print(f'{${ACQ_PER_S_SUM[$label]}/$n:.1f}')") acq/s (n=$n)"
done

log ""
log "=== mean node-path iters/attempt, COMPLETE metric ==="
for label in D G H1 H2 B1; do
  n=${ITERS_N[$label]:-0}
  [ "$n" -gt 0 ] && log "$label: mean = $(python3 -c "print(f'{${ITERS_SUM[$label]}/$n:.4f}')") (n=$n)"
done

log ""
log "=== mean exhaustion (re-arm-triggering) events per 12s window ==="
for label in D G H1 H2 B1; do
  n=${REARM_HALTS_N[$label]:-0}
  [ "$n" -gt 0 ] && log "$label: mean = $(python3 -c "print(f'{${REARM_HALTS_SUM[$label]}/$n:.1f}')") (n=$n)"
done

log ""
log "=== steal_ms per arm window ==="
for label in D G H1 H2 B1; do
  n=${STEAL_MS_N[$label]:-0}
  [ "$n" -gt 0 ] && log "$label: mean = $(python3 -c "print(f'{${STEAL_MS_SUM[$label]}/$n:.2f}')") ms (n=$n)"
done

sudo rmmod lhp_stress 2>/dev/null
log ""
log "Finished: $(date). Full log: $OUTDIR"
log ""
log "VALIDATION CHECK: does G's acq/s match D's within noise (~1%)? If not, GLOCK-12's"
log "attribution is incomplete and H1/H2 should not be trusted as tier-2's isolated effect yet."
