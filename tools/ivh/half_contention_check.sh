#!/bin/bash
# Half contention (co-runner on vCPUs 0-7): IVH+AS (original daemon, normal gate) vs PV.
# ABBA order I P P I, ROUNDS consecutive rounds per arm, capacity-settled wait before each arm.
set -u
ROUNDS=${ROUNDS:-3}; S=/proc/sys/kernel; W="hackbench -T -g1 -f8 -l400000"
OUT=/root/ivh_tools/half_contention_$(date +%H%M%S)
log() { echo "$*" | tee -a $OUT.log; }
trap 'echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null; log "restored IVH+AS"' EXIT
dmesg -n 1
log "atc=$(pgrep -xc MY_ivh_atc) vcap=$(pgrep -xc vcap)"
for arm in I P P I; do
    if [ $arm = I ]; then echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null
    else echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null; fi
    QUIET=1 /root/ivh_tools/wait_capacity_settled.sh | tee -a $OUT.log
    python3 /root/ivh_tools/drift_snap.py "$arm:start" >> $OUT.snaps.jsonl
    for r in $(seq 1 $ROUNDS); do
        v=$(timeout 150 $W 2>&1 | grep -oP '^Time:\s*\K[0-9.]+'); v=${v:-TIMEOUT}
        python3 /root/ivh_tools/drift_snap.py "$arm:r$r:$v" >> $OUT.snaps.jsonl
        log "arm $arm round $r time=${v}s"
    done
done
