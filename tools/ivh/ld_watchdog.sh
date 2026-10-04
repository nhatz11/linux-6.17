#!/bin/bash
# Silent while healthy. Each line printed is actionable.
set -u
LOG=${1:-/tmp/ladder.log}; INTERVAL=${INTERVAL:-300}
SEEN=${SEEN:-/tmp/ldseen.txt}; touch "$SEEN"; prev=-1; stall=0
while :; do
  CSV=$(grep -oP 'LADDER:.*-> \K\S+' "$LOG" 2>/dev/null | head -1)
  grep -q "LADDER-DONE" "$LOG" 2>/dev/null && exit 0
  a=$(pgrep -fc "bash ./ladder.sh|bash /root/ivh_tools/ladder.sh" 2>/dev/null); a=${a:-0}
  [ "$a" -eq 0 ] && { echo "DIED: harness gone."; tail -3 "$LOG"|sed 's/^/    /'; exit 1; }
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
        arm=r['arm']; perf=float(r['perf']); it=int(r['spin_iters'])
        t1=int(r['t1_fired']); t2=int(r['t2_fired']); heh=int(r['heh_bailed'])
        hb=int(r['bypass_fired']); sk=int(r['evict_marked'])
    except Exception: continue
    tag=f"{r['workload']}/{arm}/r{r['rep']}"
    if perf<=0: bad.append(f"NO-METRIC {tag}")
    if it==0:   bad.append(f"NO-SPIN {tag} spin_iters=0 -- the metric is dead")
    if t1==0:   bad.append(f"DEAD-FACTOR {tag} tier1 fired 0 (on in every arm)")
    w_t2  = '_t2' in arm; w_heh = '_heh' in arm
    w_sk  = '_sk' in arm; w_hb  = '_hb' in arm
    if w_t2 and t2==0:      bad.append(f"DEAD-FACTOR {tag} tier2 fired 0")
    if w_hb and hb==0:      bad.append(f"DEAD-FACTOR {tag} bypass fired 0")
    if not w_t2 and t2>0:   bad.append(f"LEAK {tag} tier2 fired {t2} with t2 OFF")
    if not w_heh and heh>0: bad.append(f"LEAK {tag} heh fired {heh} with heh OFF")
    if not w_sk and sk>0:   bad.append(f"LEAK {tag} skip fired {sk} with skip OFF")
    if not w_hb and hb>0:   bad.append(f"LEAK {tag} bypass fired {hb} with hb OFF")
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
