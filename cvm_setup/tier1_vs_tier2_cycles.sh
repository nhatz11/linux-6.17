#!/bin/bash
# Direct cycle-savings test: does tier-2 (heartbeat early-bail) save real spin
# cycles beyond what tier-1 (predecessor-halted cascade) alone gets you?
#
# Uses lhp_stress (a single shared raw_spinlock_t, N pthreaded clients) instead
# of hackbench, for two reasons: (1) all contention concentrates on ONE lock,
# so ivh_lock_halt's un-split hlt_cycles/poll_cycles aggregate is actually
# meaningful here (not diluted across hundreds of unrelated kernel locks);
# (2) oversubscribing threads-to-vCPUs forces the GUEST scheduler to
# deschedule whichever thread is mid-spin on a vCPU, which produces the exact
# same heartbeat-staleness signature tier-2 is built to detect as real host
# preemption would -- a controllable proxy that doesn't require an external
# host-side corunner.
#
# Conditions (mechanism=2 / kick_pure_ipi=1 held constant throughout -- only
# src and threshold vary, so the wait/kick plumbing is identical in every
# arm and the ONLY variable is whether tier-2 can act):
#   A  : src=0  (is_wait_preempted always returns vcpu_is_preempted(), which
#        is always false in this CVM -- tier-2 permanently inert, this arm
#        is "tier-1 + cascade only", same mechanism=2 loop-back structure)
#   B1 : src=2, threshold=100us  (aggressive, above the ~65-260us heartbeat
#        publish-cadence noise floor)
#   B2 : src=2, threshold=500us  (more conservative, still far below the
#        500-20000us range that was already shown to be past the knee)
#
# Interleaved A,B1,B2 x REPS, not blocked, to control for drift the same way
# the earlier mechanism A/B tests did.
#
# Metric (GLOCK-10 correction): node_iters delta / node_attempts delta --
# average SPIN_THRESHOLD iterations actually burned busy-spinning in
# pv_wait_node() before each wait gave up on lock-free acquisition. This is
# the pre-halt phase; ivh_lock_halt (kept below for reference) only measures
# the post-halt phase and cannot show early-bail's effect at all. If tier-2
# is doing real work, B1/B2 should show a LOWER average than A.
set -u

REPS="${REPS:-3}"
THREADS="${THREADS:-32}"
DURATION="${DURATION:-12}"
HOLD_US="${HOLD_US:-100}"
STATE=/root/linux-6.17/cvm_setup/.state
SCRATCH=/tmp/claude-0/-root-linux-6-17/b98a4d93-d606-4bb7-bd13-7031a5eea896/scratchpad
SNAP_BT="$SCRATCH/cycle_snapshot_v2.bt"
TS=$(date +%Y%m%d_%H%M%S)
OUTDIR="$STATE/t1_vs_t2_${TS}"
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

snap() { sudo bpftrace "$SNAP_BT" 2>/dev/null | grep '^SNAP'; }
field() { echo "$1" | grep -oP "${2}=\K[0-9]+"; }
acquires() { cat /dev/lhp_stress 2>/dev/null || echo 0; }

# --- setup: module + constant mechanism/kick, only src/threshold vary ---
sudo rmmod lhp_stress 2>/dev/null
sudo insmod /root/lhp_stress/lhp_stress.ko hold_us=$HOLD_US
sleep 0.5
[ -e /dev/lhp_stress ] || { log "FATAL: /dev/lhp_stress did not appear after insmod"; exit 1; }

set_sysctl ivh_pv_wait_mechanism 2
set_sysctl ivh_pv_kick_pure_ipi 1
# "baggage-stripped" isolation used elsewhere in this investigation: no re-arm
# loop-back, no node-kick IPI, so the ONLY variable across arms is tier-2.
set_sysctl ivh_pv_rearm_max 0
set_sysctl ivh_pv_kick_node_ipi 0

log "Tier1-vs-Tier2 cycle test started: $(date)"
log "THREADS=$THREADS DURATION=${DURATION}s HOLD_US=$HOLD_US REPS=$REPS (interleaved A,B1,B2)"

declare -A CYC_PER_ACQ_SUM CYC_PER_ACQ_N
declare -A ITERS_PER_ATT_SUM ITERS_PER_ATT_N
declare -A HEAD_ITERS_PER_ATT_SUM HEAD_ITERS_PER_ATT_N
declare -A ALL_ITERS_SUM ALL_ITERS_N

run_arm() {
  local label="$1"
  log ""
  log "--- rep for arm: $label ---"
  sysctl -a 2>/dev/null | grep '^kernel\.ivh_pv' | tee -a "$SUMMARY"

  local before after acq_before acq_after
  before=$(snap)
  acq_before=$(acquires)
  log "before: $before  acquires_before=$acq_before"

  /root/lhp_stress/stress_client "$THREADS" "$DURATION" >> "$SUMMARY" 2>&1

  after=$(snap)
  acq_after=$(acquires)
  log "after:  $after  acquires_after=$acq_after"

  local acq_delta=$((acq_after - acq_before))
  local t1 t2c t2f node head hc pc he pe node_it node_at head_it head_at node_ok_it node_ok_at
  t1=$(( $(field "$after" tier1) - $(field "$before" tier1) ))
  t2c=$(( $(field "$after" t2chk) - $(field "$before" t2chk) ))
  t2f=$(( $(field "$after" t2fire) - $(field "$before" t2fire) ))
  node=$(( $(field "$after" node) - $(field "$before" node) ))
  head=$(( $(field "$after" head) - $(field "$before" head) ))
  hc=$(( $(field "$after" hlt_c) - $(field "$before" hlt_c) ))
  pc=$(( $(field "$after" poll_c) - $(field "$before" poll_c) ))
  he=$(( $(field "$after" hlt_e) - $(field "$before" hlt_e) ))
  pe=$(( $(field "$after" poll_e) - $(field "$before" poll_e) ))
  node_it=$(( $(field "$after" node_iters) - $(field "$before" node_iters) ))
  node_at=$(( $(field "$after" node_att) - $(field "$before" node_att) ))
  # GLOCK-11 denominator-completeness fix: the bail/exhaust population above
  # excludes every inner-loop pass that acquired lock-free. Sum both pairs for
  # an unbiased average over ALL pv_wait_node() inner-loop passes.
  node_ok_it=$(( $(field "$after" node_ok_iters) - $(field "$before" node_ok_iters) ))
  node_ok_at=$(( $(field "$after" node_ok_att) - $(field "$before" node_ok_att) ))
  local all_it=$((node_it + node_ok_it)) all_at=$((node_at + node_ok_at))
  local all_iters_per_att="NA"
  if [ "$all_at" -gt 0 ]; then
    all_iters_per_att=$(python3 -c "print(f'{$all_it/$all_at:.2f}')")
  fi
  head_it=$(( $(field "$after" head_iters) - $(field "$before" head_iters) ))
  head_at=$(( $(field "$after" head_att) - $(field "$before" head_att) ))

  local total_cyc=$((hc + pc))
  local cyc_per_acq="NA"
  if [ "$acq_delta" -gt 0 ]; then
    cyc_per_acq=$(python3 -c "print(f'{$total_cyc/$acq_delta:.2f}')")
  fi

  local iters_per_att="NA"
  if [ "$node_at" -gt 0 ]; then
    iters_per_att=$(python3 -c "print(f'{$node_it/$node_at:.2f}')")
  fi
  local head_iters_per_att="NA"
  if [ "$head_at" -gt 0 ]; then
    head_iters_per_att=$(python3 -c "print(f'{$head_it/$head_at:.2f}')")
  fi

  log "DELTA arm=$label acquires=$acq_delta tier1=$t1 tier2_checked=$t2c tier2_fired=$t2f node_halts=$node head_halts=$head hlt_cycles=$hc poll_cycles=$pc hlt_events=$he poll_events=$pe total_cycles=$total_cyc cycles_per_acquire=$cyc_per_acq node_iters_per_attempt=$iters_per_att head_iters_per_attempt=$head_iters_per_att node_attempts=$node_at head_attempts=$head_at node_ok_iters=$node_ok_it node_ok_attempts=$node_ok_at ALL_node_iters_per_pass=$all_iters_per_att"

  ALL_ITERS_SUM[$label]=$(python3 -c "print(${ALL_ITERS_SUM[$label]:-0} + ($all_iters_per_att if '$all_iters_per_att' != 'NA' else 0))")
  ALL_ITERS_N[$label]=$(( ${ALL_ITERS_N[$label]:-0} + 1 ))

  CYC_PER_ACQ_SUM[$label]=$(python3 -c "print(${CYC_PER_ACQ_SUM[$label]:-0} + ($cyc_per_acq if '$cyc_per_acq' != 'NA' else 0))")
  CYC_PER_ACQ_N[$label]=$(( ${CYC_PER_ACQ_N[$label]:-0} + 1 ))

  ITERS_PER_ATT_SUM[$label]=$(python3 -c "print(${ITERS_PER_ATT_SUM[$label]:-0} + ($iters_per_att if '$iters_per_att' != 'NA' else 0))")
  ITERS_PER_ATT_N[$label]=$(( ${ITERS_PER_ATT_N[$label]:-0} + 1 ))

  HEAD_ITERS_PER_ATT_SUM[$label]=$(python3 -c "print(${HEAD_ITERS_PER_ATT_SUM[$label]:-0} + ($head_iters_per_att if '$head_iters_per_att' != 'NA' else 0))")
  HEAD_ITERS_PER_ATT_N[$label]=$(( ${HEAD_ITERS_PER_ATT_N[$label]:-0} + 1 ))
}

for rep in $(seq "$REPS"); do
  log ""
  log "=== interleave round $rep/$REPS ==="

  set_sysctl ivh_pv_preempt_src 0
  run_arm "A_src0_tier1only"

  set_sysctl ivh_pv_preempt_src 2
  set_sysctl ivh_pv_beat_threshold 220000
  run_arm "B1_src2_100us"

  set_sysctl ivh_pv_beat_threshold 1100000
  run_arm "B2_src2_500us"
done

log ""
log "=================================================================="
log "=== FINAL: mean node-path spin-iterations/attempt by arm (n=$REPS each) ==="
log "=== (the corrected pre-halt metric -- this is the direct test) ==="
log "=================================================================="
for label in A_src0_tier1only B1_src2_100us B2_src2_500us; do
  n=${ITERS_PER_ATT_N[$label]:-0}
  if [ "$n" -gt 0 ]; then
    mean=$(python3 -c "print(f'{${ITERS_PER_ATT_SUM[$label]}/$n:.2f}')")
    log "$label: mean node_iters/attempt = $mean (n=$n)  [SPIN_THRESHOLD=32768]"
  fi
done

log ""
log "=== UNBIASED (GLOCK-11): mean node-path iters over ALL inner-loop passes ==="
for label in A_src0_tier1only B1_src2_100us B2_src2_500us; do
  n=${ALL_ITERS_N[$label]:-0}
  if [ "$n" -gt 0 ]; then
    mean=$(python3 -c "print(f'{${ALL_ITERS_SUM[$label]}/$n:.2f}')")
    log "$label: mean ALL node_iters/pass = $mean (n=$n)  [SPIN_THRESHOLD=32768]"
  fi
done

log ""
log "=== control: mean head-path spin-iterations/attempt (should be ~32768 always) ==="
for label in A_src0_tier1only B1_src2_100us B2_src2_500us; do
  n=${HEAD_ITERS_PER_ATT_N[$label]:-0}
  if [ "$n" -gt 0 ]; then
    mean=$(python3 -c "print(f'{${HEAD_ITERS_PER_ATT_SUM[$label]}/$n:.2f}')")
    log "$label: mean head_iters/attempt = $mean (n=$n)"
  fi
done

log ""
log "=================================================================="
log "=== reference (old, flawed-metric): mean cycles-per-acquisition by arm ==="
log "=================================================================="
for label in A_src0_tier1only B1_src2_100us B2_src2_500us; do
  n=${CYC_PER_ACQ_N[$label]:-0}
  if [ "$n" -gt 0 ]; then
    mean=$(python3 -c "print(f'{${CYC_PER_ACQ_SUM[$label]}/$n:.2f}')")
    log "$label: mean cycles/acquire = $mean (n=$n)"
  fi
done

sudo rmmod lhp_stress 2>/dev/null
log ""
log "Finished: $(date). Full log: $OUTDIR"
