#!/usr/bin/env bash
# Run lhp_cstime + lhp_waittime against a workload and report CS statistics.
#
# Usage:
#   ./cs_experiment.sh [duration_seconds] [workload_command...]
#
# Examples:
#   ./cs_experiment.sh 5 ~/extend-sched
#   ./cs_experiment.sh 10 ~/extend-sched -w
#   ./cs_experiment.sh 5                        # no workload, measure system-wide

set -euo pipefail

DURATION=${1:-5}
shift || true
WORKLOAD_CMD=("$@")

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS="$(dirname "$SCRIPT_DIR")"
CSTIME_BIN="$TOOLS/lhp_cstime"
WAITTIME_BIN="$TOOLS/lhp_waittime"
CSTIME_RAW=$(mktemp /tmp/cstime_raw.XXXXXX.txt)
WAITTIME_RAW=$(mktemp /tmp/waittime_raw.XXXXXX.txt)
WAITTIME_CORRECTED=$(mktemp /tmp/waittime_corrected.XXXXXX.txt)

trap 'rm -f "$CSTIME_RAW" "$WAITTIME_RAW" "$WAITTIME_CORRECTED"' EXIT

die() { echo "ERROR: $*" >&2; exit 1; }

[[ -x "$CSTIME_BIN" ]]   || die "lhp_cstime not found at $CSTIME_BIN"
[[ -x "$WAITTIME_BIN" ]] || die "lhp_waittime not found at $WAITTIME_BIN"
command -v python3 >/dev/null || die "python3 required"

echo "=== CS experiment: ${DURATION}s collection ==="
if [[ ${#WORKLOAD_CMD[@]} -gt 0 ]]; then
    echo "    workload : ${WORKLOAD_CMD[*]}"
else
    echo "    workload : (none — system-wide)"
fi
echo

# ── 1. Start workload (if any) ────────────────────────────────────────────────
WORKLOAD_PID=""
if [[ ${#WORKLOAD_CMD[@]} -gt 0 ]]; then
    "${WORKLOAD_CMD[@]}" &
    WORKLOAD_PID=$!
    sleep 0.2
fi

# ── 2. Collect lhp_cstime ─────────────────────────────────────────────────────
echo "[1/2] Collecting kernel spinlock CS data for ${DURATION}s ..."
timeout "$DURATION" sudo "$CSTIME_BIN" 2>/dev/null > "$CSTIME_RAW" || true
echo "      $(( $(wc -l < "$CSTIME_RAW") - 1 )) events captured"

# Kill and restart workload for the second run
if [[ -n "$WORKLOAD_PID" ]]; then
    kill "$WORKLOAD_PID" 2>/dev/null || true
    wait "$WORKLOAD_PID" 2>/dev/null || true
    "${WORKLOAD_CMD[@]}" &
    WORKLOAD_PID=$!
    sleep 0.2
fi

# ── 3a. Collect lhp_waittime (raw: includes off-CPU time) ────────────────────
echo "[2/3] Collecting user rseq CS (raw, includes off-CPU) for ${DURATION}s ..."
timeout "$DURATION" sudo "$WAITTIME_BIN" 2>/dev/null > "$WAITTIME_RAW" || true
echo "      $(( $(wc -l < "$WAITTIME_RAW") - 1 )) events captured"

# Kill and restart workload for the corrected run
if [[ -n "$WORKLOAD_PID" ]]; then
    kill "$WORKLOAD_PID" 2>/dev/null || true
    wait "$WORKLOAD_PID" 2>/dev/null || true
    "${WORKLOAD_CMD[@]}" &
    WORKLOAD_PID=$!
    sleep 0.2
fi

# ── 3b. Collect lhp_waittime -c (corrected: off-CPU subtracted) ──────────────
echo "[3/3] Collecting user rseq CS (corrected, off-CPU subtracted) for ${DURATION}s ..."
timeout "$DURATION" sudo "$WAITTIME_BIN" -c 2>/dev/null > "$WAITTIME_CORRECTED" || true
echo "      $(( $(wc -l < "$WAITTIME_CORRECTED") - 1 )) events captured"

if [[ -n "$WORKLOAD_PID" ]]; then
    kill "$WORKLOAD_PID" 2>/dev/null || true
    wait "$WORKLOAD_PID" 2>/dev/null || true
fi

echo

# ── 4. Analyze ────────────────────────────────────────────────────────────────
python3 - "$CSTIME_RAW" "$WAITTIME_RAW" "$WAITTIME_CORRECTED" << 'PYEOF'
import sys, re, statistics

cstime_file, waittime_file, corrected_file = sys.argv[1], sys.argv[2], sys.argv[3]
BOGUS_CUTOFF = 10_000_000  # 10 ms — anything above is a u64 underflow artifact

# ── Parse lhp_cstime ──────────────────────────────────────────────────────────
kern_all = []
kern_by_comm = {}  # comm -> [durations]

with open(cstime_file) as f:
    for line in f:
        if '[kernel]' not in line:
            continue
        try:
            parts = line.split()
            dur = int(next(p for p in parts if p.startswith('duration=')).split('=')[1])
            comm = parts[2].strip('()')
        except (StopIteration, IndexError, ValueError):
            continue
        if dur >= BOGUS_CUTOFF:
            continue
        kern_all.append(dur)
        kern_by_comm.setdefault(comm, []).append(dur)

def pct(vals_sorted, p):
    return vals_sorted[int(p / 100 * len(vals_sorted))]

def fmt(ns):
    if ns < 1_000:
        return f"{ns} ns"
    if ns < 1_000_000:
        return f"{ns/1e3:.1f} µs"
    return f"{ns/1e6:.3f} ms"

if kern_all:
    ks = sorted(kern_all)
    n = len(ks)
    bogus_count = sum(1 for line in open(cstime_file)
                      if '[kernel]' in line
                      for dur in [int(next((p.split('=')[1] for p in line.split()
                                           if p.startswith('duration=')), '0'))]
                      if dur >= BOGUS_CUTOFF)

    print("━" * 60)
    print("KERNEL SPINLOCK CS  (lhp_cstime, system-wide)")
    print("━" * 60)
    print(f"  Valid events  : {n:,}   (filtered {bogus_count} u64-underflow artifacts)")
    print(f"  Min           : {fmt(ks[0])}")
    print(f"  Mean          : {fmt(int(statistics.mean(ks)))}")
    print(f"  Median (p50)  : {fmt(pct(ks, 50))}")
    print(f"  p90           : {fmt(pct(ks, 90))}")
    print(f"  p99           : {fmt(pct(ks, 99))}")
    print(f"  p99.9         : {fmt(pct(ks, 99.9))}")
    print(f"  Max (filtered): {fmt(ks[-1])}")
    print()

    print("  Distribution:")
    buckets = [
        ("< 1 µs",        0,        1_000),
        ("1–10 µs",    1_000,      10_000),
        ("10–100 µs", 10_000,     100_000),
        ("100 µs–1 ms", 100_000, 1_000_000),
        ("1–2 ms",   1_000_000, 2_000_000),
        ("2–4 ms",   2_000_000, 4_000_000),
        ("> 4 ms",   4_000_000, BOGUS_CUTOFF),
    ]
    for label, lo, hi in buckets:
        cnt = sum(1 for v in ks if lo <= v < hi)
        bar = "█" * int(cnt / n * 30)
        print(f"    {label:<16} {cnt:>9,} ({cnt/n*100:6.2f}%)  {bar}")
    print()

    print("  Migration threshold context:")
    for ms in [1, 2, 4]:
        ns = ms * 1_000_000
        below = sum(1 for v in ks if v < ns)
        print(f"    CS < {ms} ms: {below:,} ({below/n*100:.4f}%)  |  "
              f"CS ≥ {ms} ms: {n-below:,} ({(n-below)/n*100:.4f}%)")
    print()

    if kern_by_comm:
        print("  Top processes by long-CS event count (> 1 ms):")
        long_by_comm = {c: [d for d in ds if d >= 1_000_000]
                        for c, ds in kern_by_comm.items()}
        long_by_comm = {c: ds for c, ds in long_by_comm.items() if ds}
        for comm, ds in sorted(long_by_comm.items(), key=lambda x: -len(x[1]))[:8]:
            ds_s = sorted(ds)
            print(f"    {comm:<22} {len(ds):>5} events  "
                  f"median={fmt(ds_s[len(ds_s)//2])}  max={fmt(ds_s[-1])}")
        print()
else:
    print("No kernel spinlock CS data found in lhp_cstime output.\n")

# ── Parse lhp_waittime (raw and corrected) ───────────────────────────────────
def parse_waittime(path):
    cs_vals, wait_vals = [], []
    with open(path) as f:
        for line in f:
            m = re.search(r'cs=(\d+) ns wait=(\d+) ns', line)
            if m:
                cs_vals.append(int(m.group(1)))
                wait_vals.append(int(m.group(2)))
    return cs_vals, wait_vals

cs_vals,   wait_vals   = parse_waittime(waittime_file)
cs_corr,   wait_corr   = parse_waittime(corrected_file)

def print_cs_section(cs_vals, wait_vals, label):
    if not cs_vals:
        print(f"No events in {label}.\n")
        return
    n    = len(cs_vals)
    cs_s = sorted(cs_vals)
    wt_s = sorted(wait_vals)

    print("━" * 60)
    print(f"USER rseq CS  — {label}")
    print("━" * 60)
    print(f"  Events        : {n:,}")
    print()
    print("  CS duration:")
    print(f"    Min         : {fmt(cs_s[0])}")
    print(f"    Mean        : {fmt(int(statistics.mean(cs_vals)))}")
    print(f"    Median      : {fmt(pct(cs_s, 50))}")
    print(f"    p90         : {fmt(pct(cs_s, 90))}")
    print(f"    p99         : {fmt(pct(cs_s, 99))}")
    print(f"    Max         : {fmt(cs_s[-1])}")
    print()

    print("  Distribution:")
    cs_buckets = [
        ("< 1 µs",              0,          1_000),
        ("1–10 µs",          1_000,         10_000),
        ("10–100 µs",       10_000,        100_000),
        ("100 µs–1 ms",    100_000,      1_000_000),
        ("1–10 ms",      1_000_000,     10_000_000),
        ("> 10 ms",     10_000_000, 999_000_000_000),
    ]
    for lbl, lo, hi in cs_buckets:
        cnt = sum(1 for v in cs_s if lo <= v < hi)
        bar = "█" * int(cnt / n * 30)
        print(f"    {lbl:<16} {cnt:>9,} ({cnt/n*100:6.2f}%)  {bar}")
    print()

    print("  Wait-before-CS (time running on CPU before CS entry):")
    print(f"    Min         : {fmt(wt_s[0])}")
    print(f"    Median      : {fmt(pct(wt_s, 50))}")
    print(f"    p90         : {fmt(pct(wt_s, 90))}")
    print(f"    p99         : {fmt(pct(wt_s, 99))}")
    print(f"    Max         : {fmt(wt_s[-1])}")
    print()

print_cs_section(cs_vals,  wait_vals,  "raw (tick-sampled, includes off-CPU wait)")
print_cs_section(cs_corr,  wait_corr,  "corrected (off-CPU time subtracted)")

if cs_vals:
    n = len(cs_vals)
    print("━" * 60)
    print("MIGRATION THRESHOLD ANALYSIS  (raw CS durations)")
    print("━" * 60)
    print("  At T ms since sched-in, how many tasks are already mid-CS?")
    print()
    for ms in [1, 2, 4]:
        thr = ms * 1_000_000
        in_cs = sum(1 for w in wait_vals if w < thr)
        remaining = sorted(
            cs - (thr - wt)
            for cs, wt in zip(cs_vals, wait_vals)
            if wt < thr and cs > (thr - wt)
        )
        rem_med = remaining[len(remaining)//2] if remaining else 0
        rem_p90 = remaining[int(0.9*len(remaining))] if remaining else 0
        print(f"  T = {ms} ms:")
        print(f"    Already in CS        : {in_cs}/{n} ({in_cs/n*100:.0f}%)")
        print(f"    Not yet in CS        : {n-in_cs}/{n} ({(n-in_cs)/n*100:.0f}%)")
        print(f"    Remaining CS (defer) : median={fmt(rem_med)}  p90={fmt(rem_p90)}")
        print()
elif not cs_corr:
    print("No user rseq CS data found.")
    print("(lhp_waittime only captures rseq-registered tasks — run with extend-sched)\n")
PYEOF
