#!/bin/bash
# dedup's real wait, pv_t1 vs mig_t1. Usage: dedup_wait.sh [arm]
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
ARM="${1:-pv_t1}"; SEC="${2:-200}"
case $ARM in
  pv_t1)  bash $T/pvbase.sh >/dev/null 2>&1 ;;
  mig_t1) bash $T/p7v2_arm.sh 2500000 >/dev/null 2>&1
          echo 0 > $S/ivh_pv_tier2_enable; echo 1 > $S/ivh_cs_gate2_reference ;;
esac
echo 1 > $S/ivh_pv_tier1_enable; sleep 1
sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
BT=/root/ivh_logs/dedupwait_${ARM}_$(date +%m%d-%H%M%S).txt
bpftrace $T/dedup_wait.bt "$SEC" > "$BT" 2>&1 & BTPID=$!
sleep 2
s=$(date +%s%N)
( cd /root/parsec-benchmark && timeout 600 ./bin/parsecmgmt -a run -p dedup -c gcc -i native -n 16 >/dev/null 2>&1 )
e=$(date +%s%N)
kill -INT $BTPID 2>/dev/null; wait $BTPID 2>/dev/null
WALL=$(python3 -c "print(f'{($e-$s)/1e9:.2f}')")
python3 - "$BT" "$ARM" "$WALL" <<'PY'
import re,sys
t=open(sys.argv[1]).read()
def top(k,n=3):
    v=re.findall(r'@'+k+r'\[([^\]]+)\]:\s*(\d+)',t)
    return sorted(((int(x),c) for c,x in v),reverse=True)[:n]
print(f"  ===== dedup {sys.argv[2]}, wall {sys.argv[3]} s =====")
for k,lbl,div in (('futex_ns','FUTEX blocking',1e6),('offcpu_ns','INVOLUNTARY off-CPU',1e6)):
    v=top(k)
    if not v: print(f"    {lbl:22s} (none)"); continue
    print(f"    {lbl}:")
    for val,c in v: print(f"      {c:<20s} {val/div:12.1f} ms")
for k,lbl in (('futex_n','futex calls'),('preempted','preemptions'),('offcpu_n','resumes')):
    v=top(k,2)
    for val,c in v: print(f"    {lbl:22s} {c:<20s} {val}")
PY
