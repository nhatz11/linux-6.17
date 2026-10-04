#!/bin/bash
set -u
LOG=${1:-/tmp/advisor.log}; INTERVAL=${INTERVAL:-300}
SEEN=${SEEN:-/tmp/advseen.txt}; touch "$SEEN"; prev=-1; stall=0
while :; do
  CSV=$(grep -oP 'advisor sweep:.*-> \K\S+' "$LOG" 2>/dev/null | head -1)
  grep -q ADVISOR-DONE "$LOG" 2>/dev/null && exit 0
  a=$(pgrep -fc advisor_sweep.sh 2>/dev/null); a=${a:-0}
  [ "$a" -eq 0 ] && { echo "DIED: harness gone."; tail -3 "$LOG"|sed 's/^/    /'; exit 1; }
  grep -q FATAL "$LOG" 2>/dev/null && { echo "FATAL: $(grep FATAL "$LOG"|tail -1)"; exit 1; }
  if [ -n "${CSV:-}" ] && [ -s "$CSV" ]; then
    rows=$(($(wc -l < "$CSV")-1))
    if [ "$rows" -eq "$prev" ]; then stall=$((stall+1)); [ "$stall" -ge 2 ] && echo "STALLED at $rows rows"; else stall=0; fi
    prev=$rows
    python3 - "$CSV" "$SEEN" <<'PY'
import csv,sys
rows=list(csv.DictReader(open(sys.argv[1]))); bad=[]
for r in rows[-22:]:
    try:
        arm=r['arm']; perf=float(r['perf']); wall=int(r['wall_ns']); ht=int(r['halt_cyc'])
        t2=int(r['t2_fired'])
    except Exception: continue
    tag=f"{r['workload']}/{arm}/r{r['rep']}"
    if perf<=0: bad.append(f"NO-METRIC {tag}")
    if ht==0:   bad.append(f"NO-HALT {tag} -- decomposition dead")
    if wall and (wall-ht/2.2)<0: bad.append(f"NEG-ONCPU {tag} halt exceeds wall -- clock mismatch")
    if arm.startswith(('t2@','all@')) and t2==0: bad.append(f"NO-T2 {tag} tier2 arm fired 0")
    if not arm.startswith(('t2@','all@')) and t2>0: bad.append(f"T2-LEAK {tag} tier2 fired {t2} in a non-t2 arm")
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
