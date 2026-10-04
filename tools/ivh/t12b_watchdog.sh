#!/bin/bash
# Silent while healthy. Findings report once each (persistent seen-file).
#   DIED / STALLED / FATAL
#   NO-METRIC   perf == 0
#   NO-WAIT     wait_ns == 0  -> wait_measure cleared
#   NO-FIRE     a t12b arm logged 0 bypass or 0 tier2 -> the mechanism under
#               test did not engage, so that row measures nothing
#   SKIP-LEAK   any arm recorded evictions (skipping must be off everywhere)
set -u
LOG=${1:-/tmp/t12b.log}; INTERVAL=${INTERVAL:-300}
SEEN=${SEEN:-/tmp/t12bseen.txt}; touch "$SEEN"
prev=-1; stall=0
while :; do
  CSV=$(grep -oP 't12\+bypass:.*-> \K\S+' "$LOG" 2>/dev/null | head -1)
  alive=$(pgrep -fc t12bypass_full.sh 2>/dev/null); alive=${alive:-0}
  done_=$(grep -c T12B-DONE "$LOG" 2>/dev/null); done_=${done_:-0}
  [ "$done_" -gt 0 ] && exit 0
  if [ "$alive" -eq 0 ]; then
    echo "DIED: harness gone, no T12B-DONE. tail:"; tail -3 "$LOG" | sed 's/^/    /'; exit 1
  fi
  grep -q FATAL "$LOG" 2>/dev/null && { echo "FATAL: $(grep FATAL "$LOG" | tail -1)"; exit 1; }
  if [ -n "$CSV" ] && [ -s "$CSV" ]; then
    rows=$(($(wc -l < "$CSV") - 1))
    if [ "$rows" -eq "$prev" ]; then
      stall=$((stall+1)); [ "$stall" -ge 2 ] && echo "STALLED: still $rows rows after $((stall*INTERVAL/60)) min"
    else stall=0; fi
    prev=$rows
    python3 - "$CSV" "$SEEN" <<'PY'
import csv,sys
rows=list(csv.DictReader(open(sys.argv[1]))); bad=[]
for r in rows[-16:]:
    try:
        arm=r['arm']; perf=float(r['perf']); wait=int(r['wait_ns'])
        t2=int(r['t2_fired']); fb=int(r['bypass_fired'])
    except Exception: continue
    tag=f"{r['workload']}/{arm}/r{r['rep']}"
    if perf<=0: bad.append(f"NO-METRIC {tag}")
    if wait==0: bad.append(f"NO-WAIT {tag} (wait_measure cleared?)")
    if arm.startswith('t12b') and (t2==0 or fb==0):
        bad.append(f"NO-FIRE {tag} tier2={t2} bypass={fb} -- mechanism under test did not engage")
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
  sleep "$INTERVAL"
done
