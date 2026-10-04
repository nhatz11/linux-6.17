#!/bin/bash
# Early stop for the t12+bypass sweep.
#
# RULE: once >=3 workloads have all 4 arms at n>=4, test whether BOTH t12b arms
# lose to PV on wait/acq in EVERY completed workload. If so the verdict cannot
# change by continuing -- kill the harness and say so. Anything mixed keeps
# running, because a mixed result is exactly what needs full n.
set -u
LOG=${1:-/tmp/t12b.log}
while :; do
  CSV=$(grep -oP 't12\+bypass:.*-> \K\S+' "$LOG" 2>/dev/null | head -1)
  grep -q T12B-DONE "$LOG" 2>/dev/null && exit 0
  pgrep -f t12bypass_full.sh >/dev/null || exit 0
  if [ -n "${CSV:-}" ] && [ -s "$CSV" ]; then
    python3 - "$CSV" <<'PY'
import csv,sys,collections,statistics as st
rows=list(csv.DictReader(open(sys.argv[1])))
d=collections.defaultdict(lambda: collections.defaultdict(list))
for r in rows:
    try: d[r['workload']][r['arm']].append((float(r['perf']),int(r['wait_ns']),int(r['wait_events'])))
    except Exception: pass
done=[w for w,m in d.items() if all(len(m.get(a,[]))>=4 for a in ('pv','t1','t12b_1ms','t12b_100us'))]
if len(done)<3: raise SystemExit
verdict=[]
for w in done:
    m=d[w]
    pa=st.mean([x[1]/max(x[2],1) for x in m['pv']])
    worse=all(st.mean([x[1]/max(x[2],1) for x in m[a]])>pa for a in ('t12b_1ms','t12b_100us'))
    verdict.append(worse)
if all(verdict):
    print("EARLY-STOP: both t12b arms lose to PV on wait/acq in all "
          f"{len(done)} completed workloads ({', '.join(done)}). Verdict cannot change.")
    sys.exit(42)
PY
    [ $? -eq 42 ] && { pgrep -f t12bypass_full.sh | grep -v $$ | xargs -r kill; echo "harness stopped"; exit 42; }
  fi
  sleep 120
done
