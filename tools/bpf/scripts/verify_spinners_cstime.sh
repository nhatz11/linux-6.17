#!/usr/bin/env bash
# verify_spinners_cstime.sh
#
# Confirms two things:
#   1. Spinner detection: wait_counter (user) and wait_depth (kernel) are
#      visible to the kernel at tick time via lhp_class and trace events.
#   2. CS time: hackbench absolute (on-CPU only, from cumulative_cs_time via
#      lhp_class, shown in MICROSECONDS) and overall (wall-clock lock-to-unlock,
#      from lhp_cstime, shown in NANOSECONDS) can both be observed and compared.
#
# UNIT NOTE: cs_us / active_us in lhp_class are already µs (kernel divides
#   cumulative_cs_time by 1000 before printing).  lhp_cstime duration= values
#   are raw nanoseconds.  To compare: absolute_ns = cs_us × 1000.
#
# Requires: ~/IVH already running (MY_ivh_atc + vcap), lhp_cstime built.
# Usage: sudo bash verify_spinners_cstime.sh

set -euo pipefail

TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
EXTEND="/home/nick/extend-sched"
CSTIME_BIN="$TOOLS/lhp_cstime"
LHP_CLASS="/sys/kernel/debug/lhp_class"
TRACE_PIPE="/sys/kernel/debug/tracing/trace"

die() { echo "ERROR: $*" >&2; exit 1; }
hdr() { echo; echo "══════════════════════════════════════════════════════════"; echo "  $*"; echo "══════════════════════════════════════════════════════════"; }
sep() { echo "──────────────────────────────────────────────────────────"; }

[[ -f "$LHP_CLASS" ]]  || die "lhp_class not found — is MY_ivh_atc running?"
[[ -x "$CSTIME_BIN" ]] || die "lhp_cstime not built"
[[ -x "$EXTEND" ]]     || die "extend-sched not found at $EXTEND"
command -v hackbench   || die "hackbench not found"
command -v python3     || die "python3 required"

# ── 1. USER SPINNER DETECTION ────────────────────────────────────────────────
hdr "TEST 1: user spinner detection (wait_counter → curr_user_waiter)"
echo "  Workload: extend-sched 8 threads"
echo "  Method A: poll lhp_class every 200 ms for 8 s; capture user_waiter=1 rows"
echo "  Method B: check trace buffer for SPINNER events (curr_waiter=1 at tick)"
echo

echo > "$TRACE_PIPE"

"$EXTEND" 8 > /dev/null 2>&1 &
EXTEND_PID=$!
sleep 0.3

echo "  --- lhp_class polling (user_waiter=1 rows) ---"
FOUND_WAITER=0
for i in $(seq 1 40); do
    hits=$(grep "user_waiter=1" "$LHP_CLASS" 2>/dev/null || true)
    if [[ -n "$hits" ]]; then
        echo "$hits" | head -4
        FOUND_WAITER=1
        break
    fi
    sleep 0.2
done
[[ $FOUND_WAITER -eq 1 ]] || echo "  (no user_waiter=1 snapshot caught in 8s)"

sleep 2  # let more ticks accumulate for SPINNER events

echo
echo "  --- SPINNER trace events (curr_waiter=1 reached BPF tick hook) ---"
spinner_count=$(grep -c "SPINNER" "$TRACE_PIPE" 2>/dev/null || true)
grep "SPINNER" "$TRACE_PIPE" 2>/dev/null | grep "extend-sched" | head -6 || true
echo "  Total SPINNER events in trace: $spinner_count"

kill $EXTEND_PID 2>/dev/null || true
wait $EXTEND_PID 2>/dev/null || true

if [[ $spinner_count -gt 0 ]]; then
    echo "  RESULT: ✓  wait_counter → curr_user_waiter confirmed (SPINNER events seen)"
elif [[ $FOUND_WAITER -eq 1 ]]; then
    echo "  RESULT: ✓  wait_counter → curr_user_waiter confirmed (lhp_class snapshot)"
else
    echo "  RESULT: ✗  no user_waiter signal detected (check extend-sched version)"
fi

# ── 2. KERNEL SPINNER DETECTION ──────────────────────────────────────────────
hdr "TEST 2: kernel spinner detection (wait_depth → curr_waiter)"
echo "  Workload: hackbench -g 32 (high kernel lock contention)"
echo "  Method: QSPIN_ENTER fentry/fexit on queued_spin_lock_slowpath"
echo "          (that is the site where wait_depth is incremented)"
echo

QSPIN_BPF_OBJ="$TOOLS/qspinlock_probe.bpf.o"
QSPIN_PIN="/sys/fs/bpf/qspin_probe_vscrip"
rm -rf "$QSPIN_PIN" 2>/dev/null || true
echo > "$TRACE_PIPE"

if [[ -f "$QSPIN_BPF_OBJ" ]]; then
    bpftool prog loadall "$QSPIN_BPF_OBJ" "$QSPIN_PIN" autoattach 2>/dev/null || true
    hackbench -g 32 -l 500 > /dev/null 2>&1 &
    HB_PID=$!
    sleep 3
    qspin_count=$(grep -c "QSPIN_ENTER" "$TRACE_PIPE" 2>/dev/null || echo 0)
    echo "  --- QSPIN_ENTER events (hackbench hitting qspinlock slowpath) ---"
    grep "QSPIN_ENTER" "$TRACE_PIPE" 2>/dev/null | grep "hackbench" | head -6 || \
        grep "QSPIN_ENTER" "$TRACE_PIPE" 2>/dev/null | head -6 || true
    echo "  Total QSPIN_ENTER events: $qspin_count"
    kill $HB_PID 2>/dev/null || true; wait $HB_PID 2>/dev/null || true
    rm -rf "$QSPIN_PIN" 2>/dev/null || true
    spin_tick=$(grep -c "SPINNER" "$TRACE_PIPE" 2>/dev/null || echo 0)
    echo "  SPINNER events at tick (wait_depth>0 at 4ms tick — sparse by design): $spin_tick"
    if [[ $qspin_count -gt 0 ]]; then
        echo "  RESULT: ✓  wait_depth increment sites reached under hackbench contention"
    else
        echo "  RESULT: ✗  no QSPIN_ENTER events"
    fi
else
    echo "  qspinlock_probe.bpf.o not found — skipping (build with: clang -O2 -g -target bpf ...)"
fi

# ── 3. HACKBENCH ABSOLUTE CS TIME (cumulative_cs_time via lhp_class) ─────────
hdr "TEST 3: hackbench absolute CS time (on-CPU only, from lhp_class)"
echo "  Field: cumulative_cs_time → cs_us in lhp_class"
echo "  Meaning: total MICROSECONDS (µs) the task spent holding spinlocks ON-CPU"
echo "           (kernel prints cumulative_cs_time ÷ 1000; to compare with lhp_cstime"
echo "            ns values: absolute_ns = cs_us × 1000)"
echo "           (paused during preemption in prepare_task_switch)"
echo

hackbench -g 8 -l 100000 > /dev/null 2>&1 &
HB_PID=$!
sleep 0.5

echo "  --- lhp_class snapshot during hackbench (cs_us = absolute on-CPU CS time) ---"
echo "  Polling until hackbench tasks appear with cs_us > 0 ..."
FOUND_HB=0
for i in $(seq 1 30); do
    snap=$(grep "hackbench" "$LHP_CLASS" 2>/dev/null || true)
    if [[ -n "$snap" ]]; then
        echo "$snap"
        FOUND_HB=1
        break
    fi
    sleep 0.2
done
[[ $FOUND_HB -eq 1 ]] || echo "  (hackbench finished before snapshot — try more loops)"

kill $HB_PID 2>/dev/null || true; wait $HB_PID 2>/dev/null || true

# ── 4. HACKBENCH OVERALL CS TIME (wall-clock, from lhp_cstime) ──────────────
hdr "TEST 4: hackbench overall CS time (wall-clock lock-to-unlock, from lhp_cstime)"
echo "  Field: fexit/_raw_spin_lock → fentry/_raw_spin_unlock wall clock delta"
echo "  Meaning: total elapsed time lock was held INCLUDING any off-CPU preemption"
echo

hackbench -g 8 -l 100000 > /dev/null 2>&1 &
HB_PID=$!
sleep 0.1
echo "  Running lhp_cstime for 8 s alongside hackbench ..."
RAW=$(mktemp /tmp/cstime_hb.XXXXXX.txt)
timeout 8 "$CSTIME_BIN" 2>/dev/null > "$RAW" || true
kill $HB_PID 2>/dev/null || true; wait $HB_PID 2>/dev/null || true

python3 - "$RAW" << 'PYEOF'
import sys, re, statistics

path = sys.argv[1]
durations = []
hb_durations = []
with open(path) as f:
    for line in f:
        m = re.match(r'pid \d+ \((\S+)\) .* duration=(\d+) ns', line)
        if not m:
            m = re.match(r'pid (\d+) \((\S+)\) cpu \d+ cs=(\d+) ns', line)
            if m:
                comm, d = m.group(2), int(m.group(3))
            else:
                continue
        else:
            comm, d = m.group(1), int(m.group(2))
        durations.append(d)
        if 'hackbench' in comm:
            hb_durations.append(d)

def fmt(ns):
    if ns < 1000: return f"{ns} ns"
    if ns < 1_000_000: return f"{ns/1000:.1f} µs"
    return f"{ns/1_000_000:.3f} ms"

def pct(s, p): return s[int(len(s)*p//100)]

if durations:
    s = sorted(durations)
    print(f"  System-wide events : {len(s):,}")
    print(f"  Overall CS median  : {fmt(pct(s,50))}  p90={fmt(pct(s,90))}  p99={fmt(pct(s,99))}")

if hb_durations:
    h = sorted(hb_durations)
    print(f"\n  hackbench events   : {len(h):,}")
    print(f"  Overall CS median  : {fmt(pct(h,50))}  p90={fmt(pct(h,90))}  p99={fmt(pct(h,99))}")
    print(f"  (wall-clock: includes any off-CPU time mid-CS)")
else:
    print("\n  No hackbench events parsed (check lhp_cstime output format)")
    # fall back: show raw line count
    with open(path) as f:
        lines = [l for l in f if 'duration' in l or ' cs=' in l]
    if lines:
        print(f"  Sample lines: {lines[0].strip()}")
PYEOF
rm -f "$RAW"

# ── 5. SUMMARY ───────────────────────────────────────────────────────────────
hdr "SUMMARY"
cat << 'EOF'
  UNIT SUMMARY
  ─────────────────────────────────────────────────────────────
  cs_us in lhp_class  → CUMULATIVE total on-CPU CS time for that task (µs)
                        source: cumulative_cs_time / 1000
                        = SUM of ALL individual lock hold times (on-CPU only)
                        → cs_pct = cs_us / active_us = fraction of CPU time
                          the task spent holding spinlocks (useful metric)
                        → NOT comparable to a single lhp_cstime event

  duration= in lhp_cstime → PER-EVENT wall-clock lock hold time (ns)
                        source: fexit/_raw_spin_lock → fentry/_raw_spin_unlock
                        = wall-clock for ONE individual lock acquire/release
                        → p50 ≈ fast path (no preemption, ~100 ns)
                        → p99 tail = LHP events (preempted mid-CS, hundreds µs)
                        → LHP penalty per event ≈ p99 − p50 of this distribution

  DO NOT subtract cs_us from lhp_cstime duration — different scales:
    cs_us is the cumulative total; duration is per-event.

  For per-event on-CPU vs wall-clock (same CS), use userspace rseq fields:
  ─────────────────────────────────────────────────────────────

  For userspace locks (pthread via preload/glibc):
    last_cs_overall_ns  @ rseq+40  = overall CS time of most recent CS (ns)
    last_cs_active_ns   @ rseq+48  = absolute (CLOCK_THREAD_CPUTIME_ID) (ns)
    last_wait_overall_ns@ rseq+56  = wait time of most recent acquire (ns)
    → read at any tick by BPF via bpf_probe_read_user()
EOF
echo
