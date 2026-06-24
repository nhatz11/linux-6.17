#!/bin/bash
# demo_spinner.sh — confirm curr_waiter signal (user+kernel spinners)
set -e
TOOLS=$(dirname "$(dirname "$(realpath "$0")")")
EXTEND="/home/nick/extend-sched"
DEBUGFS_LHP="/sys/kernel/debug/lhp_class"
TRACE_PIPE="/sys/kernel/debug/tracing/trace"

die() { echo "ERROR: $*" >&2; exit 1; }
require() { command -v "$1" &>/dev/null || die "missing: $1"; }

require hackbench
require bpftool
[ -x "$EXTEND" ]       || die "extend-sched not found at $EXTEND"
[ -f "$DEBUGFS_LHP" ]  || die "lhp_class debugfs not found"

cd "$TOOLS"

echo "=== spinner demo ==="
echo

# ── Rebuild BPF objects ───────────────────────────────────────────────────────
echo "--- building BPF programs ---"
clang -O2 -g -Wall -target bpf -D__TARGET_ARCH_x86 -I. \
    -c MY_ivh_atc.bpf.c -o MY_ivh_atc.bpf.o 2>&1 | grep "^MY_ivh_atc\|error:" || true
make MY_ivh_atc 2>&1 | grep -v "^make\[" | tail -3

clang -O2 -g -Wall -target bpf -D__TARGET_ARCH_x86 -I. \
    -c qspinlock_probe.bpf.c -o qspinlock_probe.bpf.o 2>&1 | grep "error:" || echo "qspinlock_probe compile OK"

# ── Load qspinlock fentry/fexit probe (no kernel rebuild needed) ──────────────
echo "--- loading qspinlock_probe fentry/fexit ---"
bpftool prog loadall qspinlock_probe.bpf.o /sys/fs/bpf/qspin_probe \
    autoattach 2>&1 | grep -E "Error|error|Couldn't" || echo "  fentry probe loaded OK"

# ── Start MY_ivh_atc skeleton loader (attaches sched hooks) ──────────────────
echo "--- starting MY_ivh_atc (attaches cfs_sched_tick_end) ---"
./MY_ivh_atc > /tmp/ivh_atc.log 2>&1 &
MAIN_PID=$!
sleep 1
if ! kill -0 $MAIN_PID 2>/dev/null; then
    echo "  MY_ivh_atc exited early:"
    cat /tmp/ivh_atc.log | tail -5
else
    echo "  MY_ivh_atc running (pid $MAIN_PID)"
fi

# ── Clear trace buffer ────────────────────────────────────────────────────────
echo > "$TRACE_PIPE"

# ── Test 1: user spinner (wait_counter → curr_waiter=1) ──────────────────────
echo
echo "--- Test 1: user spinner (extend-sched 8 threads, 3 s) ---"
"$EXTEND" 8 &
EXTEND_PID=$!
sleep 3

echo "  lhp_class (user_waiter=1):"
grep "user_waiter=1" "$DEBUGFS_LHP" | head -8 || echo "  (none)"

echo "  SPINNER events in trace:"
grep "SPINNER" /sys/kernel/debug/tracing/trace | head -10 || echo "  (none)"

kill $EXTEND_PID 2>/dev/null || true; wait $EXTEND_PID 2>/dev/null || true

# ── Test 2: kernel qspinlock contention (QSPIN_ENTER from fentry) ────────────
echo
echo "--- Test 2: kernel spinner (hackbench -g 32, 5 s) ---"
echo > "$TRACE_PIPE"
hackbench -g 32 -l 500 > /dev/null 2>&1 &
HB_PID=$!
sleep 5

echo "  QSPIN_ENTER events (slowpath hit = wait_depth incremented):"
grep "QSPIN_ENTER" /sys/kernel/debug/tracing/trace | grep -v "pid=0" | head -10 || \
    grep "QSPIN_ENTER" /sys/kernel/debug/tracing/trace | head -5 || echo "  (none)"

echo "  SPINNER events at tick (curr_waiter=1 from wait_depth):"
grep "SPINNER" /sys/kernel/debug/tracing/trace | head -10 || echo "  (none — expected: window is 4ms tick vs µs spin)"

kill $HB_PID 2>/dev/null || true; wait $HB_PID 2>/dev/null || true

# ── Cleanup ────────────────────────────────────────────────────────────────────
kill $MAIN_PID 2>/dev/null || true; wait $MAIN_PID 2>/dev/null || true
rm -rf /sys/fs/bpf/qspin_probe 2>/dev/null || true

echo
echo "=== summary ==="
echo "  user_waiter=1 in lhp_class  → rseq wait_counter path works"
echo "  SPINNER in trace             → curr_waiter reached BPF hook at tick time"
echo "  QSPIN_ENTER in trace         → kernel qspinlock slowpath (wait_depth increment sites) reached"
