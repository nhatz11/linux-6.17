#!/bin/bash
# word_count: PV vs NOHALT (spin threshold max = never halt, pure busy spin) vs IVH.
# If NOHALT differs from PV, the kernel spinlock halt path matters for this workload,
# i.e. there is real LHP to solve. Order rotates so drift cancels.
set -u
S=/proc/sys/kernel; W="/root/bench/phoenix/phoenix-2.0/tests/word_count/word_count /root/bench/data_wc.txt"
OUT=/root/ivh_tools/wc_three_arm_$(date +%H%M%S).csv; echo "block,arm,secs" > $OUT
arm() { case $1 in
  pv)     echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1; echo 32768    > $S/ivh_pv_spin_threshold ;;
  nohalt) echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1; echo 16777216 > $S/ivh_pv_spin_threshold ;;
  ivh)    echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1; echo 32768    > $S/ivh_pv_spin_threshold ;;
  esac; }
trap 'arm ivh; echo 32768 > $S/ivh_pv_spin_threshold; echo restored' EXIT
for b in 1 2 3 4; do
  QUIET=1 MIN_S=45 /root/ivh_tools/wait_capacity_settled.sh >/dev/null 2>&1
  case $((b%2)) in 1) ORD="pv nohalt ivh ivh nohalt pv";; 0) ORD="ivh nohalt pv pv nohalt ivh";; esac
  for a in $ORD; do
    arm $a
    T0=$(date +%s.%N); for i in 1 2 3 4 5; do $W >/dev/null 2>&1; done; T1=$(date +%s.%N)
    V=$(echo "$T1-$T0" | bc); echo "$b,$a,$V" >> $OUT; echo "blk$b $a = ${V}s"
  done
done
python3 - "$OUT" <<'PY'
import csv,sys,statistics as st
d={}
for r in csv.DictReader(open(sys.argv[1])): d.setdefault(r['arm'],[]).append(float(r['secs']))
pv=st.mean(d['pv'])
print()
for a in ('pv','nohalt','ivh'):
    m=st.mean(d[a]); print(f"{a:7s} mean {m:6.2f}s   vs PV {100*(pv-m)/pv:+6.1f}%   n={len(d[a])}")
PY
