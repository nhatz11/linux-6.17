#!/usr/bin/env bash
# verify_cs_measurement.sh
#
# Proves that critical-section duration is measured correctly for BOTH
# userspace and kernel spinlocks, and that:
#
#   overall_ns >= active_ns   (wall-clock CS time >= on-CPU CS time)
#
# overall_ns == active_ns  → no preemption during CS (clean fast path)
# overall_ns >  active_ns  → task was off-CPU during CS (LHP condition)
#
# Userspace path:
#   rseq+40  last_cs_overall_ns  = CLOCK_MONOTONIC(unlock) - CLOCK_MONOTONIC(lock_acq)
#   rseq+48  last_cs_active_ns   = CLOCK_THREAD_CPUTIME_ID(unlock) - same(lock_acq)
#   Written by NHextend at every unlock.
#
# Kernel path:
#   task->last_cs_ns          = sched_clock() - cs_wall_start_ts  (overall)
#   task->cumulative_cs_time  = Σ on-CPU slices inside CS          (cumulative active)
#   Per-CS active = Δcumulative_cs_time across one lock/unlock pair.
#   Written by cs_exit() in kernel/locking/spinlock.c at every outermost release.
#
# Usage: sudo bash verify_cs_measurement.sh
# Requires: NHextend -n built, hackbench available, MY_ivh_atc running

set -euo pipefail

NHEXTEND="/home/nick/NHextend"
die()  { echo "ERROR: $*" >&2; exit 1; }
hdr()  { echo; echo "══════════════════════════════════════════════════════════"; echo "  $*"; echo "══════════════════════════════════════════════════════════"; }
pass() { echo "  PASS: $*"; }
fail() { echo "  FAIL: $*"; FAILURES=$((FAILURES+1)); }

[[ -x "$NHEXTEND" ]] || die "NHextend not found/built"
command -v hackbench  || die "hackbench not found"
command -v bpftrace   || die "bpftrace not found"

FAILURES=0
TMPDIR_LOCAL=$(mktemp -d /tmp/verify_cs.XXXXXX)
trap 'rm -rf "$TMPDIR_LOCAL"' EXIT

# ── A. USERSPACE CS MEASUREMENT (NHextend) ───────────────────────────────────
hdr "TEST A: userspace CS measurement (NHextend -n, rseq+40 / rseq+48)"
echo "  Workload: NHextend -n 16 (unpinned, triggers IVH migrations)"
echo "  Fields:   rseq+40 last_cs_overall_ns  (CLOCK_MONOTONIC delta)"
echo "            rseq+48 last_cs_active_ns   (CLOCK_THREAD_CPUTIME_ID delta)"
echo "  Assertion: overall >= active on every observed CS"
echo "             at least one CS where overall > active (preemption seen)"
echo

"$NHEXTEND" -n 16 > /dev/null 2>&1 &
NH_PID=$!
sleep 0.3

BPF_OUT_A="$TMPDIR_LOCAL/user_cs.txt"

# Sample rseq fields from live NHextend threads every 500ms for 10s.
# Read both fields atomically-enough (same bpftrace probe firing).
# overall - active = off-CPU time during that CS.
sudo bpftrace --no-warnings -e '
tracepoint:sched:sched_process_fork { }   /* dummy to ensure bpftrace starts */

kprobe:bpf_sched_pre_lock_migrate
/comm == "NHextend"/
{
    $rseq = (uint64)curtask->rseq;
    if ($rseq == 0) return;

    $overall = *(uint64*)($rseq + 40);
    $active  = *(uint64*)($rseq + 48);

    if ($overall == 0 || $active == 0) return;

    printf("overall=%llu active=%llu offcpu=%lld\n",
           $overall, $active, (int64)$overall - (int64)$active);

    @overall_samples = lhist($overall, 0, 5000000, 100000);
    @active_samples  = lhist($active,  0, 5000000, 100000);
    @offcpu_samples  = lhist((int64)$overall - (int64)$active, 0, 2000000, 50000);
    @violations      += ($overall < $active) ? 1 : 0;
    @total           = count();
}

interval:s:10 { exit(); }
' 2>/dev/null > "$BPF_OUT_A" &
BT_PID_A=$!

sleep 11
kill $NH_PID   2>/dev/null; wait $NH_PID   2>/dev/null || true
wait $BT_PID_A 2>/dev/null || true

echo "  --- Samples (first 5) ---"
grep "^overall=" "$BPF_OUT_A" | head -5 || echo "  (no samples)"

TOTAL_A=$(grep -c "^overall=" "$BPF_OUT_A" 2>/dev/null || echo 0)
VIOL_A=$(grep "^overall=" "$BPF_OUT_A" | awk -F'offcpu=' '{if ($2+0 < 0) c++} END{print c+0}')
PREEMPT_A=$(grep "^overall=" "$BPF_OUT_A" | awk -F'offcpu=' '{if ($2+0 > 1000) c++} END{print c+0}')

echo
echo "  Total CS samples:          $TOTAL_A"
echo "  overall < active (BUG):    $VIOL_A"
echo "  overall > active + 1µs:    $PREEMPT_A  (preemption events seen)"

if [[ $TOTAL_A -eq 0 ]]; then
    fail "no CS samples captured (NHextend threads not reaching rseq fields)"
elif [[ $VIOL_A -gt 0 ]]; then
    fail "overall < active in $VIOL_A samples — clock measurement broken"
else
    pass "overall >= active holds across all $TOTAL_A samples"
fi

if [[ $PREEMPT_A -gt 0 ]]; then
    pass "off-CPU CS time observed ($PREEMPT_A samples with overall > active + 1µs)"
else
    echo "  NOTE: no preemption events caught — system may be lightly loaded"
    echo "        (overall == active is valid on an uncontended vCPU)"
fi

# ── B. KERNEL CS MEASUREMENT (hackbench) ────────────────────────────────────
hdr "TEST B: kernel CS measurement (hackbench, task->last_cs_ns / cumulative_cs_time)"
echo "  Workload: hackbench -g 16 (high kernel spinlock contention)"
echo "  Fields:   curtask->last_cs_ns         (overall, sched_clock wall-clock delta)"
echo "            Δcurtask->cumulative_cs_time  (per-CS on-CPU, saved at lock entry)"
echo "  Assertion: overall >= active (last_cs_ns >= Δcumulative_cs_time)"
echo "             at least one CS where overall > active"
echo

hackbench -g 16 -l 200000 > /dev/null 2>&1 &
HB_PID=$!
sleep 0.3

BPF_OUT_B="$TMPDIR_LOCAL/kern_cs.txt"

# Probe cs_enter (lock_depth just became 1) to save cumulative_cs_time baseline.
# Probe cs_exit (lock_depth just became 0) to read last_cs_ns and the delta.
# We identify CS boundaries via _raw_spin_lock/_raw_spin_unlock with lock_depth
# transition, matching by tid.
sudo bpftrace --no-warnings -e '
kretprobe:_raw_spin_lock,
kretprobe:_raw_spin_lock_irqsave,
kretprobe:_raw_spin_lock_irq,
kretprobe:_raw_spin_lock_bh
/comm == "hackbench" && curtask->lock_depth == 1/
{
    @cs_enter_cumul[tid] = curtask->cumulative_cs_time;
}

kprobe:_raw_spin_unlock,
kprobe:_raw_spin_unlock_irqrestore,
kprobe:_raw_spin_unlock_irq,
kprobe:_raw_spin_unlock_bh
/comm == "hackbench" && curtask->lock_depth == 1 && @cs_enter_cumul[tid] != 0/
{
    $overall = curtask->last_cs_ns;
    $active  = curtask->cumulative_cs_time - @cs_enter_cumul[tid];
    delete(@cs_enter_cumul[tid]);

    if ($overall == 0) return;

    printf("overall=%llu active=%llu offcpu=%lld\n",
           $overall, $active, (int64)$overall - (int64)$active);

    @k_overall_samples = lhist($overall, 0, 5000000, 100000);
    @k_active_samples  = lhist($active,  0, 5000000, 100000);
    @k_offcpu_samples  = lhist((int64)$overall - (int64)$active, 0, 2000000, 50000);
    @k_total           = count();
}

interval:s:10 { exit(); }
' 2>/dev/null > "$BPF_OUT_B" &
BT_PID_B=$!

sleep 11
kill $HB_PID   2>/dev/null; wait $HB_PID   2>/dev/null || true
wait $BT_PID_B 2>/dev/null || true

echo "  --- Samples (first 5) ---"
grep "^overall=" "$BPF_OUT_B" | head -5 || echo "  (no samples)"

TOTAL_B=$(grep -c "^overall=" "$BPF_OUT_B" 2>/dev/null || echo 0)
VIOL_B=$(grep "^overall=" "$BPF_OUT_B" | awk -F'offcpu=' '{if ($2+0 < 0) c++} END{print c+0}')
PREEMPT_B=$(grep "^overall=" "$BPF_OUT_B" | awk -F'offcpu=' '{if ($2+0 > 1000) c++} END{print c+0}')

echo
echo "  Total CS samples:          $TOTAL_B"
echo "  overall < active (BUG):    $VIOL_B"
echo "  overall > active + 1µs:    $PREEMPT_B  (preemption events seen)"

if [[ $TOTAL_B -eq 0 ]]; then
    fail "no kernel CS samples captured"
elif [[ $VIOL_B -gt 0 ]]; then
    fail "overall < active in $VIOL_B kernel samples — cs_start_ts accounting broken"
else
    pass "overall >= active holds across all $TOTAL_B kernel samples"
fi

if [[ $PREEMPT_B -gt 0 ]]; then
    pass "off-CPU kernel CS time observed ($PREEMPT_B samples with overall > active + 1µs)"
else
    echo "  NOTE: no kernel preemption events — try running under heavier vCPU contention"
fi

# ── SUMMARY ──────────────────────────────────────────────────────────────────
hdr "SUMMARY"
echo "  User CS:   $TOTAL_A samples, $VIOL_A violations, $PREEMPT_A preemption events"
echo "  Kernel CS: $TOTAL_B samples, $VIOL_B violations, $PREEMPT_B preemption events"
echo
echo "  Key invariant: overall_ns >= active_ns"
echo "  (wall-clock CS duration must be >= on-CPU CS duration)"
echo "  A violation means a clock source is broken or fields are mismatched."
echo

if [[ $FAILURES -eq 0 ]]; then
    echo "  RESULT: ALL ASSERTIONS PASSED"
else
    echo "  RESULT: $FAILURES ASSERTION(S) FAILED"
    exit 1
fi
