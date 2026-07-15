#!/usr/bin/env bash
# prove_cs_ns.sh -- confirms kernel spinlock critical-section wall-clock
# duration (fexit/_raw_spin_lock -> fentry/_raw_spin_unlock, via the
# project's existing lhp_cstime tool) for a specific workload, and confirms
# the measurement is actually capturing real events for that workload (not
# silently reading zero, which would mean the proof is worthless, not that
# CS time is short).
#
# Usage:
#   sudo bash tools/bpf/scripts/prove_cs_ns.sh <comm-substring> <duration-sec> -- <workload...>
#
# Examples:
#   sudo bash tools/bpf/scripts/prove_cs_ns.sh hackbench 8 -- /usr/bin/hackbench -T -g 1 -f 8 -l 400000
#   sudo bash tools/bpf/scripts/prove_cs_ns.sh NHextend  8 -- /home/nick/ivh_exec /home/nick/NHextend -n -l

set -euo pipefail

TOOLS=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
CSTIME_BIN="$TOOLS/lhp_cstime"

[[ $# -ge 4 ]] || { echo "usage: $0 <comm-substring> <duration-sec> -- <workload...>" >&2; exit 1; }
COMM="$1"; DUR="$2"; shift 2
[[ "$1" == "--" ]] || { echo "expected -- before workload command" >&2; exit 1; }
shift

[[ -x "$CSTIME_BIN" ]] || { echo "ERROR: lhp_cstime not built at $CSTIME_BIN" >&2; exit 1; }

"$@" > /dev/null 2>&1 &
WPID=$!

RAW=$(mktemp /tmp/cstime_proof.XXXXXX.txt)
echo "Running lhp_cstime for ${DUR}s alongside: $* (pid $WPID)"
timeout "$DUR" "$CSTIME_BIN" 2>/dev/null > "$RAW" || true

kill "$WPID" 2>/dev/null || true
wait "$WPID" 2>/dev/null || true

python3 - "$RAW" "$COMM" << 'PYEOF'
import sys, re

path, comm_filter = sys.argv[1], sys.argv[2]
durations, filtered = [], []

with open(path) as f:
    for line in f:
        m = re.match(r'pid \d+ \((\S+)\) .* duration=(\d+) ns', line)
        if not m:
            continue
        comm, d = m.group(1), int(m.group(2))
        durations.append(d)
        if comm_filter in comm:
            filtered.append(d)

def fmt(ns):
    if ns < 1000: return f"{ns} ns"
    if ns < 1_000_000: return f"{ns/1000:.1f} us"
    return f"{ns/1_000_000:.3f} ms"

def pct(s, p): return s[int(len(s) * p // 100)]

print(f"\n=== CS wall-clock duration: system-wide ===")
print(f"Events: {len(durations):,}")
if durations:
    s = sorted(durations)
    print(f"  median={fmt(pct(s,50))}  p90={fmt(pct(s,90))}  p99={fmt(pct(s,99))}")

print(f"\n=== CS wall-clock duration: '{comm_filter}' only ===")
print(f"Events: {len(filtered):,}")
if filtered:
    fl = sorted(filtered)
    print(f"  median={fmt(pct(fl,50))}  p90={fmt(pct(fl,90))}  p99={fmt(pct(fl,99))}")
    print(f"\nMEASUREMENT CHECK: {len(filtered):,} '{comm_filter}' events captured -- ")
    print("real data, not an empty/broken measurement.")
else:
    print(f"\nMEASUREMENT CHECK FAILED: zero events matched '{comm_filter}'.")
    print("Either the comm-substring is wrong, the workload didn't run during")
    print("the capture window, or lhp_cstime isn't attaching correctly --")
    print("this result would NOT be usable as proof of anything.")
PYEOF

rm -f "$RAW"
