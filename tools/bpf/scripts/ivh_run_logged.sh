#!/usr/bin/env bash
# ivh_run_logged.sh — Run MY_ivh_atc with crash-persistent logging.
#
# Two files are written and fsync'd every 500 ms so they survive a hard reset:
#   /var/log/ivh_trace.log   BPF trace events (every accepted migration)
#   /var/log/ivh_dmesg.log   kernel messages (soft lockup warnings, BUGs, panics)
#
# After a crash/reboot, read those files to see exactly what IVH was doing
# and what the kernel reported in the moments before the reset.
#
# Usage:
#   sudo ./ivh_run_logged.sh [MY_ivh_atc args...]
# Example:
#   sudo ./ivh_run_logged.sh -a 1

set -euo pipefail

IVH_SCRIPT="/home/nick/IVH"
TRACE_LOG="/var/log/ivh_trace.log"
DMESG_LOG="/var/log/ivh_dmesg.log"

# Find trace_pipe — may be under debugfs or tracefs
[[ -x "$IVH_SCRIPT" ]] || { echo "ERROR: $IVH_SCRIPT not found or not executable"; exit 1; }

DMESG_PID=""
SYNC_PID=""
IVH_PID=""

cleanup() {
    echo "=== ivh_run_logged stopped at $(date) ===" | tee -a "$TRACE_LOG"
    sync
    [[ -n "$IVH_PID"   ]] && kill "$IVH_PID" 2>/dev/null || true
    [[ -n "$DMESG_PID" ]] && kill "$DMESG_PID" 2>/dev/null || true
    [[ -n "$SYNC_PID"  ]] && kill "$SYNC_PID"  2>/dev/null || true
    sync
}
trap cleanup EXIT INT TERM

# ── Header ────────────────────────────────────────────────────────────────────
{
    echo "=== ivh_run_logged started: $(date) ==="
    echo "    kernel : $(uname -r)"
    echo "    CPUs   : $(nproc)"
    echo "    IVH    : $IVH_SCRIPT"
    echo "    log    : /var/log/ivh_migrations.log (polled by MY_ivh_atc every 5s)"
} | tee "$TRACE_LOG"
echo "=== ivh_run_logged started: $(date) ===" > "$DMESG_LOG"
sync

# ── Capture kernel messages → ivh_dmesg.log ──────────────────────────────────
# MY_ivh_atc polls its own BPF map and writes → /var/log/ivh_migrations.log.
# We do NOT read trace_pipe here: a `cat trace_pipe` process on a throttled
# CPU becomes an IVH migration candidate, causing a feedback loop that floods
# the spinlock path and triggers TLB IPI soft lockups.
# dmesg -w streams new kernel log lines in real time.
# Soft lockup / RCU stall / BUG lines appear here before a hard reset.
stdbuf -oL dmesg -w >> "$DMESG_LOG" 2>&1 &
DMESG_PID=$!

# ── Periodic fsync — the critical ingredient for hard-reset survival ──────────
# sync flushes all dirty kernel page-cache buffers.  500 ms means at most
# ~500 ms of events are lost if the hypervisor hard-resets the VM.
( while true; do sync; sleep 0.5; done ) &
SYNC_PID=$!

# ── Launch IVH (setup + vcap + MY_ivh_atc) ───────────────────────────────────
sync
bash "$IVH_SCRIPT"
# /home/nick/IVH exits immediately after backgrounding vcap and MY_ivh_atc.
# Find MY_ivh_atc's actual PID and track that instead.
sleep 1
IVH_PID=$(pgrep -n MY_ivh_atc 2>/dev/null || true)
if [[ -z "$IVH_PID" ]]; then
    echo "ERROR: MY_ivh_atc not found after IVH launch" | tee -a "$TRACE_LOG"
    exit 1
fi
echo "MY_ivh_atc pid=$IVH_PID" | tee -a "$TRACE_LOG"
sync

# ── Heartbeat: timestamp every 5 s so crash time is bracketed ────────────────
while kill -0 "$IVH_PID" 2>/dev/null; do
    echo "# alive $(date '+%T')" >> "$TRACE_LOG"
    sync
    sleep 5
done

echo "MY_ivh_atc exited (pid=$IVH_PID)." | tee -a "$TRACE_LOG"
