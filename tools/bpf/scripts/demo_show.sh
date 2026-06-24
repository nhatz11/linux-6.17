#!/usr/bin/env bash
# demo_show.sh — Advisor demos: Q2 lockholder detection, Q3 movability, Q6 tick-timing
#
# Usage (must run as root):
#   sudo ./demo_show.sh [duration_seconds] [workload_command...]
#
# Examples:
#   sudo ./demo_show.sh 10 ~/extend-sched
#   sudo ./demo_show.sh 10 ~/extend-sched -w
#   sudo ./demo_show.sh 10               # system-wide, no workload

set -euo pipefail

DURATION=${1:-10}
shift || true
WORKLOAD_CMD=("$@")

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS="$(dirname "$SCRIPT_DIR")"
IVH_BIN="$TOOLS/MY_ivh_atc"
MOVABLE_BIN="$TOOLS/lhp_movable"

die() { echo "ERROR: $*" >&2; exit 1; }

[[ -x "$IVH_BIN" ]]     || die "MY_ivh_atc not found at $IVH_BIN (rebuild?)"
[[ -x "$MOVABLE_BIN" ]] || die "lhp_movable not found at $MOVABLE_BIN (rebuild?)"
[[ -x "$SCRIPT_DIR/cs_experiment.sh" ]] || die "cs_experiment.sh not found"
command -v python3  >/dev/null || die "python3 required"
command -v bpftool  >/dev/null || die "bpftool required"
[[ $EUID -eq 0 ]]   || die "must run as root: sudo $0 ..."

MOVABLE_RAW=$(mktemp /tmp/movable_raw.XXXXXX.txt)
COUNTERS_JSON=$(mktemp /tmp/ivh_ctrs.XXXXXX.json)
IVH_LOG=$(mktemp /tmp/ivh_log.XXXXXX.txt)
trap 'rm -f "$MOVABLE_RAW" "$COUNTERS_JSON" "$IVH_LOG"' EXIT

banner() {
    local msg="$1"
    local width=64
    echo
    printf '═%.0s' $(seq 1 $width); echo
    printf "  %s\n" "$msg"
    printf '═%.0s' $(seq 1 $width); echo
    echo
}

start_workload() {
    if [[ ${#WORKLOAD_CMD[@]} -gt 0 ]]; then
        # setsid creates a new process group so stop_workload can kill the
        # entire group (including any child processes the workload spawns)
        setsid "${WORKLOAD_CMD[@]}" >/dev/null 2>&1 &
        local wpid=$!
        sleep 0.3
        echo "$wpid"
    fi
}

stop_workload() {
    local wpid="${1:-}"
    if [[ -n "$wpid" ]]; then
        # kill the whole process group (negative PID = send to entire group)
        kill -TERM -"$wpid" 2>/dev/null || true
        sleep 0.3
        kill -KILL -"$wpid" 2>/dev/null || true
        wait "$wpid" 2>/dev/null || true
    fi
}

# ══════════════════════════════════════════════════════════════════════════════
banner "DEMO 6 · Tick-time CS duration analysis (cs_experiment.sh, ${DURATION}s)"
# ══════════════════════════════════════════════════════════════════════════════
# Runs lhp_cstime then lhp_waittime sequentially, each with the workload.
# Shows kernel spinlock CS duration distribution and rseq wait-time breakdown.
"$SCRIPT_DIR/cs_experiment.sh" "$DURATION" "${WORKLOAD_CMD[@]+"${WORKLOAD_CMD[@]}"}"

# ══════════════════════════════════════════════════════════════════════════════
banner "DEMO 3 · Per-tick movability survey (lhp_movable, ${DURATION}s)"
# ══════════════════════════════════════════════════════════════════════════════
# Shows which tasks are movable (cpumask_weight > 1) at each scheduler tick.
# Non-movable tasks are pinned to a single CPU and cannot benefit from IVH.
echo "  Collecting movability data..."
WPID=$(start_workload)
timeout "$DURATION" "$MOVABLE_BIN" 2>/dev/null >"$MOVABLE_RAW" || true
stop_workload "$WPID"

NEVENTS=$(awk '/movable=/{c++} END{print c+0}' "$MOVABLE_RAW")
echo "  $NEVENTS tick events captured"
echo

python3 - "$MOVABLE_RAW" <<'PYEOF'
import sys, re, collections

data = []
with open(sys.argv[1]) as f:
    for line in f:
        m = re.search(r'pid (\d+) tgid \d+ \((\S+)\).*movable=(yes|no)', line)
        if m:
            data.append((m.group(2), m.group(3) == 'yes'))

if not data:
    print("  No events — is lhp_movable working and is a workload running?")
    sys.exit(0)

n   = len(data)
mov = sum(1 for _, m in data if m)
non = n - mov

print(f"  Total tick events  : {n:,}")
print(f"  Movable tasks      : {mov:,}  ({mov/n*100:.1f}%)")
print(f"  Non-movable tasks  : {non:,}  ({non/n*100:.1f}%)")
print()

by_comm = collections.defaultdict(lambda: [0, 0])
for comm, m in data:
    by_comm[comm][0 if m else 1] += 1

ranked = sorted(by_comm.items(), key=lambda x: -(x[1][0]+x[1][1]))[:14]
print(f"  {'comm':<22}  {'movable':>9}  {'non-movable':>11}  {'total':>8}")
print(f"  {'-'*22}  {'-'*9}  {'-'*11}  {'-'*8}")
for comm, (m, nm) in ranked:
    mark = " ← IVH candidate" if m > nm else ""
    print(f"  {comm:<22}  {m:>9,}  {nm:>11,}  {m+nm:>8,}{mark}")
PYEOF

# ══════════════════════════════════════════════════════════════════════════════
banner "DEMO 2 · Lockholder detection + shadow candidates (MY_ivh_atc, ${DURATION}s)"
# ══════════════════════════════════════════════════════════════════════════════
# Loads the full IVH BPF program.  Shadow candidate counters accumulate even
# when the IVH gates (capacity/util/runtime) prevent actual migration.
# This lets us measure lockholder frequency independent of IVH eligibility.
echo "  Loading MY_ivh_atc (this is the live IVH — migrations may fire)..."
"$IVH_BIN" 2>"$IVH_LOG" &
IVH_PID=$!
sleep 2   # allow BPF programs to load and attach (may be slow under heavy VM contention)

# Under VM contention the loader can crash silently — catch it early
if ! kill -0 "$IVH_PID" 2>/dev/null; then
    echo "  ERROR: MY_ivh_atc exited during startup — BPF load/attach failed."
    if [[ -s "$IVH_LOG" ]]; then
        echo "  Loader stderr:"
        tail -10 "$IVH_LOG" | sed 's/^/    /'
    fi
    echo "  Skipping LH detection analysis."
    IVH_PID=""
fi

[[ -z "$IVH_PID" ]] && { echo; echo "  Demo complete."; exit 0; }

# Capture map ID now — process is alive and maps are guaranteed visible
MAP_ID=$(bpftool map show 2>/dev/null \
    | awk '/name lhp_counters/{gsub(/:/,"",$1); print $1; exit}') || MAP_ID=""

WPID=$(start_workload)
echo "  Collecting for ${DURATION}s..."
sleep "$DURATION"
stop_workload "$WPID"

# Verify still alive before reading — may have died during collection under heavy contention
if ! kill -0 "$IVH_PID" 2>/dev/null; then
    echo "  WARNING: MY_ivh_atc died during collection — counters unavailable."
    if [[ -s "$IVH_LOG" ]]; then
        echo "  Loader output:"
        grep -Ev '^libbpf: (prog|CO-RE relocating|sec '"'"'|elf:|looking|collected|extern \(var|map.*at sec_idx|map.*found)' \
            "$IVH_LOG" | sed 's/^/    /'
    fi
elif [[ -n "$MAP_ID" ]]; then
    bpftool map dump id "$MAP_ID" -j 2>/dev/null >"$COUNTERS_JSON"
    python3 - "$COUNTERS_JSON" <<'PYEOF'
import sys, json

ENTRIES = [
    (0, "USER_MOVABLE ticks",       "user rseq CS, movable task"),
    (1, "USER_NONMOVABLE ticks",    "user rseq CS, pinned task"),
    (2, "KERNEL_MOVABLE ticks",     "kernel spinlock, movable task"),
    (3, "KERNEL_NONMOVABLE ticks",  "kernel spinlock, pinned task"),
    (4, "CANDIDATE_TOTAL",          "unique movable lockholders seen (one-shot)"),
    (5, "CAND_USER_MOVABLE",        "  ↳ user rseq, movable"),
    (7, "CAND_KERNEL_MOVABLE",      "  ↳ kernel lock, movable"),
]

with open(sys.argv[1]) as f:
    try:
        raw = json.load(f)
    except Exception as e:
        print(f"  Could not parse bpftool JSON: {e}")
        sys.exit(0)

totals = {}
for entry in raw:
    # bpftool -j uses "formatted" sub-object with plain int key/values
    fmt = entry.get("formatted", entry)
    key = int(fmt.get("key", 0))
    total = sum(int(v.get("value", 0)) for v in fmt.get("values", []))
    totals[key] = total

print(f"  {'Counter':<26}  {'Value':>10}  {'Description'}")
print(f"  {'-'*26}  {'-'*10}  {'-'*38}")
for idx, name, desc in ENTRIES:
    val = totals.get(idx, 0)
    print(f"  {name:<26}  {val:>10,}  {desc}")

print()
user_ticks = totals.get(0,0) + totals.get(1,0)
kern_ticks = totals.get(2,0) + totals.get(3,0)
candidates = totals.get(4,0)
print(f"  Summary:")
print(f"    User rseq lockholder ticks    : {user_ticks:,}")
print(f"    Kernel spinlock lockholder ticks: {kern_ticks:,}")
print(f"    Unique IVH candidates (movable) : {candidates:,}")
if user_ticks + kern_ticks > 0:
    frac = candidates / (user_ticks + kern_ticks) * 100
    print(f"    Candidate rate (cands/ticks)   : {frac:.2f}%")
PYEOF
else
    echo "  WARNING: lhp_counters map not visible right after startup — BPF map creation may have failed."
    if [[ -s "$IVH_LOG" ]]; then
        echo "  Loader output:"
        grep -Ev '^libbpf: (prog|CO-RE relocating|sec '"'"'|elf:|looking|collected|extern \(var|map.*at sec_idx|map.*found)' \
            "$IVH_LOG" | sed 's/^/    /'
    fi
fi

kill "$IVH_PID" 2>/dev/null || true
wait "$IVH_PID" 2>/dev/null || true

echo
echo "  Demo complete."
