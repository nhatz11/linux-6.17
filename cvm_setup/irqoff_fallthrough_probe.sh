#!/bin/bash
# irqoff_fallthrough_probe.sh -- ONE targeted question:
#
#   Under hackbench, how often does ivh_pv_wait() with wait_mechanism=2 hit the
#   `irqs_disabled()` fall-through (arch/x86/kernel/kvm.c:1661) and busy-poll for
#   up to IVH_PV_ADAPTIVE_TSC (3,000,000 cycles = 1.36 ms @ tsc_khz=2200000)
#   instead of taking a real halt?
#
# ivh_lock_halt (arch/x86/include/asm/ivh_tsc_beat.h:253) splits exactly that:
#   hlt_events/hlt_cycles  <- ivh_lock_halt_begin(false): a REAL halt()/safe_halt()
#   poll_events/poll_cycles<- ivh_lock_halt_begin(true) : the bounded PAUSE poll
#
# For mechanism 0 (arm D) the poll bucket is UNREACHABLE (kvm.c:1552-1582 returns
# before it whenever KVM_FEATURE_PV_UNHALT is present, which it is here).  For
# mechanism 2 (arm G0) the poll bucket is reachable ONLY via the irqs_disabled()
# fall-through.  So poll_events under G0 is a direct, unambiguous count of
# "wait that mechanism 0 would have slept through but mechanism 2 burned CPU on".
#
# Contrast arm: lhp_stress takes its lock with a plain raw_spin_lock() (no
# irqsave, /root/lhp_stress/lhp_stress.c:25), so it should show poll_events ~ 0.
#
# Read-only w.r.t. everything except the ivh sysctls, which are saved and
# restored.  No reboot, no host access.
set -u

REPS="${REPS:-3}"
HB_ARGS="${HB_ARGS:--T -g 1 -f 8 -l 50000}"
LHP_SECS="${LHP_SECS:-12}"
LHP_THREADS="${LHP_THREADS:-32}"
LHP_HOLD_US="${LHP_HOLD_US:-100}"

CVM=/root/linux-6.17/cvm_setup
STATE=$CVM/.state
LOCKFILE=$CVM/.ivh_sysctl.lock
SCRATCH=/tmp/claude-0/-root-linux-6-17/b98a4d93-d606-4bb7-bd13-7031a5eea896/scratchpad
SNAP_BT="$SCRATCH/cycle_snapshot_v2.bt"      # has hlt_e/poll_e/hlt_c/poll_c
TS=$(date +%Y%m%d_%H%M%S)
OUTDIR="$STATE/irqoff_probe_${TS}"
mkdir -p "$OUTDIR"
SUMMARY="$OUTDIR/SUMMARY.txt"
: > "$SUMMARY"

log() { echo "$@" | tee -a "$SUMMARY"; }
fatal() { log "FATAL: $*"; exit 1; }

[ -r "$SNAP_BT" ] || fatal "missing $SNAP_BT"

KNOBS="ivh_pv_wait_mechanism ivh_pv_kick_pure_ipi ivh_pv_kick_node_ipi ivh_pv_kick_unlock_ipi ivh_pv_rearm_max ivh_pv_preempt_src ivh_pv_beat_threshold ivh_pv_beat_publish_mask"

set_sysctl() {
  echo "$2" | sudo tee "/proc/sys/kernel/$1" > /dev/null
  local got; got=$(cat "/proc/sys/kernel/$1")
  [ "$got" = "$2" ] || fatal "write to $1 rejected: wanted $2, got $got"
}

snap() {
  local out; out=$(sudo bpftrace "$SNAP_BT" 2>/dev/null | grep '^SNAP')
  case "$out" in *hlt_e=*poll_e=*) echo "$out" ;; *) return 1 ;; esac
}
field() { echo "$1" | grep -oP "(^| )${2}=\K[0-9]+" | head -1; }

# ---- save / restore -------------------------------------------------------
declare -A SAVED
save_state() { local k; for k in $KNOBS; do SAVED[$k]=$(cat /proc/sys/kernel/$k); done; }
restore_state() {
  # wait_mechanism must be nonzero before kick_pure_ipi=1 can be set, and
  # kick_pure_ipi must be 0 before wait_mechanism=0 -- honour both orders.
  set_sysctl ivh_pv_preempt_src 0
  if [ "${SAVED[ivh_pv_wait_mechanism]}" = "0" ]; then
    set_sysctl ivh_pv_kick_pure_ipi 0
    set_sysctl ivh_pv_wait_mechanism 0
  else
    set_sysctl ivh_pv_wait_mechanism "${SAVED[ivh_pv_wait_mechanism]}"
    set_sysctl ivh_pv_kick_pure_ipi "${SAVED[ivh_pv_kick_pure_ipi]}"
  fi
  local k
  for k in ivh_pv_kick_node_ipi ivh_pv_kick_unlock_ipi ivh_pv_rearm_max \
           ivh_pv_beat_threshold ivh_pv_beat_publish_mask ivh_pv_preempt_src; do
    set_sysctl "$k" "${SAVED[$k]}"
  done
  log "sysctls restored"
}

# ---- arms (identical to hackbench_tier2_isolation.sh's D and G0) -----------
configure_D() {
  set_sysctl ivh_pv_preempt_src 0
  set_sysctl ivh_pv_kick_pure_ipi 0
  set_sysctl ivh_pv_wait_mechanism 0
  set_sysctl ivh_pv_kick_unlock_ipi 1
  set_sysctl ivh_pv_kick_node_ipi 1
  set_sysctl ivh_pv_rearm_max 18446744073709551615
}
configure_G0() {
  set_sysctl ivh_pv_preempt_src 0
  set_sysctl ivh_pv_wait_mechanism 2
  set_sysctl ivh_pv_kick_pure_ipi 1
  set_sysctl ivh_pv_kick_node_ipi 0
  set_sysctl ivh_pv_kick_unlock_ipi 1
  set_sysctl ivh_pv_rearm_max 0
  set_sysctl ivh_pv_beat_publish_mask 4095
}

report() {   # $1=label $2=before $3=after $4=wall $5=extra
  local b="$2" a="$3"
  local he pe hc pc nd hd
  he=$(( $(field "$a" hlt_e)  - $(field "$b" hlt_e) ))
  pe=$(( $(field "$a" poll_e) - $(field "$b" poll_e) ))
  hc=$(( $(field "$a" hlt_c)  - $(field "$b" hlt_c) ))
  pc=$(( $(field "$a" poll_c) - $(field "$b" poll_c) ))
  nd=$(( $(field "$a" node)   - $(field "$b" node) ))
  hd=$(( $(field "$a" head)   - $(field "$b" head) ))
  python3 - "$1" "$4" "$he" "$pe" "$hc" "$pc" "$nd" "$hd" "$5" <<'EOF' | tee -a "$SUMMARY"
import sys
lab, wall, he, pe, hc, pc, nd, hd, extra = sys.argv[1:10]
he,pe,hc,pc,nd,hd = map(int,(he,pe,hc,pc,nd,hd))
KHZ = 2200000.0   # dmesg: tsc_khz=2200000
def us(c,n): return (c/KHZ*1000.0)/n if n else 0.0
tot = he+pe
print(f"RESULT arm={lab} wall={wall}s node_halts={nd} head_halts={hd} "
      f"hlt_events={he} poll_events={pe} poll_share={100.0*pe/tot if tot else 0:.2f}% "
      f"hlt_us_total={hc/KHZ/1000:.3f}s poll_us_total={pc/KHZ/1000:.3f}s "
      f"mean_hlt_us={us(hc,he):.2f} mean_poll_us={us(pc,pe):.2f} {extra}")
EOF
}

run_hackbench() {
  local label="$1" before after t0 t1 wall hb
  before=$(snap) || fatal "snap failed (before $label)"
  t0=$(date +%s.%N)
  hb=$(hackbench $HB_ARGS 2>&1 | grep -oP '^Time: \K[0-9.]+')
  t1=$(date +%s.%N)
  after=$(snap) || fatal "snap failed (after $label)"
  wall=$(python3 -c "print(f'{$t1-$t0:.3f}')")
  report "$label" "$before" "$after" "$wall" "hbtime=${hb}s"
}

trap 'restore_state' EXIT

exec 9>"$LOCKFILE"
flock -w 900 9 || fatal "could not acquire $LOCKFILE"

save_state
log "irqoff fall-through probe: $(date)"
log "HB_ARGS='$HB_ARGS' REPS=$REPS"
log "saved sysctls: $(for k in $KNOBS; do printf '%s=%s ' $k ${SAVED[$k]}; done)"
log ""
log "PREDICTION: arm D (mechanism 0) poll_events == 0 by construction."
log "            arm G0 (mechanism 2) poll_events > 0 iff hackbench's contended"
log "            locks are taken with IRQs disabled -- kvm.c:1661 refuses to halt"
log "            there and burns up to 3e6 TSC cycles (1.36 ms) of PAUSE instead."
log ""

for r in $(seq 1 "$REPS"); do
  log "--- round $r ---"
  configure_D;  run_hackbench "D_r$r"
  configure_G0; run_hackbench "G0_r$r"
done

# ---- contrast workload: lhp_stress takes a plain raw_spin_lock (IRQs ON) ---
log ""
log "--- lhp_stress contrast (raw_spin_lock, no irqsave -> expect poll_events ~ 0) ---"
if [ -f /root/lhp_stress/lhp_stress.ko ]; then
  sudo rmmod lhp_stress 2>/dev/null
  if sudo insmod /root/lhp_stress/lhp_stress.ko hold_us=$LHP_HOLD_US 2>/dev/null; then
    sleep 0.5
    if [ -e /dev/lhp_stress ]; then
      configure_G0
      before=$(snap) || fatal "snap failed (lhp before)"
      /root/lhp_stress/stress_client "$LHP_THREADS" "$LHP_SECS" >> "$SUMMARY" 2>&1
      after=$(snap) || fatal "snap failed (lhp after)"
      report "G0_lhp_stress" "$before" "$after" "$LHP_SECS" "workload=lhp_stress"
    else
      log "SKIP lhp_stress: /dev/lhp_stress did not appear"
    fi
    sudo rmmod lhp_stress 2>/dev/null
  else
    log "SKIP lhp_stress: insmod failed"
  fi
else
  log "SKIP lhp_stress: module not built"
fi

log ""
log "done: $(date)  ->  $SUMMARY"
