#!/bin/bash
# Point-11 watchdog. Silent while healthy; every line printed is a real problem.
#   DIED / STALLED / FATAL   harness gone, no new rows, or an arm assert tripped
#   NO-METRIC   perf == 0: benchmark produced no parseable number
#   NO-WAIT     wait_ns == 0: ivh_slowpath_wait_measure got cleared
#   NO-EVICT    the LOWEST-threshold arm did 0 evictions -> the knob is dead on
#               that workload. A HIGH-threshold arm firing zero is the knob
#               reaching its off-end -- expected sweep shape, NOT flagged.
#
# The prerequisite sysctls are deliberately NOT polled from here. The harness
# toggles them per arm (the PV arm sets evict_enable=0 by design) and asserts
# each one inside setarm with a FATAL exit, so an outside poll can only race
# with the arm switch and report a loss that is not one. FATAL in the log is
# the correct signal, and it is already checked below.
#   MIGRATED    migs > 0 on any arm -> eligibility gate leaked, sweep confounded
#   TMPFS       /dev/shm filling (the fs_mark failure mode)
set -u
LOG=${1:-/tmp/p11full.log}; INTERVAL=${INTERVAL:-300}
prev=-1; stall=0
SEEN=${SEEN:-/tmp/p11seen.txt}; touch "$SEEN"
while :; do
  CSV=$(grep -oP 'point 11:.*-> \K\S+' "$LOG" 2>/dev/null | head -1)
  alive=$(pgrep -fc point11_full.sh 2>/dev/null); alive=${alive:-0}
  done_=$(grep -c P11FULL-DONE "$LOG" 2>/dev/null); done_=${done_:-0}
  [ "$done_" -gt 0 ] && exit 0
  if [ "$alive" -eq 0 ]; then
    echo "DIED: harness gone, no P11FULL-DONE. tail:"; tail -3 "$LOG" | sed 's/^/    /'; exit 1
  fi
  if grep -q FATAL "$LOG" 2>/dev/null; then
    echo "FATAL: $(grep FATAL "$LOG" | tail -1)"; exit 1
  fi
  if [ -n "$CSV" ] && [ -s "$CSV" ]; then
    rows=$(($(wc -l < "$CSV") - 1))
    if [ "$rows" -eq "$prev" ]; then
      stall=$((stall+1))
      [ "$stall" -ge 2 ] && echo "STALLED: still $rows rows after $((stall*INTERVAL/60)) min"
    else stall=0; fi
    prev=$rows
    python3 - "$CSV" "$SEEN" <<'PY'
import csv,sys
rows=list(csv.DictReader(open(sys.argv[1])))
arms=[int(r['arm_cyc']) for r in rows if r['arm_cyc'].isdigit() and int(r['arm_cyc'])]
LOWEST=min(arms) if arms else 0
bad=[]
for r in rows[-12:]:
    try:
        arm=int(r['arm_cyc']); perf=float(r['perf']); wait=int(r['wait_ns'])
        mk=int(r['ev_marked']); mg=int(r['migs'])
    except Exception: continue
    tag=f"{r['workload']}/{'PV' if not arm else str(arm//2200)+'us'}/r{r['rep']}"
    if perf<=0:            bad.append(f"NO-METRIC {tag}")
    if wait==0:            bad.append(f"NO-WAIT {tag} (wait_measure cleared?)")
    if arm==LOWEST and mk==0: bad.append(f"NO-EVICT {tag} at the LOWEST threshold -- knob dead")
    if mg>0:               bad.append(f"MIGRATED {tag} migs={mg} -- gate leaked")
seen=set()
try: seen=set(open(sys.argv[2]).read().split('\n'))
except Exception: pass
new=[b for b in dict.fromkeys(bad) if b not in seen]
for b in new: print(b)
if new:
    with open(sys.argv[2],'a') as f:
        for b in new: f.write(b+'\n')
PY
  fi
  used=$(df --output=pcent /dev/shm 2>/dev/null | tail -1 | tr -dc '0-9')
  [ -n "$used" ] && [ "$used" -ge 80 ] && echo "TMPFS: /dev/shm ${used}% full"
  sleep "$INTERVAL"
done
