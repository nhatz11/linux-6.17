#!/bin/bash
# Stage A detect-only run: hackbench loop for DUR seconds with counters snapshotted around it.
# env: CLR (owner_clear) MIG (universal_eligible) THR (spin threshold) DUR TAG
set -u; S=/proc/sys/kernel; SP=/tmp/claude-0/-root-linux-6-17/b98a4d93-d606-4bb7-bd13-7031a5eea896/scratchpad
CLR=${CLR:-1}; FAST=${FAST:-0}; MIG=${MIG:-1}; THR=${THR:-16777216}; DUR=${DUR:-60}; TAG=${TAG:-run}
OUT=/root/ivh_tools/cs_detect_${TAG}_$(date +%H%M%S)
echo $MIG > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null
echo 1 > $S/ivh_cs_owner_enable; echo $CLR > $S/ivh_cs_owner_clear; [ -e $S/ivh_cs_owner_fast ] && echo $FAST > $S/ivh_cs_owner_fast; echo 1 > $S/ivh_cs_head_probe; echo 0 > $S/ivh_cs_head_bail
dmesg -n 1
trap 'echo 32768 > $S/ivh_pv_spin_threshold; [ -e $S/ivh_cs_owner_fast ] && echo 0 > $S/ivh_cs_owner_fast' EXIT
QUIET=1 /root/ivh_tools/wait_capacity_settled.sh
echo $THR > $S/ivh_pv_spin_threshold
python3 /root/ivh_tools/phase0b_dump.py $OUT.before.json; t0=$(date +%s)
while [ $(( $(date +%s) - t0 )) -lt $DUR ]; do hackbench -T -g1 -f8 -l400000 2>&1 | grep -oP '^Time:\s*\K[0-9.]+' | sed 's/^/hackbench time=/'; done | tee $OUT.times
python3 /root/ivh_tools/phase0b_dump.py $OUT.after.json
echo "=== $TAG clr=$CLR fast=$FAST mig=$MIG thr=$THR elapsed=$(( $(date +%s) - t0 ))s ==="
python3 /root/ivh_tools/cs_stage_a.py $OUT.before.json $OUT.after.json $THR | tee $OUT.report
