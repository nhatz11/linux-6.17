#!/bin/bash
# Does the contended/idle capacity differential survive the new calibration?
# No probers: cpus 0-14 are host-contended by the sysbench VM, cpu15 is not.
# We want 15 clearly ABOVE 0-14 at both settings.
set -u; S=/proc/sys/kernel; T=/root/ivh_tools
SETTLE=${SETTLE:-75}
for cfg in "100 50000 shipped" "100 1000 tuned"; do
  set -- $cfg; P=$1; D=$2; NAME=$3
  echo "$P" > $S/ivh_tks_phase_pct; echo "$D" > $S/ivh_tks_deadband_ns
  echo "### $NAME (phase_pct=$P deadband=$D) -- settling ${SETTLE}s"
  sleep $SETTLE
  for i in 1 2 3 4 5; do
    python3 $T/read_vact_rq.py ivh_uc_capacity 2>/dev/null \
      | sed 's/.*per-cpu=\[//;s/\].*//' | tr -d ' '
    sleep 4
  done | python3 -c '
import sys
rows=[[int(x) for x in l.strip().split(",")] for l in sys.stdin if l.strip()]
n=len(rows[0]); avg=[sum(r[i] for r in rows)/len(rows) for i in range(n)]
cont=avg[:15]; idle=avg[15]
print("   per-cpu capacity:", " ".join(f"{v:.0f}" for v in avg))
print(f"   contended(0-14) mean={sum(cont)/len(cont):.0f} min={min(cont):.0f} max={max(cont):.0f}")
print(f"   uncontended(15)  = {idle:.0f}")
print(f"   DIFFERENTIAL     = {idle-sum(cont)/len(cont):+.0f}")
'
  echo
done
echo "DONE"
