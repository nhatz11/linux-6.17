#!/bin/bash
set -u
LOG=${1:-/tmp/spinhalt.log}; INTERVAL=${INTERVAL:-300}
SEEN=${SEEN:-/tmp/shseen.txt}; touch "$SEEN"; prev=-1; stall=0
while :; do
  CSV=$(grep -oP 'spin/halt:.*-> \K\S+' "$LOG" 2>/dev/null | head -1)
  grep -q SPINHALT-DONE "$LOG" 2>/dev/null && exit 0
  alive=$(pgrep -fc spinhalt_full.sh 2>/dev/null); alive=${alive:-0}
  [ "$alive" -eq 0 ] && { echo "DIED: harness gone."; tail -3 "$LOG"|sed 's/^/    /'; exit 1; }
  grep -q FATAL "$LOG" 2>/dev/null && { echo "FATAL: $(grep FATAL "$LOG"|tail -1)"; exit 1; }
  if [ -n "${CSV:-}" ] && [ -s "$CSV" ]; then
    rows=$(($(wc -l < "$CSV")-1))
    if [ "$rows" -eq "$prev" ]; then stall=$((stall+1)); [ "$stall" -ge 2 ] && echo "STALLED at $rows rows"; else stall=0; fi
    prev=$rows
    python3 - "$CSV" "$SEEN" <<'PY'
import csv,sys
rows=list(csv.DictReader(open(sys.argv[1]))); bad=[]
for r in rows[-20:]:
    try:
        arm=r['arm']; perf=float(r['perf']); wall=int(r['wall_ns'])
        ht=int(r['halt_cyc_total']); h2=int(r['halt_cyc_t2']); fb=int(r['bypass_fired'])
    except Exception: continue
    tag=f"{r['workload']}/{arm}/r{r['rep']}"
    if perf<=0: bad.append(f"NO-METRIC {tag}")
    if wall==0: bad.append(f"NO-WAIT {tag} wall=0")
    if ht==0:   bad.append(f"NO-HALT {tag} halt_cycles=0 -- decomposition is dead")
    if arm.startswith('t2') and h2==0: bad.append(f"NO-T2HALT {tag} tier2 attributed 0 cycles")
    if arm in ('t2_hb','t2_hb_skip') and fb==0: bad.append(f"NO-BYPASS {tag} bypass fired 0")
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
