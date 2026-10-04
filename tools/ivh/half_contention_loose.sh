#!/bin/bash
# Half contention, NEW (loose) gate: I P P I. I = loose MY_ivh_atc, 3 rounds; P = PV, 1 round.
set -u
S=/proc/sys/kernel; W="hackbench -T -g1 -f8 -l400000"
B=/tmp/claude-0/-root-linux-6-17/b98a4d93-d606-4bb7-bd13-7031a5eea896/scratchpad/atc_build
ORIG=/root/kernels/linux-6.17-vanilla/tools/bpf/MY_ivh_atc
OUT=/root/ivh_tools/half_contention_loose_$(date +%H%M%S)
log() { echo "$*" | tee -a $OUT.log; }
start_atc() {
    echo 0 > $S/ivh_universal_eligible
    pkill -9 -x MY_ivh_atc; for i in $(seq 50); do pgrep -x MY_ivh_atc >/dev/null || break; sleep 0.2; done
    setsid nohup "$1" > /root/ivh_logs/atc.log 2>&1 < /dev/null &
    for i in $(seq 40); do bpftool map lookup name ivh_cfg key 0 0 0 0 >/dev/null 2>&1 && break; sleep 0.25; done
    bpftool map update name ivh_cfg key 0 0 0 0 value "$(cat $S/ivh_cap_source)" 0 0 0 || { log "FATAL: ivh_cfg update failed"; exit 1; }
}
trap 'start_atc "$ORIG"; echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null; log "restored original daemon, IVH+AS (atc=$(pgrep -xc MY_ivh_atc))"' EXIT
dmesg -n 1
start_atc $B/loose/MY_ivh_atc; log "loose daemon up (atc=$(pgrep -xc MY_ivh_atc))"
for arm in I P P I; do
    if [ $arm = I ]; then echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null; n=3
    else echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null; n=1; fi
    QUIET=1 /root/ivh_tools/wait_capacity_settled.sh | tee -a $OUT.log
    python3 /root/ivh_tools/drift_snap.py "$arm:start" >> $OUT.snaps.jsonl
    for r in $(seq 1 $n); do
        v=$(timeout 150 $W 2>&1 | grep -oP '^Time:\s*\K[0-9.]+'); v=${v:-TIMEOUT}
        python3 /root/ivh_tools/drift_snap.py "$arm:r$r:$v" >> $OUT.snaps.jsonl
        log "arm $arm round $r time=${v}s"
    done
done
