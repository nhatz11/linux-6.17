#!/bin/bash
# Point-7 watchdog. Prints NOTHING while healthy; every line it prints is a
# problem worth acting on. Exits when the run finishes or dies.
#
# Failure modes it catches (each one has actually happened in this project):
#   DIED        harness gone without P8FULL-DONE
#   STALLED     no new CSV rows in two consecutive checks
#   FATAL       an arm assert tripped (threshold rejected, wrong spin_mode, ...)
#   NO-METRIC   perf == 0: the benchmark produced no parseable number
#   NO-MIGS     an IVH arm did 0 migrations -> host contention gone, or a gate shut
#   NO-BPF      migrations happened but bpftrace caught none -> probe not attached
#   NO-WAIT     wait_ns == 0 -> ivh_slowpath_wait_measure got cleared
#   ALL-UNSET   every IVH row of a workload capacity-unsettled -> nothing comparable
#   TMPFS       /dev/shm filling (the fs_mark failure mode)
set -u
LOG=${1:-/tmp/p7full.log}
INTERVAL=${INTERVAL:-300}
prev_rows=-1
stall=0
while :; do
  CSV=$(grep -oP 'point 8:.*-> \K\S+' "$LOG" 2>/dev/null | head -1)
  alive=$(pgrep -fc point8_full.sh 2>/dev/null); alive=${alive:-0}
  done_=$(grep -c P8FULL-DONE "$LOG" 2>/dev/null); done_=${done_:-0}

  if [ "$done_" -gt 0 ]; then exit 0; fi
  if [ "$alive" -eq 0 ]; then
    echo "DIED: harness not running and no P8FULL-DONE. tail:"
    tail -3 "$LOG" 2>/dev/null | sed 's/^/    /'
    exit 1
  fi

  if grep -q FATAL "$LOG" 2>/dev/null; then
    echo "FATAL in log: $(grep FATAL "$LOG" | tail -1)"
    exit 1
  fi

  if [ -n "$CSV" ] && [ -s "$CSV" ]; then
    rows=$(($(wc -l < "$CSV") - 1))
    if [ "$rows" -eq "$prev_rows" ]; then
      stall=$((stall + 1))
      if [ "$stall" -ge 2 ] && [ "$INTERVAL" -ge 60 ]; then
        echo "STALLED: still $rows rows after $((stall * INTERVAL / 60)) min"
      fi
    else
      stall=0
    fi
    prev_rows=$rows
    # inspect only rows added since the last check
    python3 - "$CSV" <<'PY'
import csv, sys, collections
rows = list(csv.DictReader(open(sys.argv[1])))
if not rows: raise SystemExit
recent = rows[-12:]
bad = []
for r in recent:
    try:
        arm = int(r['arm_ns']); perf = float(r['perf'])
        migs = int(r['migs']); mign = int(r['mig_n']); wait = int(r['wait_ns'])
    except Exception:
        continue
    tag = f"{r['workload']}/{'PV' if arm==0 else str(arm//1000)+'us'}/r{r['rep']}"
    if perf <= 0:                      bad.append(f"NO-METRIC {tag} perf=0")
    if arm and migs == 0:              bad.append(f"NO-MIGS {tag} (contention gone or gate shut)")
    if arm and migs > 100 and mign==0: bad.append(f"NO-BPF {tag} migs={migs} but probe caught 0")
    if wait == 0:                      bad.append(f"NO-WAIT {tag} wait_ns=0 (wait_measure cleared?)")
for b in dict.fromkeys(bad): print(b)
# a whole workload with every IVH row unsettled is not comparable
byw = collections.defaultdict(list)
for r in rows:
    try:
        if int(r['arm_ns']): byw[r['workload']].append(int(r['g1_reject']))
    except Exception: pass
for w, g in byw.items():
    if len(g) >= 8 and max(g) < 50000:
        print(f"ALL-UNSET {w}: {len(g)} IVH rows, max g1_reject={max(g):,} -- none comparable")
PY
  fi

  used=$(df --output=pcent /dev/shm 2>/dev/null | tail -1 | tr -dc '0-9')
  [ -n "$used" ] && [ "$used" -ge 80 ] && echo "TMPFS: /dev/shm ${used}% full"

  sleep "$INTERVAL"
done
