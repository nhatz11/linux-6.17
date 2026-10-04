#!/bin/bash
set -u
LOG=${1:-/tmp/skipfair.log}; INTERVAL=${INTERVAL:-300}
SEEN=${SEEN:-/tmp/sfseen.txt}; touch "$SEEN"; prev=-1; stall=0
while :; do
  CSV=$(grep -oP 'skip fairness:.*-> \K\S+' "$LOG" 2>/dev/null | head -1)
  grep -q SKIPFAIR-DONE "$LOG" 2>/dev/null && exit 0
  a=$(pgrep -fc skipfair.sh 2>/dev/null); a=${a:-0}
  [ "$a" -eq 0 ] && { echo "DIED: harness gone."; tail -3 "$LOG"|sed 's/^/    /'; exit 1; }
  grep -q FATAL "$LOG" 2>/dev/null && { echo "FATAL: $(grep FATAL "$LOG"|tail -1)"; exit 1; }
  if [ -n "${CSV:-}" ] && [ -s "$CSV" ]; then
    rows=$(($(wc -l < "$CSV")-1))
    if [ "$rows" -eq "$prev" ]; then stall=$((stall+1)); [ "$stall" -ge 2 ] && echo "STALLED at $rows"; else stall=0; fi
    prev=$rows
    python3 - "$CSV" "$SEEN" <<'PY'
import csv,sys
rows=list(csv.DictReader(open(sys.argv[1]))); bad=[]
for r in rows[-12:]:
    try:
        arm=r['arm']; perf=float(r['perf']); ev=int(r['wait_events'])
        t1=int(r['t1_fired']); sk=int(r['evict_marked'])
    except Exception: continue
    tag=f"{r['workload']}/{arm}/r{r['rep']}"
    if perf<=0: bad.append(f"NO-METRIC {tag}")
    if ev==0:   bad.append(f"NO-EVENTS {tag} wait_events=0")
    if arm.endswith('t1off') and t1>0: bad.append(f"T1-LEAK {tag} tier1 fired {t1} with tier1 OFF")
    if arm.startswith('base') and sk>0: bad.append(f"SKIP-LEAK {tag} evictions {sk} in a no-skip arm")
seen=set(open(sys.argv[2]).read().split('\n'))
new=[b for b in dict.fromkeys(bad) if b not in seen]
for b in new: print(b)
if new:
    with open(sys.argv[2],'a') as f:
        for b in new: f.write(b+'\n')
PY
  fi
  sleep "$INTERVAL"
done
