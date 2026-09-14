#!/bin/bash
# v2 of the tier1-vs-tier2 spin-savings test, rebuilt per an independent Opus
# review of the v1 result (49x reduction claim). Findings addressed here:
#
#   1. Missing controls: v1 only had {tier1-only, tier2 100us, tier2 500us},
#      all under mechanism=2. That's IVH-vs-IVH, not IVH-vs-stock, and never
#      isolated "src=2's decision" from "everything else src!=0 changes"
#      (extra rdtsc, extra remote cacheline touches). Added:
#        - arm C (src=1, "shadow mode"): does all the heartbeat work but the
#          real decision still uses the (always-false) KVM bit, so it's an
#          exact isolation of tier-2's overhead without its effect.
#        - arm D (mechanism=0, kick_pure_ipi=0): real stock behavior -- halts
#          unconditionally on threshold exhaustion, no re-arm cascade, no
#          tier-2 consultation at all (pv_wait_early returns false immediately
#          when mechanism==0).
#   2. Order not counterbalanced: v1 always ran A,B1,B2 in that order every
#      round, so "runs first" and "is arm A" were perfectly confounded.
#      Reversed on even rounds here.
#   3. Round 1's 22900 outlier was a cold start unique to whichever arm ran
#      first ever (fresh insmod, fresh threads, near-zero system-wide
#      qspinlock activity) -- not host drift (acquisitions/s jumped and
#      stayed flat within 30s, contradicting a decaying-corunner theory).
#      Every arm-execution here gets an equally fresh rmmod/insmod + fresh
#      client, so a real cold-start effect should show up in EVERY arm's
#      first exposure, not just whichever ran first historically. If it
#      doesn't reproduce under this equal treatment, round 1 was noise.
#   4. GLOCK-10's node_iters_per_attempt metric silently excluded successful
#      (lock-acquired) passes -- this session's GLOCK-11 build adds the
#      missing ivh_node_spin_success_iters_sum/attempts counters. This script
#      reports BOTH the old bail-only metric (for continuity) and the new
#      complete metric (bail-only + success, the trustworthy one).
#   5. Independent host-load signal: rq->ivh_tks_steal_ns (accumulates
#      unconditionally regardless of ivh_steal_source) is snapshotted per arm
#      so a real host-contention difference between arms/rounds is now
#      directly measurable instead of inferred from acquisition-rate alone.
set -u

REPS="${REPS:-5}"
THREADS="${THREADS:-32}"
DURATION="${DURATION:-12}"
HOLD_US="${HOLD_US:-100}"
STATE=/root/linux-6.17/cvm_setup/.state
SCRATCH=/tmp/claude-0/-root-linux-6-17/b98a4d93-d606-4bb7-bd13-7031a5eea896/scratchpad
SNAP_BT="$SCRATCH/cycle_snapshot_v2.bt"
TS=$(date +%Y%m%d_%H%M%S)
OUTDIR="$STATE/t1_vs_t2_v2_${TS}"
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

# Safe cross-knob sequencing for ivh_pv_wait_mechanism / ivh_pv_kick_pure_ipi:
# kick_pure_ipi must be 0 before mechanism goes to 0; mechanism must move off
# 0 before kick_pure_ipi goes to 1. This ordering is safe from ANY prior state.
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

log "Tier1-vs-Tier2 v2 test started: $(date)"
log "THREADS=$THREADS DURATION=${DURATION}s HOLD_US=$HOLD_US REPS=$REPS"
log "Arms: A(mech2,src0) B1(mech2,src2,100us) B2(mech2,src2,500us) C(mech2,src1-shadow,100us) D(mech0,stock)"
log "Fresh rmmod/insmod + fresh client on EVERY arm-execution (cold-start equalized across arms)."
log "Order counterbalanced: forward on odd rounds, reversed on even rounds."

declare -A ITERS_OLD_SUM ITERS_OLD_N
declare -A ITERS_FULL_SUM ITERS_FULL_N
declare -A HEAD_ITERS_SUM HEAD_ITERS_N
declare -A ACQ_PER_S_SUM ACQ_PER_S_N
declare -A STEAL_MS_SUM STEAL_MS_N

configure_arm() {
  case "$1" in
    A)  set_pv 2 1; set_sysctl ivh_pv_preempt_src 0 ;;
    B1) set_pv 2 1; set_sysctl ivh_pv_preempt_src 2; set_sysctl ivh_pv_beat_threshold 220000 ;;
    B2) set_pv 2 1; set_sysctl ivh_pv_preempt_src 2; set_sysctl ivh_pv_beat_threshold 1100000 ;;
    C)  set_pv 2 1; set_sysctl ivh_pv_preempt_src 1; set_sysctl ivh_pv_beat_threshold 220000 ;;
    D)  set_pv 0 0; set_sysctl ivh_pv_preempt_src 0 ;;
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
  log "before: $before  acquires_before=$acq_before"

  local t0 t1_wall
  t0=$(date +%s.%N)
  /root/lhp_stress/stress_client "$THREADS" "$DURATION" >> "$SUMMARY" 2>&1
  t1_wall=$(date +%s.%N)

  after=$(snap)
  acq_after=$(acquires)
  log "after:  $after  acquires_after=$acq_after"

  local wall; wall=$(python3 -c "print(f'{$t1_wall-$t0:.3f}')")
  local acq_delta=$((acq_after - acq_before))
  local acq_per_s; acq_per_s=$(python3 -c "print(f'{$acq_delta/$wall:.1f}' if $wall>0 else 'NA')")

  local t1 t2c t2f node head node_it node_at node_ok_it node_ok_at head_it head_at steal_before steal_after steal_delta
  t1=$(( $(field "$after" tier1) - $(field "$before" tier1) ))
  t2c=$(( $(field "$after" t2chk) - $(field "$before" t2chk) ))
  t2f=$(( $(field "$after" t2fire) - $(field "$before" t2fire) ))
  node=$(( $(field "$after" node) - $(field "$before" node) ))
  head=$(( $(field "$after" head) - $(field "$before" head) ))
  node_it=$(( $(field "$after" node_iters) - $(field "$before" node_iters) ))
  node_at=$(( $(field "$after" node_att) - $(field "$before" node_att) ))
  node_ok_it=$(( $(field "$after" node_ok_iters) - $(field "$before" node_ok_iters) ))
  node_ok_at=$(( $(field "$after" node_ok_att) - $(field "$before" node_ok_att) ))
  head_it=$(( $(field "$after" head_iters) - $(field "$before" head_iters) ))
  head_at=$(( $(field "$after" head_att) - $(field "$before" head_att) ))
  steal_before=$(field "$before" steal_ns)
  steal_after=$(field "$after" steal_ns)
  steal_delta=$((steal_after - steal_before))
  local steal_ms; steal_ms=$(python3 -c "print(f'{$steal_delta/1e6:.2f}')")

  local iters_old="NA" iters_full="NA" head_iters_avg="NA"
  [ "$node_at" -gt 0 ] && iters_old=$(python3 -c "print(f'{$node_it/$node_at:.2f}')")
  local full_it=$((node_it + node_ok_it)) full_at=$((node_at + node_ok_at))
  [ "$full_at" -gt 0 ] && iters_full=$(python3 -c "print(f'{$full_it/$full_at:.4f}')")
  [ "$head_at" -gt 0 ] && head_iters_avg=$(python3 -c "print(f'{$head_it/$head_at:.2f}')")

  log "DELTA arm=$label wall=${wall}s acquires=$acq_delta acq_per_s=$acq_per_s steal_ms=$steal_ms tier1=$t1 tier2_checked=$t2c tier2_fired=$t2f node_halts=$node head_halts=$head node_attempts=$node_at node_success_attempts=$node_ok_at iters_per_attempt_BAILONLY=$iters_old iters_per_attempt_COMPLETE=$iters_full head_iters_per_attempt=$head_iters_avg"

  ITERS_OLD_SUM[$label]=$(python3 -c "print(${ITERS_OLD_SUM[$label]:-0} + ($iters_old if '$iters_old'!='NA' else 0))")
  ITERS_OLD_N[$label]=$(( ${ITERS_OLD_N[$label]:-0} + 1 ))
  ITERS_FULL_SUM[$label]=$(python3 -c "print(${ITERS_FULL_SUM[$label]:-0} + ($iters_full if '$iters_full'!='NA' else 0))")
  ITERS_FULL_N[$label]=$(( ${ITERS_FULL_N[$label]:-0} + 1 ))
  HEAD_ITERS_SUM[$label]=$(python3 -c "print(${HEAD_ITERS_SUM[$label]:-0} + ($head_iters_avg if '$head_iters_avg'!='NA' else 0))")
  HEAD_ITERS_N[$label]=$(( ${HEAD_ITERS_N[$label]:-0} + 1 ))
  ACQ_PER_S_SUM[$label]=$(python3 -c "print(${ACQ_PER_S_SUM[$label]:-0} + ($acq_per_s if '$acq_per_s'!='NA' else 0))")
  ACQ_PER_S_N[$label]=$(( ${ACQ_PER_S_N[$label]:-0} + 1 ))
  STEAL_MS_SUM[$label]=$(python3 -c "print(${STEAL_MS_SUM[$label]:-0} + $steal_ms)")
  STEAL_MS_N[$label]=$(( ${STEAL_MS_N[$label]:-0} + 1 ))
}

FORWARD=(A B1 B2 C D)
REVERSE=(D C B2 B1 A)

for rep in $(seq "$REPS"); do
  log ""
  log "=== round $rep/$REPS ==="
  if (( rep % 2 == 1 )); then
    order=("${FORWARD[@]}")
  else
    order=("${REVERSE[@]}")
  fi
  log "order this round: ${order[*]}"
  for arm in "${order[@]}"; do
    run_arm "$arm"
  done
done

log ""
log "=================================================================="
log "=== FINAL: mean node-path iters/attempt, BAIL-ONLY metric (old, biased) ==="
log "=================================================================="
for label in A B1 B2 C D; do
  n=${ITERS_OLD_N[$label]:-0}
  [ "$n" -gt 0 ] && log "$label: mean = $(python3 -c "print(f'{${ITERS_OLD_SUM[$label]}/$n:.2f}')") (n=$n)"
done

log ""
log "=================================================================="
log "=== FINAL: mean node-path iters/attempt, COMPLETE metric (bail+success, trustworthy) ==="
log "=================================================================="
for label in A B1 B2 C D; do
  n=${ITERS_FULL_N[$label]:-0}
  [ "$n" -gt 0 ] && log "$label: mean = $(python3 -c "print(f'{${ITERS_FULL_SUM[$label]}/$n:.4f}')") (n=$n)  [SPIN_THRESHOLD=32768]"
done

log ""
log "=== control: mean head-path iters/attempt (should be ~32768 always) ==="
for label in A B1 B2 C D; do
  n=${HEAD_ITERS_N[$label]:-0}
  [ "$n" -gt 0 ] && log "$label: mean = $(python3 -c "print(f'{${HEAD_ITERS_SUM[$label]}/$n:.2f}')") (n=$n)"
done

log ""
log "=== acquisitions/s (throughput -- expect ~flat if lock-duty-saturated) ==="
for label in A B1 B2 C D; do
  n=${ACQ_PER_S_N[$label]:-0}
  [ "$n" -gt 0 ] && log "$label: mean = $(python3 -c "print(f'{${ACQ_PER_S_SUM[$label]}/$n:.1f}')") acq/s (n=$n)"
done

log ""
log "=== steal_ms per arm window (independent host-contention signal) ==="
for label in A B1 B2 C D; do
  n=${STEAL_MS_N[$label]:-0}
  [ "$n" -gt 0 ] && log "$label: mean = $(python3 -c "print(f'{${STEAL_MS_SUM[$label]}/$n:.2f}')") ms (n=$n)"
done

sudo rmmod lhp_stress 2>/dev/null
log ""
log "Finished: $(date). Full log: $OUTDIR"
