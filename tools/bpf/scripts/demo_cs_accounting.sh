#!/bin/bash
# demo_cs_accounting.sh — demonstrate per-task CS time and active runtime accounting
#
# Shows cumulative_cs_time and cumulative_active_time from the lhp_class debugfs
# node while a workload runs.  IVH must be loaded because running_migration()
# (which updates the snapshot) only fires when a BPF sched program is active.
#
# Usage:
#   sudo bash demo_cs_accounting.sh [duration_seconds] [workload_command...]
#   duration_seconds defaults to 10
#   workload_command defaults to: hackbench -l 9999999 -g 8
#
# Examples:
#   sudo bash demo_cs_accounting.sh 20
#   sudo bash demo_cs_accounting.sh 30 stress-ng --cpu 8 -t 60
#   sudo bash demo_cs_accounting.sh 15 sysbench cpu --threads=16 run

set -e

DURATION=${1:-10}
shift 1 || true
if [[ $# -gt 0 ]]; then
    WORKLOAD_CMD=("$@")
else
    WORKLOAD_CMD=(hackbench -l 9999999 -g 8)
fi

DEBUGFS=/sys/kernel/debug/lhp_class
IVH_BIN=/home/nick/IVH

# strip lock_depth=N from a snapshot line; also strip user_waiter since it's
# always 0 for non-rseq workloads and clutters the output
strip_fields() {
    sed 's/lock_depth=[0-9-]* \+//; s/user_waiter=[0-9]* \+//'
}

# sort by cs_pct (last numeric field) descending
sort_by_cs_pct() {
    awk '{ match($0, /cs_pct=([0-9]+)/, a); printf "%05d %s\n", a[1], $0 }' \
        | sort -rn | cut -d' ' -f2-
}

# ── sanity checks ────────────────────────────────────────────────────────────
if [[ $EUID -ne 0 ]]; then
    echo "Run as root (sudo $0)" >&2
    exit 1
fi

if [[ ! -x "$IVH_BIN" ]]; then
    echo "IVH binary not found at $IVH_BIN" >&2
    exit 1
fi

if [[ ! -f "$DEBUGFS" ]]; then
    echo "debugfs node $DEBUGFS missing — wrong kernel?" >&2
    exit 1
fi

# ── load IVH (enables bpf_sched_enabled, required for snapshot updates) ──────
echo "==> Loading IVH BPF program (shadow mode)..."
"$IVH_BIN" > /tmp/ivh_demo.log 2>&1 &
IVH_PID=$!
sleep 1

wait "$IVH_PID" 2>/dev/null  # IVH is a one-shot loader; wait for it to finish attaching
SCHED_PROGS=$(bpftool prog list 2>/dev/null | grep -c "sched" || true)
if [[ "$SCHED_PROGS" -eq 0 ]]; then
    echo "No sched BPF progs loaded after IVH run; see /tmp/ivh_demo.log" >&2
    exit 1
fi
echo "    IVH loaded ($SCHED_PROGS sched BPF programs active)"

# ── start workload ───────────────────────────────────────────────────────────
echo ""
echo "==> Starting workload: ${WORKLOAD_CMD[*]} (${DURATION}s)"
"${WORKLOAD_CMD[@]}" > /tmp/workload_demo.log 2>&1 &
WORKLOAD_PID=$!
sleep 1  # let it warm up

# ── poll snapshot ─────────────────────────────────────────────────────────────
echo ""
echo "==> lhp_class snapshot (polling every 2s for ${DURATION}s):"
echo "    Fields: cs_us = cumulative on-CPU spinlock CS time (µs)"
echo "            active_us = cumulative on-CPU time (µs)"
echo "            cs_pct = cs_us / active_us × 100 (integer %)"
echo ""

END=$((SECONDS + DURATION))
POLL=0
while [[ $SECONDS -lt $END ]]; do
    POLL=$((POLL + 1))
    echo "── snapshot #${POLL} (t=${SECONDS}s) ──────────────────────────────────────"
    cat "$DEBUGFS" \
        | awk '/pid=0/ { next } /cs_us=0 / { next } { print }' \
        | strip_fields \
        | sort_by_cs_pct \
        | head -8
    echo ""
    sleep 2
done

# ── final summary ────────────────────────────────────────────────────────────
echo "==> Final snapshot (all CPUs):"
cat "$DEBUGFS" | strip_fields

# ── teardown ─────────────────────────────────────────────────────────────────
echo ""
echo "==> Stopping workload..."
kill "$WORKLOAD_PID" 2>/dev/null; wait "$WORKLOAD_PID" 2>/dev/null || true
# IVH is a one-shot loader; it already exited after attaching BPF programs

echo ""
echo "==> Done.  Interpretation guide:"
echo "    cs_pct < 5%   : light spinlock user (background kernel work)"
echo "    cs_pct 5-20%  : moderate; worth watching under contention"
echo "    cs_pct > 20%  : heavy spinlock user — strong IVH candidate when user_waiter > 0"
echo ""
echo "    Next step: add  '(cs_pct > THRESHOLD && curr_waiter)'  to the IVH gate"
echo "    in MY_ivh_atc.bpf.c test() after Gate 4 to try the cs_pct policy."
