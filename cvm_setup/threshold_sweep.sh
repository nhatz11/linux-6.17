#!/bin/bash
# Full benchmark rerun across all ivh_pv_beat_threshold sweep points, to find
# which threshold (if any) beats stock-mimicking tier-1-only PV (mechanism=0)
# on performance and/or overhead.
#
# "Cycle usage": TDX blocks HW PMU cycle counters to the guest (confirmed via
# `perf stat -a -e cycles` -> "<not supported>"), so there is no real guest
# cycle count available. We report perf software events instead:
#   - task-clock is ~elapsed_wallclock * 16 (this is a 16-vcpu box) and
#     carries no information beyond wall time -- NOT a cycle proxy, reported
#     for completeness only.
#   - context-switches / cpu-migrations ARE informative: every unnecessary
#     early-bail halt+wake round trip should show up here.
#   - ivh_lock_halt's hlt_cycles/poll_cycles are only populated when
#     ivh_pv_wait_mechanism==2 (mechanism 0/3 never call the accounting
#     begin/end). They are therefore only meaningfully comparable
#     THRESHOLD-vs-THRESHOLD, never against the mechanism=0 baseline (which
#     will structurally read zero deltas -- expected, not a bug, not "no
#     halting happened").
#
# Configs, in order:
#   1. baseline_mechanism0       {mech=0, ipi=0, src=0}
#   2. shadow_control            {mech=2, ipi=1, src=1, threshold=3.3M (1500us, arbitrary -- src=1 never acts on it)}
#      Matched control: identical mechanism=2 wait/kick machinery as every
#      threshold arm below, but src=1 computes the heartbeat verdict for
#      diagnostics only and returns the (always-0-in-this-CVM) KVM steal bit
#      -- i.e. tier-2 never actually fires. This isolates the early-bail's
#      real effect from the mechanism/kick_pure_ipi wake-path change, which
#      the naive baseline-vs-threshold comparison would otherwise confound.
#   3-12. threshold_<us>us       {mech=2, ipi=1, src=2, threshold=us*2200 cycles}
#         500/1000/1500/2000/3000/4000/6000/8000/12000/20000 us.
#   13. baseline_mechanism0_repeat  same as #1, run last, to bound host-side
#       drift across the ~50min sweep window.
#
# Sysctl write-order safety (verified against kvm.c's ivh_pv_reject_unsafe_combo
# gate this session): kick_pure_ipi must go to 0 BEFORE mechanism goes to 0;
# mechanism must move away from 0 BEFORE kick_pure_ipi goes to 1. Every
# transition below follows this. set_sysctl reads back every write and
# hard-aborts on mismatch (preempt_src=2 is genuinely rejectable if any
# online CPU has never published a heartbeat -- a silent rejection would
# leave all 10 threshold arms running at src=0/1 behavior and produce a
# worthless flat result).
set -u

REPS=2
STATE=/root/linux-6.17/cvm_setup/.state
SCRATCH=/tmp/claude-0/-root-linux-6-17/b98a4d93-d606-4bb7-bd13-7031a5eea896/scratchpad
HALT_BT="$SCRATCH/halt_snapshot.bt"
TS=$(date +%Y%m%d_%H%M%S)
OUTDIR="$STATE/sweep_${TS}"
mkdir -p "$OUTDIR"
SUMMARY="$OUTDIR/SUMMARY.txt"
: > "$SUMMARY"

log() { echo "$@" | tee -a "$SUMMARY"; }

set_sysctl() {
  echo "$2" | sudo tee "/proc/sys/kernel/$1" > /dev/null
  local got
  got=$(cat "/proc/sys/kernel/$1")
  if [ "$got" != "$2" ]; then
    log "FATAL: write to $1 rejected: wanted $2, got $got -- aborting sweep, no data past this point is valid"
    exit 1
  fi
}

# Capture pre-run live state so we can restore it on any exit (normal,
# aborted, or timeout-killed child -- a killed benchmark does not kill this
# script, so the trap only needs to cover script-level exit).
ORIG_MECH=$(cat /proc/sys/kernel/ivh_pv_wait_mechanism)
ORIG_IPI=$(cat /proc/sys/kernel/ivh_pv_kick_pure_ipi)
ORIG_SRC=$(cat /proc/sys/kernel/ivh_pv_preempt_src)
ORIG_THRESH=$(cat /proc/sys/kernel/ivh_pv_beat_threshold)

restore_state() {
  echo "Restoring pre-sweep sysctl state: mech=$ORIG_MECH ipi=$ORIG_IPI src=$ORIG_SRC threshold=$ORIG_THRESH" | tee -a "$SUMMARY"
  # order-safe regardless of current state: move mechanism off 0 before ipi=1,
  # or ipi to 0 before mechanism=0, matching whichever ORIG_MECH needs.
  if [ "$ORIG_MECH" = "0" ]; then
    echo 0 | sudo tee /proc/sys/kernel/ivh_pv_kick_pure_ipi > /dev/null
    echo 0 | sudo tee /proc/sys/kernel/ivh_pv_wait_mechanism > /dev/null
  else
    echo "$ORIG_MECH" | sudo tee /proc/sys/kernel/ivh_pv_wait_mechanism > /dev/null
    echo "$ORIG_IPI" | sudo tee /proc/sys/kernel/ivh_pv_kick_pure_ipi > /dev/null
  fi
  echo "$ORIG_SRC" | sudo tee /proc/sys/kernel/ivh_pv_preempt_src > /dev/null
  echo "$ORIG_THRESH" | sudo tee /proc/sys/kernel/ivh_pv_beat_threshold > /dev/null
}
trap restore_state EXIT

halt_snapshot() {
  local out
  out=$(sudo bpftrace "$HALT_BT" 2>/dev/null | grep HALT_SNAPSHOT)
  if [ -z "$out" ]; then
    log "WARN: halt snapshot FAILED (empty bpftrace output) -- treat this config's halt_delta as UNKNOWN, not zero"
  fi
  echo "$out"
}

parse_halt() {
  # $1 = "HALT_SNAPSHOT hlt_cycles=X hlt_events=Y poll_cycles=Z poll_events=W", $2 = field name
  echo "$1" | grep -oP "${2}=\K[0-9]+"
}

run_config() {
  local label="$1"
  log ""
  log "=================================================================="
  log "=== CONFIG: $label ==="
  log "=================================================================="
  sysctl -a 2>/dev/null | grep '^kernel\.ivh_pv' | tee -a "$SUMMARY"

  local h_before h_after
  h_before=$(halt_snapshot)
  log "halt_before: ${h_before:-<empty>}"

  local perf_out="$OUTDIR/perf_${label}.txt"
  local bench_out="$OUTDIR/bench_${label}.log"

  timeout -k 30 900 perf stat -a -e task-clock,context-switches,cpu-migrations \
      -o "$perf_out" -- \
      /root/linux-6.17/cvm_setup/run_baseline.sh "$REPS" > "$bench_out" 2>&1
  local rc=$?
  [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ] && log "WARN: config $label TIMED OUT (900s) -- results incomplete/missing"

  h_after=$(halt_snapshot)
  log "halt_after:  ${h_after:-<empty>}"

  if [ -n "$h_before" ] && [ -n "$h_after" ]; then
    local hb_c hb_e pb_c pb_e ha_c ha_e pa_c pa_e
    hb_c=$(parse_halt "$h_before" hlt_cycles); hb_e=$(parse_halt "$h_before" hlt_events)
    pb_c=$(parse_halt "$h_before" poll_cycles); pb_e=$(parse_halt "$h_before" poll_events)
    ha_c=$(parse_halt "$h_after" hlt_cycles);  ha_e=$(parse_halt "$h_after" hlt_events)
    pa_c=$(parse_halt "$h_after" poll_cycles);  pa_e=$(parse_halt "$h_after" poll_events)
    log "halt_delta: hlt_cycles=$((ha_c - hb_c)) hlt_events=$((ha_e - hb_e)) poll_cycles=$((pa_c - pb_c)) poll_events=$((pa_e - pb_e))"
  else
    log "halt_delta: UNKNOWN (a snapshot failed, see WARN above)"
  fi

  log "--- perf stat (system-wide, whole $REPS-rep suite; task-clock is NOT a cycle proxy, see header) ---"
  grep -E 'task-clock|context-switches|cpu-migrations|seconds time elapsed' "$perf_out" | tee -a "$SUMMARY"

  log "--- benchmark summary (tail of run_baseline.sh output) ---"
  grep -A20 '=== Summary' "$bench_out" | tee -a "$SUMMARY"
}

log "Threshold sweep started: $(date)"
log "REPS=$REPS per config. Pre-run live state: mech=$ORIG_MECH ipi=$ORIG_IPI src=$ORIG_SRC threshold=$ORIG_THRESH"

# --- 1. Baseline: mechanism=0 (tier-1 only, stock-mimicking). ---
set_sysctl ivh_pv_kick_pure_ipi 0
set_sysctl ivh_pv_wait_mechanism 0
set_sysctl ivh_pv_preempt_src 0
run_config "baseline_mechanism0"

# --- 2. Shadow-mode matched control: same mech=2/ipi=1 wake machinery as
# every threshold arm, but src=1 never lets tier-2 change behavior. ---
set_sysctl ivh_pv_wait_mechanism 2
set_sysctl ivh_pv_kick_pure_ipi 1
set_sysctl ivh_pv_preempt_src 1
set_sysctl ivh_pv_beat_threshold 3300000
run_config "shadow_control_mechanism2_src1"

# --- 3-12. Threshold sweep: mechanism=2, preempt_src=2 (REAL early-bail). ---
for us in 500 1000 1500 2000 3000 4000 6000 8000 12000 20000; do
  cycles=$((us * 2200))
  set_sysctl ivh_pv_wait_mechanism 2
  set_sysctl ivh_pv_kick_pure_ipi 1
  set_sysctl ivh_pv_preempt_src 2
  set_sysctl ivh_pv_beat_threshold "$cycles"
  run_config "threshold_${us}us"
done

# --- 13. Repeat baseline to bound host-side drift across the sweep window. ---
set_sysctl ivh_pv_kick_pure_ipi 0
set_sysctl ivh_pv_wait_mechanism 0
set_sysctl ivh_pv_preempt_src 0
run_config "baseline_mechanism0_repeat"

log ""
log "Threshold sweep finished: $(date)"
log "Full per-config logs in: $OUTDIR"
