#!/bin/bash
# finalcombo watchdog. Silent while healthy; each line printed is actionable.
#   DIED / STALLED / FATAL
#   NO-METRIC   perf == 0
#   NO-MIG      an arm did 0 migrations -> the BASELINE mechanism is dead, so
#               every "vs mig" number in that row is meaningless
#   NO-WAIT     wait_events == 0
#   DEAD-FACTOR a factor that should be ON fired 0 in that arm
#   LEAK        a factor that should be OFF fired > 0
# Findings report once each (persistent seen-file).
set -u
LOG=${1:-/tmp/fcA.log}; INTERVAL=${INTERVAL:-300}
SEEN=${SEEN:-/tmp/fcseen.txt}; touch "$SEEN"; prev=-1; stall=0
while :; do
  CSV=$(grep -oP 'FINAL COMBO phase .*-> \K\S+' "$LOG" 2>/dev/null | head -1)
  grep -qE "FINALCOMBO-[AB]-DONE" "$LOG" 2>/dev/null && exit 0
  a=$(pgrep -fc finalcombo.sh 2>/dev/null); a=${a:-0}
  [ "$a" -eq 0 ] && { echo "DIED: harness gone."; tail -3 "$LOG" | sed 's/^/    /'; exit 1; }
  grep -q FATAL "$LOG" 2>/dev/null && { echo "FATAL: $(grep FATAL "$LOG" | tail -1)"; exit 1; }
  if [ -n "${CSV:-}" ] && [ -s "$CSV" ]; then
    rows=$(($(wc -l < "$CSV") - 1))
    if [ "$rows" -eq "$prev" ]; then
      stall=$((stall+1)); [ "$stall" -ge 2 ] && echo "STALLED: $rows rows for $((stall*INTERVAL/60)) min"
    else stall=0; fi
    prev=$rows
    python3 - "$CSV" "$SEEN" <<'PY'
import csv, sys
rows = list(csv.DictReader(open(sys.argv[1])))
bad = []
for r in rows[-20:]:
    try:
        arm=r['arm']; perf=float(r['perf']); ev=int(r['wait_events']); mg=int(r['migs'])
        t1=int(r['t1_fired']); t2=int(r['t2_fired']); heh=int(r['heh_bailed'])
        hb=int(r['bypass_fired']); sk=int(r['evict_marked'])
    except Exception:
        continue
    tag=f"{r['workload']}/{arm}/r{r['rep']}"
    if perf<=0: bad.append(f"NO-METRIC {tag}")
    if ev==0:   bad.append(f"NO-WAIT {tag} wait_events=0")
    if mg==0:   bad.append(f"NO-MIG {tag} -- baseline mechanism dead, row is meaningless")
    # factors that must be ON for this arm, by name
    want_t1  = arm in ('mig_heh_hb_sk_t1','mig_t1','mig_t1_heh','mig_t1t2_heh')
    want_t2  = arm == 'mig_t1t2_heh'
    want_heh = 'heh' in arm
    want_hb  = '_hb' in arm
    want_sk  = '_sk' in arm
    if want_t1 and t1==0:  bad.append(f"DEAD-FACTOR {tag} tier1 fired 0")
    if want_t2 and t2==0:  bad.append(f"DEAD-FACTOR {tag} tier2 fired 0")
    if want_hb and hb==0:  bad.append(f"DEAD-FACTOR {tag} bypass fired 0")
    if want_sk and sk==0:  bad.append(f"DEAD-FACTOR {tag} skip fired 0")
    if not want_t1 and t1>0: bad.append(f"LEAK {tag} tier1 fired {t1} with t1 OFF")
    if not want_t2 and t2>0: bad.append(f"LEAK {tag} tier2 fired {t2} with t2 OFF")
    if not want_hb and hb>0: bad.append(f"LEAK {tag} bypass fired {hb} with hb OFF")
    if not want_sk and sk>0: bad.append(f"LEAK {tag} skip fired {sk} with skip OFF")
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
