#!/bin/bash
# Does IVH match stock's actual wake mechanism too, not just its detection
# ceiling? D uses the real PV_UNHALT hypercall exclusively (confirmed
# available on this host). H2 (GLOCK-12's validated fix) still uses an IPI
# at the unlock-time kick site (ivh_pv_kick_unlock_ipi defaults to 1).
# Arm I is H2 with that knob also set to 0, forcing the SAME hypercall-only
# wake stock uses at both sites -- the closest IVH can get to being
# architecturally identical to stock in every respect except tier-2's
# detection logic riding on top for free.
set -u

REPS="${REPS:-5}"
THREADS="${THREADS:-32}"
DURATION="${DURATION:-12}"
HOLD_US="${HOLD_US:-100}"
STATE=/root/linux-6.17/cvm_setup/.state
SCRATCH=/tmp/claude-0/-root-linux-6-17/b98a4d93-d606-4bb7-bd13-7031a5eea896/scratchpad
SNAP_BT="$SCRATCH/cycle_snapshot_v2.bt"
TS=$(date +%Y%m%d_%H%M%S)
OUTDIR="$STATE/pv_parity_${TS}"
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

log "PV parity test started: $(date)"
log "THREADS=$THREADS DURATION=${DURATION}s HOLD_US=$HOLD_US REPS=$REPS"
log "D = stock (hypercall wake only, by construction)"
log "H2 = GLOCK-12 fixed IVH: kick_node_ipi=0, rearm_max=0, src=2/500us, kick_unlock_ipi=1 (IPI still used at unlock)"
log "I  = same as H2 but kick_unlock_ipi=0 (forces hypercall-only wake, matching stock's wake mechanism exactly)"

declare -A ACQ_SUM ACQ_N
declare -A ITERS_SUM ITERS_N

configure_arm() {
  case "$1" in
    D)  set_pv 0 0
        set_sysctl ivh_pv_preempt_src 0 ;;
    H2) set_pv 2 1
        set_sysctl ivh_pv_kick_node_ipi 0
        set_sysctl ivh_pv_kick_unlock_ipi 1
        set_sysctl ivh_pv_rearm_max 0
        set_sysctl ivh_pv_preempt_src 2
        set_sysctl ivh_pv_beat_threshold 1100000 ;;
    I)  set_pv 2 1
        set_sysctl ivh_pv_kick_node_ipi 0
        set_sysctl ivh_pv_kick_unlock_ipi 0
        set_sysctl ivh_pv_rearm_max 0
        set_sysctl ivh_pv_preempt_src 2
        set_sysctl ivh_pv_beat_threshold 1100000 ;;
  esac
}

run_arm() {
  local label="$1"
  log ""
  log "--- arm: $label ---"

  sudo rmmod lhp_stress 2>/dev/null
  sudo insmod /root/lhp_stress/lhp_stress.ko hold_us=$HOLD_US
  sleep 0.5
  [ -e /dev/lhp_stress ] || { log "FATAL: /dev/lhp_stress did not appear"; exit 1; }

  configure_arm "$label"
  sysctl -a 2>/dev/null | grep '^kernel\.ivh_pv' | tee -a "$SUMMARY"

  local before after acq_before acq_after
  before=$(snap)
  acq_before=$(acquires)

  local t0 t1
  t0=$(date +%s.%N)
  /root/lhp_stress/stress_client "$THREADS" "$DURATION" >> "$SUMMARY" 2>&1
  t1=$(date +%s.%N)

  after=$(snap)
  acq_after=$(acquires)

  local wall; wall=$(python3 -c "print(f'{$t1-$t0:.3f}')")
  local acq_delta=$((acq_after - acq_before))
  local acq_per_s; acq_per_s=$(python3 -c "print(f'{$acq_delta/$wall:.1f}')")

  local node_it node_at node_ok_it node_ok_at
  node_it=$(( $(field "$after" node_iters) - $(field "$before" node_iters) ))
  node_at=$(( $(field "$after" node_att) - $(field "$before" node_att) ))
  node_ok_it=$(( $(field "$after" node_ok_iters) - $(field "$before" node_ok_iters) ))
  node_ok_at=$(( $(field "$after" node_ok_att) - $(field "$before" node_ok_att) ))
  local full_it=$((node_it + node_ok_it)) full_at=$((node_at + node_ok_at))
  local iters_full="NA"
  [ "$full_at" -gt 0 ] && iters_full=$(python3 -c "print(f'{$full_it/$full_at:.4f}')")

  log "DELTA arm=$label wall=${wall}s acquires=$acq_delta acq_per_s=$acq_per_s iters_per_attempt=$iters_full"

  ACQ_SUM[$label]=$(python3 -c "print(${ACQ_SUM[$label]:-0} + $acq_per_s)")
  ACQ_N[$label]=$(( ${ACQ_N[$label]:-0} + 1 ))
  ITERS_SUM[$label]=$(python3 -c "print(${ITERS_SUM[$label]:-0} + ($iters_full if '$iters_full'!='NA' else 0))")
  ITERS_N[$label]=$(( ${ITERS_N[$label]:-0} + 1 ))
}

FORWARD=(D H2 I)
REVERSE=(I H2 D)

for rep in $(seq "$REPS"); do
  log ""
  log "=== round $rep/$REPS ==="
  if (( rep % 2 == 1 )); then order=("${FORWARD[@]}"); else order=("${REVERSE[@]}"); fi
  for arm in "${order[@]}"; do run_arm "$arm"; done
done

log ""
log "=== FINAL: acq/s ==="
for label in D H2 I; do
  n=${ACQ_N[$label]:-0}
  [ "$n" -gt 0 ] && log "$label: mean = $(python3 -c "print(f'{${ACQ_SUM[$label]}/$n:.1f}')") acq/s (n=$n)"
done
log ""
log "=== FINAL: iters/attempt ==="
for label in D H2 I; do
  n=${ITERS_N[$label]:-0}
  [ "$n" -gt 0 ] && log "$label: mean = $(python3 -c "print(f'{${ITERS_SUM[$label]}/$n:.4f}')") (n=$n)"
done

sudo rmmod lhp_stress 2>/dev/null
log ""
log "Finished: $(date). Full log: $OUTDIR"
