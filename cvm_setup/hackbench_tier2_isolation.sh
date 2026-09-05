#!/bin/bash
# Tier-2 (TSC-heartbeat early-bail) isolation on hackbench, replacing the
# lhp_stress probe used in t1_vs_t2_v3.sh / tier1_vs_tier2_cycles.sh.
#
# WHY hackbench, not lhp_stress: two independent audits (2026-09-01) found
# tier-2's decision share on lhp_stress is only 0.01-0.08% of node_halts --
# tier-1 (prev fully halted) shadows it almost completely on a single shared
# lock where every waiter is queued behind an already-halted predecessor.
# On hackbench (many locks, shallower queues), the SAME kernel build showed
# tier-2 with real decision share. A single calibration run on 2026-09-01
# (mech=2, src=2, 500us, rearm_max=0) measured, per hackbench run:
#   node_iters+node_ok_iters = 1.233e10, t2fire = 39733, tier1 = 350572,
#   halt_from_node = 468394  ->  tier-2 decided 8.5% of node halts, and its
#   THEORETICAL CEILING on the primary metric is 39733 * SPIN_THRESHOLD
#   (32768) / 1.233e10 = 10.6%. A lower threshold (100us) fires more often
#   and has a proportionally larger ceiling. So this probe can, in principle,
#   resolve a real effect -- unlike lhp_stress, where the ceiling was ~0.05%.
#
# WHY NOT phase2_validation.sh's existing hackbench arms: (a) its D-to-H
# comparison flips FOUR sysctls at once (wait_mechanism, kick_pure_ipi,
# kick_node_ipi, preempt_src) -- only the last is tier-2, so a D-vs-H delta
# there cannot be attributed to tier-2 specifically; a same-day audit proved
# this by reproducing phase2's node_halts collapse with tier-2 PROVABLY OFF
# (a G arm: mechanism=2, tier-2 off). (b) phase2 used MECH_REPS=2 with no
# randomization -- its own header says hackbench's ~57% spread needs ~3250
# reps/arm to resolve a 4% wall-clock effect, and a same-day rerun of its
# exact D/H config did not replicate (flipped sign entirely). This script
# fixes both: real controls isolate tier-2's marginal effect against a
# same-mechanism baseline, not against stock; and arm order is randomized
# every round with drift-based round rejection (pattern borrowed from
# root_cause_isolation.sh) instead of a fixed sequential D-then-H loop.
#
# THE TWO CONTROLS, and why there are two (added 2026-09-01 in review of the
# first draft, which had only G0):
#   ivh_pv_preempt_src != 0 does not only change the tier-2 VERDICT. Reading
#   qspinlock_paravirt.h in the RUNNING kernel's tree
#   (/root/kernels/linux-6.17-vanilla, NOT /root/linux-6.17 which is stale),
#   src != 0 also switches on real work that src == 0 skips entirely:
#     - pv_init_node():          an rdtsc + heartbeat store on EVERY
#                                qspinlock slowpath entry;
#     - ivh_beat_publish_in_spin(): an rdtsc + store every
#                                (ivh_pv_beat_publish_mask+1) = 4096 spin
#                                iterations;
#     - is_wait_preempted():     an ivh_beat_age() rdtsc on every 256th
#                                spin iteration.
#   That instrumentation SLOWS each spin iteration, which by itself lowers
#   the iteration count accumulated during a fixed real-time wait -- i.e. it
#   biases the primary metric in the SAME direction a real tier-2 win would.
#   Comparing H against src==0 alone therefore cannot separate the two.
#   So:
#     G0  src=0: no heartbeat instrumentation at all, tier-2 off.
#     G1  src=1: ALL of the above instrumentation active (and then some --
#         src==1 additionally does the vcpu_is_preempted() read, an ilog2
#         and two histogram increments that src==2 skips), but the value
#         RETURNED to pv_wait_early() is the KVM steal bit, not the
#         heartbeat verdict. On this TDX guest the KVM bit is architecturally
#         dead (always false), so tier-2 provably never fires in G1 while
#         paying strictly MORE per-check cost than H does.
#   H-minus-G1 is therefore the conservative, instrumentation-cost-matched
#   estimate of tier-2's marginal effect; H-minus-G0 is the optimistic one.
#   If G1-minus-G0 lands near zero, the instrumentation is free and the two
#   estimates should agree -- that agreement is the thing to check before
#   believing either. G1 also directly re-proves the dead-steal-bit premise:
#   at src==1 the kernel accumulates the 2x2 agreement matrix, so
#   (agree_true + false_neg) == 0 means the host never once said "preempted".
#
# PLACEBO METRIC: head_iters (ivh_head_spin_iters_sum, the queue-HEAD spin
# path in pv_wait_head_or_lock). is_wait_preempted() is called from exactly
# one place -- pv_wait_early(), which is only reached from pv_wait_node() --
# so tier-2 CANNOT affect the head path by construction. If head_iters moves
# across arms with the same sign and relative magnitude as node_iters, the
# "effect" is systemic drift, not tier-2. Reported alongside the primary.
#
# WHY a lockfile: the SAME-DAY cycles measurement got silently corrupted
# mid-run by a second, concurrently-running IVH test script mutating the same
# global sysctls -- nothing in this test family took a lock on shared kernel
# state before today. This script takes one. It does NOT protect against an
# older sibling script that hasn't been updated to take the same lock;
# pgrep-check those by name as a best-effort backstop.
#
# NO HOST ACCESS: unlike root_cause_isolation.sh's host_wait_ms() (SSH to
# the host to read schedstat), this script's drift rejection is purely
# in-guest (round-mean wall-clock vs running median), by design -- this
# investigation is scoped to guest-only, no-reboot changes.
#
# PRIMARY METRIC: total node-path spin iterations (node_iters + node_ok_iters,
# the "COMPLETE" metric from t1_vs_t2_v3.sh), NOT normalized per-acquisition.
# hackbench's workload (fixed message/loop count) is identical across arms,
# so a raw iteration-count delta across arms IS a direct measure of "how much
# extra kernel-side spin/wait work was needed to service the same amount of
# application-level lock traffic" -- i.e. a direct total-wait-time proxy.
# Wall-clock and tier1/tier2 decision counts are logged as secondary/
# diagnostic, not the primary claim.
#
# Arms (order randomized every round):
#   D   true stock: mechanism=0, kick_pure_ipi=0 (set BEFORE wait_mechanism=0
#       -- VERIFIED against the running kernel's source: both
#       ivh_pv_proc_wait_mechanism() and ivh_pv_proc_kick_pure_ipi() in
#       arch/x86/kernel/kvm.c return -EINVAL for the {mech==0,kpi==1}
#       combination, in EITHER write order, so the safe knob must move
#       first in both directions), kick_node_ipi=1, kick_unlock_ipi=1,
#       rearm_max=unbounded, preempt_src=0.
#   G0  mechanism=2, kick_pure_ipi=1, kick_node_ipi=0, kick_unlock_ipi=1,
#       rearm_max=0 (halt on first exhaustion, exactly like stock), tier-2
#       OFF and heartbeat instrumentation OFF (preempt_src=0). Validates
#       against D (t1_vs_t2_v3 found this config within ~0.4% of D on
#       lhp_stress).
#   G1  same as G0 but preempt_src=1: instrumentation ON, verdict still the
#       (dead) KVM bit. The cost-matched control.
#   H1  same as G0 but preempt_src=2, beat_threshold=220000 (100us).
#   H2  same as G0 but preempt_src=2, beat_threshold=1100000 (500us).
#
# Usage: TARGET_CLEAN=6 ./hackbench_tier2_isolation.sh
#   PILOT:  TARGET_CLEAN=2 HACKBENCH_ARGS='-T -g 1 -f 8 -l 100000' ...
#   (one hackbench at the default -l 400000 takes ~104s on this guest, so a
#    round of 5 arms is ~9 min and TARGET_CLEAN=6 is ~55 min.)
#
# ===== RESULT, first full run: .state/hackbench_tier2_20260901_052413 =====
# 6/6 clean rounds, TARGET_CLEAN=6, -T -g 1 -f 8 -l 400000. Paired within round.
#   node_iters_total  H1-G1  -27.98%  t=-12.82  6/6 rounds negative
#   bail_iters/pass   H1-G1  -57.07%  t=-24.13  6/6 rounds negative
#   node spin attempts H1-G1 +0.56%   t=  0.42  -> NOT a contention artifact
#   VALIDATION G1-G0 (instr) -2.64%   t= -1.90  -> instrumentation ~free
#   placebo head_iters H1-G1 -6.81%   t= -0.65  -> does not co-move
#   ceiling check: -27.98% observed vs 35.25% theoretical max. Passes.
#   wall_s H1-G1 -6.10% t=-0.78 (4/6) -> favorable but NOT significant.
# H2 (500us) shows the same sign an order of magnitude smaller (-2.35% /
# -8.42%), i.e. a threshold dose-response, which corroborates the mechanism.
# CAVEAT that must travel with this number: node_iters_total is a SPIN-WORK
# metric, not a throughput metric, and this same run proves they can move in
# opposite directions -- stock D has +54% node_iters_total vs G0 while being
# 7.5% FASTER in wall-clock. So "tier-2 cuts node-path spin work by 28%" is
# established; "tier-2 makes hackbench faster" is not.
# ==========================================================================
set -u

TARGET_CLEAN="${TARGET_CLEAN:-6}"
MAX_REPS="${MAX_REPS:-16}"
HACKBENCH_ARGS="${HACKBENCH_ARGS:--T -g 1 -f 8 -l 400000}"
THRESH_CYCLES_1="${THRESH_CYCLES_1:-220000}"   # 100us @ tsc_khz=2200000
THRESH_CYCLES_2="${THRESH_CYCLES_2:-1100000}"  # 500us
PUBLISH_MASK="${PUBLISH_MASK:-4095}"           # kernel default 0xfff; pinned
                                               # explicitly so a leftover
                                               # value from a previous run
                                               # can't silently change the
                                               # heartbeat's freshness.
ARMS="${ARMS:-D G0 G1 H1 H2}"

STATE=/root/linux-6.17/cvm_setup/.state
SCRATCH=/tmp/claude-0/-root-linux-6-17/b98a4d93-d606-4bb7-bd13-7031a5eea896/scratchpad
SNAP_BT="${SNAP_BT:-$SCRATCH/cycle_snapshot_v3.bt}"
LOCKFILE=/root/linux-6.17/cvm_setup/.ivh_sysctl.lock
TS=$(date +%Y%m%d_%H%M%S)
ROOT="$STATE/hackbench_tier2_${TS}"
mkdir -p "$ROOT"
SUMMARY="$ROOT/SUMMARY.txt"
PAIRED_CSV="$ROOT/paired_rounds.csv"
: > "$SUMMARY"
ROUND_MEANS_FILE="$ROOT/round_means.txt"
: > "$ROUND_MEANS_FILE"

log() { echo "$@" | tee -a "$SUMMARY"; }
fatal() { log "FATAL: $*"; exit 1; }

# CSV header, generated from $ARMS so adding/removing an arm needs no edit here.
{
  hdr="round"
  for a in $ARMS; do
    hdr="$hdr,${a}_wall,${a}_hbtime,${a}_iters,${a}_att,${a}_bailit,${a}_bailatt,${a}_okit,${a}_okatt,${a}_head,${a}_tier1,${a}_t2f,${a}_t2c,${a}_nhalt,${a}_hhalt,${a}_hltc,${a}_hlte"
  done
  echo "$hdr"
} > "$PAIRED_CSV"

# --- mutual exclusion: refuse to run alongside another instance of this
# family of scripts. Best-effort for old siblings (pgrep by name); hard
# guarantee only for scripts that also flock this same LOCKFILE. ---
exec 9>"$LOCKFILE"
if ! flock -n 9; then
  fatal "$LOCKFILE is held by another process -- another IVH sysctl test is likely running. Aborting rather than corrupting both runs."
fi
# Match sibling SCRIPT FILENAMES, not bare tokens: the first draft's bare
# 'threshold_' pattern matches any shell whose command line merely MENTIONS
# it (it matched this agent's own diagnostic shell during review). Exclude
# our own process tree explicitly as well.
# Our own ancestor chain: the wrapper shell that launched this script has our
# filename on ITS command line too, so a bare "not $$" exclusion self-trips.
SELF_PIDS=" "
_p=$$
while [ "$_p" -gt 1 ] 2>/dev/null; do
  SELF_PIDS="$SELF_PIDS$_p "
  # strip through the last ')' first: comm can contain spaces/parens.
  _p=$(sed 's/.*) //' "/proc/$_p/stat" 2>/dev/null | cut -d' ' -f2) || break
  [ -n "$_p" ] || break
done
SELF_PGID=$(ps -o pgid= -p $$ 2>/dev/null | tr -d ' ')
for name in 'tier1_vs_tier2[a-z0-9_]*\.sh' 'phase2_validation\.sh' 'root_cause_isolation\.sh' 'threshold_[a-z0-9_]*\.sh' 'pv_parity_test\.sh' 'hackbench_tier2_isolation\.sh'; do
  for pid in $(pgrep -f "$name" 2>/dev/null); do
    case "$SELF_PIDS" in *" $pid "*) continue ;; esac
    # Same process group = a sibling of this very pipeline (the wrapper shell
    # forked for `script | tee`, say), not a competing test run.
    pgid=$(ps -o pgid= -p "$pid" 2>/dev/null | tr -d ' ')
    [ -n "$pgid" ] && [ "$pgid" = "$SELF_PGID" ] && continue
    cl=$( (tr '\0' ' ' < "/proc/$pid/cmdline") 2>/dev/null )
    # Raced away between pgrep and now -- it cannot be mutating sysctls.
    [ -z "$cl" ] && continue
    fatal "a process matching '$name' is already running (pid $pid: $cl). Aborting -- it does not hold $LOCKFILE so flock alone can't see it."
  done
done

set_sysctl() {
  echo "$2" | sudo tee "/proc/sys/kernel/$1" > /dev/null 2>&1
  local got; got=$(cat "/proc/sys/kernel/$1")
  if [ "$got" != "$2" ]; then
    fatal "write to $1 rejected: wanted $2, got $got (check dmesg for the IVH refusal reason)"
  fi
}

snap() {
  local out; out=$(sudo bpftrace "$SNAP_BT" 2>/dev/null | grep '^SNAP')
  case "$out" in
    *node_iters=*head_iters=*) echo "$out" ;;
    *) return 1 ;;
  esac
}
# Anchored so e.g. 'node' cannot match inside 'node_iters'.
field() { echo "$1" | grep -oP "(^| )${2}=\K[0-9]+" | head -1; }

# --- arm configurations ---------------------------------------------------
# Ordering rules enforced below, both verified against the running kernel's
# arch/x86/kernel/kvm.c:
#   * entering D:  kick_pure_ipi=0 BEFORE wait_mechanism=0.
#   * leaving  D:  wait_mechanism=2 BEFORE kick_pure_ipi=1.
#   * beat_threshold is set BEFORE preempt_src=2, never after, so src==2 is
#     never briefly live with the other arm's threshold.
configure_D() {
  set_sysctl ivh_pv_preempt_src 0
  set_sysctl ivh_pv_kick_pure_ipi 0
  set_sysctl ivh_pv_wait_mechanism 0
  set_sysctl ivh_pv_kick_unlock_ipi 1
  set_sysctl ivh_pv_kick_node_ipi 1
  set_sysctl ivh_pv_rearm_max 18446744073709551615
  set_sysctl ivh_universal_eligible 0
}

configure_mech2_base() {   # everything except preempt_src / beat_threshold
  set_sysctl ivh_pv_preempt_src 0
  set_sysctl ivh_pv_wait_mechanism 2
  set_sysctl ivh_pv_kick_pure_ipi 1
  set_sysctl ivh_pv_kick_node_ipi 0
  set_sysctl ivh_pv_kick_unlock_ipi 1
  set_sysctl ivh_pv_rearm_max 0
  set_sysctl ivh_pv_beat_publish_mask "$PUBLISH_MASK"
  set_sysctl ivh_universal_eligible 0
}

configure_G0() { configure_mech2_base; }
configure_G1() {
  configure_mech2_base
  set_sysctl ivh_pv_beat_threshold "$THRESH_CYCLES_1"
  set_sysctl ivh_pv_preempt_src 1
}
configure_H1() {
  configure_mech2_base
  set_sysctl ivh_pv_beat_threshold "$THRESH_CYCLES_1"
  set_sysctl ivh_pv_preempt_src 2
}
configure_H2() {
  configure_mech2_base
  set_sysctl ivh_pv_beat_threshold "$THRESH_CYCLES_2"
  set_sysctl ivh_pv_preempt_src 2
}
configure_H1K() {   # H1 but head-wake uses the hypercall (kick_pure_ipi=0), same as D
  configure_H1
  set_sysctl ivh_pv_kick_pure_ipi 0
}
configure_G0K() {   # G0 (tier-2 off) but wake uses the latching hypercall, not the IPI.
  # Isolates the "sticky vs lossy wake" hypothesis from tier-2 entirely: G0K
  # differs from G0 in exactly kick_pure_ipi. If per-halt cycle cost collapses
  # toward D's, the wake vehicle -- not tier-2, not halt count -- is the driver.
  configure_G0
  set_sysctl ivh_pv_kick_pure_ipi 0
}
configure_G0N() {   # G0 but the node-unlock IPI is also restored (kick_node_ipi=1)
  configure_G0
  set_sysctl ivh_pv_kick_node_ipi 1
}
configure_D_MIG() {    # D + IVH proactive migration on (vcap/MY_ivh_atc must already be running)
  configure_D
  set_sysctl ivh_universal_eligible 1
}
configure_H1_MIG() {   # H1 + IVH proactive migration on
  configure_H1
  set_sysctl ivh_universal_eligible 1
}

configure_arm() {
  case "$1" in
    D)      configure_D ;;
    G0)     configure_G0 ;;
    G1)     configure_G1 ;;
    H1)     configure_H1 ;;
    H2)     configure_H2 ;;
    H1K)    configure_H1K ;;
    G0K)    configure_G0K ;;
    G0N)    configure_G0N ;;
    D_MIG)  configure_D_MIG ;;
    H1_MIG) configure_H1_MIG ;;
    *)      fatal "unknown arm '$1'" ;;
  esac
}

run_arm() {
  local label="$1" before after hb
  before=$(snap) || fatal "bpftrace snapshot failed (before, arm $label)"
  local t0 t1
  t0=$(date +%s.%N)
  hb=$(hackbench $HACKBENCH_ARGS 2>&1 | tee -a "$SUMMARY" | grep -oP '^Time: \K[0-9.]+')
  t1=$(date +%s.%N)
  after=$(snap) || fatal "bpftrace snapshot failed (after, arm $label)"
  [ -n "$hb" ] || fatal "could not parse hackbench 'Time:' line (arm $label)"
  local wall; wall=$(python3 -c "print(f'{$t1-$t0:.3f}')")

  # The node spin path has TWO disjoint populations, and only one of them is
  # the one tier-2 can act on. Keep them separate, not just their sum:
  #
  #   BAIL/EXHAUST (ivh_node_spin_iters_sum / _attempts): passes that stopped
  #     spinning without acquiring -- pv_wait_early() fired (tier-1 OR tier-2)
  #     or the SPIN_THRESHOLD budget ran out. With rearm_max=0 each of these
  #     ends in a halt. This is EXACTLY the population tier-2 acts on: firing
  #     early converts an ~SPIN_THRESHOLD-iteration exhaustion into a short
  #     pass. bailit/bailatt is therefore the sharpest available signal, and
  #     its mean has a hard, known upper bound of SPIN_THRESHOLD = 32768.
  #
  #   SUCCESS (ivh_node_spin_success_iters_sum / _attempts): passes that saw
  #     node->locked and returned. pv_wait_early()'s verdict never ends one of
  #     these, so tier-2 can only touch them second-order (through changed
  #     queue dynamics). Effectively a second placebo channel.
  #
  # node_iters_total (the sum) stays the headline metric, but a move in the
  # sum that is NOT visible in bailit/bailatt is not a tier-2 mechanism story.
  LAST_BAILIT=$(( $(field "$after" node_iters) - $(field "$before" node_iters) ))
  LAST_BAILATT=$(( $(field "$after" node_att) - $(field "$before" node_att) ))
  LAST_OKIT=$(( $(field "$after" node_ok_iters) - $(field "$before" node_ok_iters) ))
  LAST_OKATT=$(( $(field "$after" node_ok_att) - $(field "$before" node_ok_att) ))
  LAST_ITERS=$((LAST_BAILIT + LAST_OKIT))
  LAST_ATT=$((LAST_BAILATT + LAST_OKATT))
  LAST_HEAD=$(( $(field "$after" head_iters) - $(field "$before" head_iters) ))
  LAST_T1F=$(( $(field "$after" tier1) - $(field "$before" tier1) ))
  LAST_T2F=$(( $(field "$after" t2fire) - $(field "$before" t2fire) ))
  LAST_T2C=$(( $(field "$after" t2chk) - $(field "$before" t2chk) ))
  LAST_NHALT=$(( $(field "$after" node) - $(field "$before" node) ))
  # Halts at the queue HEAD (pv_wait_head_or_lock -> ivh_halt_from_head),
  # counted separately from mid-queue node halts above. This is the split
  # needed to bound the max benefit of a head-scoped-halt gate before
  # building one: idea 2 in ivh_two_enhancement_designs_2026-09-04.md.
  LAST_HHALT=$(( $(field "$after" head) - $(field "$before" head) ))
  # Cumulative real-HLT cycles/events (ivh_lock_halt.hlt_cycles/hlt_events,
  # arch/x86/include/asm/ivh_tsc_beat.h) across ALL halt sites (head+node),
  # both real halt mechanisms (mech 0's PV_UNHALT halt and mech 2's scoped
  # halt both populate this). hlt_c/hlt_e gives mean per-halt cost directly
  # -- the "sticky vs lossy wake" test needs THIS, not wall-clock: see
  # ivh_two_enhancement_designs_2026-09-04.md and the 2026-09-05 test-plan.
  LAST_HLTC=$(( $(field "$after" hlt_c) - $(field "$before" hlt_c) ))
  LAST_HLTE=$(( $(field "$after" hlt_e) - $(field "$before" hlt_e) ))
  LAST_WALL="$wall"; LAST_HB="$hb"

  # src==1 only: 2x2 agreement matrix vs the host's steal bit.
  local agt fn
  agt=$(( $(field "$after" agree_true) - $(field "$before" agree_true) ))
  fn=$(( $(field "$after" false_neg) - $(field "$before" false_neg) ))
  local hostsaid=$((agt + fn))

  local bpp; bpp=$(python3 -c "print(f'{$LAST_BAILIT/max($LAST_BAILATT,1):.0f}')")
  local hltmean; hltmean=$(python3 -c "print(f'{${LAST_HLTC}/max(${LAST_HLTE},1)/2200:.1f}')")
  log "DELTA arm=$label wall=${wall}s hbtime=${hb}s node_iters_total=$LAST_ITERS bail=${LAST_BAILIT}/${LAST_BAILATT}(${bpp}/pass, max 32768) ok=${LAST_OKIT}/${LAST_OKATT} head_iters=$LAST_HEAD tier1=$LAST_T1F t2fire=$LAST_T2F t2chk=$LAST_T2C node_halts=$LAST_NHALT head_halts=$LAST_HHALT hlt_cycles=$LAST_HLTC/$LAST_HLTE(${hltmean}us/halt) host_said_preempted=$hostsaid"
}

running_median_file() {
  python3 -c "
import statistics
vals = [float(l) for l in open('$1') if l.strip()]
print(statistics.median(vals) if vals else 0)
"
}

echo voluntary | sudo tee /sys/kernel/debug/sched/preempt > /dev/null

log "hackbench tier-2 isolation started: $(date)"
log "kernel: $(uname -r)   source of record: /root/kernels/linux-6.17-vanilla"
log "TARGET_CLEAN=$TARGET_CLEAN MAX_REPS=$MAX_REPS HACKBENCH_ARGS='$HACKBENCH_ARGS'"
log "THRESH_CYCLES_1=$THRESH_CYCLES_1 THRESH_CYCLES_2=$THRESH_CYCLES_2 PUBLISH_MASK=$PUBLISH_MASK"
log "ARMS='$ARMS' -- D(stock) G0(mech2,src0) G1(mech2,src1=cost-matched control) H1(src2@100us) H2(src2@500us)"
log "results dir: $ROOT"

NARMS=$(echo $ARMS | wc -w)
clean=0
total=0
rep=0

while [ "$clean" -lt "$TARGET_CLEAN" ] && [ "$rep" -lt "$MAX_REPS" ]; do
  rep=$((rep+1))
  order=$(python3 -c "import random; a='$ARMS'.split(); random.shuffle(a); print(' '.join(a))")

  declare -A rw rhb ri ra rbi rba roi roa rh rt1 rt2f rt2c rnh rhh rhltc rhlte
  for arm in $order; do
    configure_arm "$arm"
    run_arm "$arm"
    rw[$arm]="$LAST_WALL"; rhb[$arm]="$LAST_HB"; ri[$arm]="$LAST_ITERS"; ra[$arm]="$LAST_ATT"
    rbi[$arm]="$LAST_BAILIT"; rba[$arm]="$LAST_BAILATT"; roi[$arm]="$LAST_OKIT"; roa[$arm]="$LAST_OKATT"
    rh[$arm]="$LAST_HEAD"; rt1[$arm]="$LAST_T1F"
    rt2f[$arm]="$LAST_T2F"; rt2c[$arm]="$LAST_T2C"; rnh[$arm]="$LAST_NHALT"; rhh[$arm]="$LAST_HHALT"
    rhltc[$arm]="$LAST_HLTC"; rhlte[$arm]="$LAST_HLTE"
  done

  sum=0
  for arm in $order; do sum=$(python3 -c "print($sum + ${rw[$arm]})"); done
  round_mean=$(python3 -c "print($sum/$NARMS)")

  # Within-round wall-clock spread. The 2026-09-01 pilot saw a 3.3x spread
  # for IDENTICAL work inside a single round (9.4s .. 31.4s), which is far
  # larger than any plausible arm effect -- so this is logged on every round
  # as a first-class quality number, and used post-hoc to weight/exclude
  # rounds rather than silently thrown away. NOT an automatic rejection: an
  # arm effect would also widen the spread, so auto-rejecting on it could
  # select against the very thing being measured.
  spread=$(python3 -c "
v=[$(for arm in $ARMS; do printf '%s,' "${rw[$arm]}"; done)]
print(f'{max(v)/min(v):.2f}')")

  med=$(running_median_file "$ROUND_MEANS_FILE")
  round_bad=0
  if [ "$(python3 -c "print(1 if $med > 0 else 0)")" = "1" ]; then
    round_bad=$(python3 -c "print(1 if ($round_mean < 0.5*$med or $round_mean > 2*$med) else 0)")
  fi

  total=$((total+1))
  if [ "$round_bad" = "1" ]; then
    log "  ** ROUND $rep FLAGGED BAD (wall-clock drift): round_mean=${round_mean}s median=${med}s order=[$order] **"
  else
    clean=$((clean+1))
    log "  round $rep CLEAN (n_clean now $clean/$TARGET_CLEAN) round_mean=${round_mean}s within_round_wall_spread=${spread}x order=[$order]"
    line="  node_iters_total:"; for a in $ARMS; do line="$line $a=${ri[$a]}"; done; log "$line"
    line="  bail_iters/pass (tier-2's own population, max 32768):"; for a in $ARMS; do line="$line $a=$(python3 -c "print(f'{${rbi[$a]}/max(${rba[$a]},1):.0f}')")"; done; log "$line"
    line="  head_iters(placebo):"; for a in $ARMS; do line="$line $a=${rh[$a]}"; done; log "$line"
    line="  wall_s:"; for a in $ARMS; do line="$line $a=${rw[$a]}"; done; log "$line"
    line="  t2fire/node_halts:"; for a in $ARMS; do line="$line $a=${rt2f[$a]}/${rnh[$a]}"; done; log "$line"
    line="  node_halts/head_halts (split, idea-2 ceiling check):"; for a in $ARMS; do line="$line $a=${rnh[$a]}/${rhh[$a]}"; done; log "$line"
    line="  us/halt (hlt_cycles/hlt_events/2200, sticky-wake test):"; for a in $ARMS; do line="$line $a=$(python3 -c "print(f'{${rhltc[$a]}/max(${rhlte[$a]},1)/2200:.1f}')")"; done; log "$line"
    echo "$round_mean" >> "$ROUND_MEANS_FILE"
    row="$rep"
    for a in $ARMS; do
      row="$row,${rw[$a]},${rhb[$a]},${ri[$a]},${ra[$a]},${rbi[$a]},${rba[$a]},${roi[$a]},${roa[$a]},${rh[$a]},${rt1[$a]},${rt2f[$a]},${rt2c[$a]},${rnh[$a]},${rhh[$a]},${rhltc[$a]},${rhlte[$a]}"
    done
    echo "$row" >> "$PAIRED_CSV"
  fi

  unset rw rhb ri ra rbi rba roi roa rh rt1 rt2f rt2c rnh rhh rhltc rhlte
done

log ""
log "=================================================================="
log "=== FINISHED: $clean clean rounds out of $total attempted (target $TARGET_CLEAN) ==="
log "=================================================================="

python3 /root/linux-6.17/cvm_setup/hackbench_tier2_stats.py "$PAIRED_CSV" 2>&1 | tee -a "$SUMMARY"

log ""
log "Finished: $(date). Full log + paired_rounds.csv: $ROOT"
log "A negative H-minus-G on node_iters_total (H doing LESS total node-path spin work than the SAME mechanism without tier-2) is the signature of a real tier-2 win. Before believing it: (1) G0-minus-D must be near zero, (2) G1-minus-G0 must be near zero or the instrumentation cost is confounding H, (3) head_iters (placebo) must NOT move the same way, (4) the effect must be under the theoretical ceiling t2fire*32768/node_iters_total."
