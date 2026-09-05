#!/bin/bash
# Phase 2: validate Phase 1's candidate threshold against real benchmarks,
# per Opus's plan review. dbench gets real rep budget (only one of the three
# with a realistic noise floor, 4.76%, to detect an effect at all). hackbench
# and ebizzy are reframed as MECHANISM-ENGAGEMENT diagnostics only -- do NOT
# try to detect a throughput difference in them (hackbench's 57.4% documented
# spread would need ~3250 reps/arm to resolve a 4% effect; ebizzy's post-
# restart spread of 15.4% is nearly as hopeless). Instead just check whether
# tier2 ever engages at all under each workload's contention shape.
#
# Usage: THRESH_US=<value> ./phase2_validation.sh
set -u

THRESH_US="${THRESH_US:?set THRESH_US to the Phase 1 candidate, in microseconds}"
THRESH_CYCLES=$(python3 -c "print(int($THRESH_US * 2200))")
DBENCH_REPS="${DBENCH_REPS:-15}"
MECH_REPS="${MECH_REPS:-2}"

STATE=/root/linux-6.17/cvm_setup/.state
SCRATCH=/tmp/claude-0/-root-linux-6-17/b98a4d93-d606-4bb7-bd13-7031a5eea896/scratchpad
SNAP_BT="$SCRATCH/cycle_snapshot_v2.bt"
TS=$(date +%Y%m%d_%H%M%S)
OUTDIR="$STATE/phase2_${TS}"
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

configure_stock() {
  set_sysctl ivh_pv_kick_unlock_ipi 1
  set_sysctl ivh_pv_kick_node_ipi 1
  set_sysctl ivh_pv_rearm_max 18446744073709551615
  set_sysctl ivh_pv_kick_pure_ipi 0
  set_sysctl ivh_pv_wait_mechanism 0
  set_sysctl ivh_pv_preempt_src 0
}

configure_ivh_h() {
  set_sysctl ivh_pv_wait_mechanism 2
  set_sysctl ivh_pv_kick_pure_ipi 1
  set_sysctl ivh_pv_kick_node_ipi 0
  set_sysctl ivh_pv_kick_unlock_ipi 1
  set_sysctl ivh_pv_rearm_max 0
  set_sysctl ivh_pv_preempt_src 2
  set_sysctl ivh_pv_beat_threshold "$THRESH_CYCLES"
}

log "Phase 2 validation started: $(date)"
log "Candidate threshold: ${THRESH_US}us (${THRESH_CYCLES} cycles)"
log "DBENCH_REPS=$DBENCH_REPS MECH_REPS=$MECH_REPS"

echo voluntary | sudo tee /sys/kernel/debug/sched/preempt > /dev/null

# --- PART A: dbench, real throughput comparison, interleaved+counterbalanced ---
log ""
log "=================================================================="
log "=== PART A: dbench -t 12 16 -- REAL throughput comparison ==="
log "=================================================================="

declare -A DB_SUM DB_N

run_dbench() {
  local label="$1"
  local v
  v=$(dbench -t 12 16 -D /root/dbench_test 2>&1 | grep '^Throughput' | awk '{print $2}')
  log "DBENCH label=$label MB/s=$v"
  DB_SUM[$label]=$(python3 -c "print(${DB_SUM[$label]:-0} + $v)")
  DB_N[$label]=$(( ${DB_N[$label]:-0} + 1 ))
}

for rep in $(seq "$DBENCH_REPS"); do
  if (( rep % 2 == 1 )); then
    configure_stock; run_dbench "D"
    configure_ivh_h; run_dbench "H"
  else
    configure_ivh_h; run_dbench "H"
    configure_stock; run_dbench "D"
  fi
done

log ""
log "-- dbench summary --"
for label in D H; do
  n=${DB_N[$label]:-0}
  [ "$n" -gt 0 ] && log "$label: mean = $(python3 -c "print(f'{${DB_SUM[$label]}/$n:.2f}')") MB/s (n=$n)"
done

# --- PART B: hackbench, mechanism-engagement diagnostic only ---
log ""
log "=================================================================="
log "=== PART B: hackbench -- mechanism-engagement diagnostic (NOT throughput) ==="
log "=================================================================="

run_mech_diag() {
  local label="$1" cmd="$2"
  local before after
  before=$(snap)
  local t0 t1
  t0=$(date +%s.%N)
  eval "$cmd" >> "$SUMMARY" 2>&1
  t1=$(date +%s.%N)
  after=$(snap)
  local wall; wall=$(python3 -c "print(f'{$t1-$t0:.3f}')")

  local tier1 t2c t2f node
  tier1=$(( $(field "$after" tier1) - $(field "$before" tier1) ))
  t2c=$(( $(field "$after" t2chk) - $(field "$before" t2chk) ))
  t2f=$(( $(field "$after" t2fire) - $(field "$before" t2fire) ))
  node=$(( $(field "$after" node) - $(field "$before" node) ))

  log "MECH label=$label wall=${wall}s tier1_fired=$tier1 tier2_checked=$t2c tier2_fired=$t2f node_halts=$node"
}

for rep in $(seq "$MECH_REPS"); do
  configure_stock
  run_mech_diag "hackbench_D_rep${rep}" "hackbench -T -g 1 -f 8 -l 400000"
  configure_ivh_h
  run_mech_diag "hackbench_H_rep${rep}" "hackbench -T -g 1 -f 8 -l 400000"
done

# --- PART C: ebizzy, mechanism-engagement diagnostic only ---
log ""
log "=================================================================="
log "=== PART C: ebizzy mmap 4MB -- mechanism-engagement diagnostic (NOT throughput) ==="
log "=================================================================="

for rep in $(seq "$MECH_REPS"); do
  configure_stock
  run_mech_diag "ebizzy_D_rep${rep}" "/home/nick/Desktop/ebizzy -S 20 -t 16 -m -s 4194304"
  configure_ivh_h
  run_mech_diag "ebizzy_H_rep${rep}" "/home/nick/Desktop/ebizzy -S 20 -t 16 -m -s 4194304"
done

log ""
log "Finished: $(date). Full log: $OUTDIR"
