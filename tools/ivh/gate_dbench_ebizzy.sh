#!/bin/bash
# Old vs new capacity gate on the two confirmed migration wins, at the current
# co-runner setting. Per workload: N L P L N  (N = original daemon / old gate,
# L = loose daemon / new gate: 3 rounds each; P = PV: 2 rounds).
# Capacity-settled wait before each arm. Higher is better for both metrics.
set -u
S=/proc/sys/kernel
B=/tmp/claude-0/-root-linux-6-17/b98a4d93-d606-4bb7-bd13-7031a5eea896/scratchpad/atc_build
ORIG=/root/kernels/linux-6.17-vanilla/tools/bpf/MY_ivh_atc
OUT=/root/ivh_tools/gate_dbench_ebizzy_$(date +%H%M%S)
log() { echo "$*" | tee -a $OUT.log; }
start_atc() {
    echo 0 > $S/ivh_universal_eligible
    pkill -9 -x MY_ivh_atc; for i in $(seq 50); do pgrep -x MY_ivh_atc >/dev/null || break; sleep 0.2; done
    setsid nohup "$1" > /root/ivh_logs/atc.log 2>&1 < /dev/null &
    for i in $(seq 40); do bpftool map lookup name ivh_cfg key 0 0 0 0 >/dev/null 2>&1 && break; sleep 0.25; done
    bpftool map update name ivh_cfg key 0 0 0 0 value "$(cat $S/ivh_cap_source)" 0 0 0 || { log "FATAL: ivh_cfg update failed"; exit 1; }
}
trap 'start_atc "$ORIG"; echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null; log "restored original daemon, IVH+AS (atc=$(pgrep -xc MY_ivh_atc))"' EXIT
dmesg -n 1; D0=$(dmesg | grep -ciE 'soft lockup|rcu.*stall|hung task')

declare -A CMD=(
  [dbench]="dbench -F -t 12 16 -D /root/dbench_test"
  [ebizzy]="/home/nick/Desktop/ebizzy -S 20 -t 16 -m -s 4194304"
)
declare -A EXT=(
  [dbench]="grep -oP 'Throughput\s+\K[0-9.]+'"
  [ebizzy]="grep -oP '^\K[0-9]+(?= records/s)'"
)
for w in dbench ebizzy; do
  for arm in N L P L N; do
    case $arm in
      N) start_atc "$ORIG"; echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null; n=3 ;;
      L) start_atc "$B/loose/MY_ivh_atc"; echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null; n=3 ;;
      P) echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null; n=2 ;;
    esac
    QUIET=1 /root/ivh_tools/wait_capacity_settled.sh | tee -a $OUT.log
    for r in $(seq 1 $n); do
      v=$(cd /root && timeout 120 ${CMD[$w]} 2>&1 | eval "${EXT[$w]}" | head -1); v=${v:-FAIL}
      log "$w arm $arm round $r value=$v"
      echo "$w,$arm,$r,$v" >> $OUT.csv
    done
    [ "$(dmesg | grep -ciE 'soft lockup|rcu.*stall|hung task')" != "$D0" ] && { log "EARLY EXIT: kernel warning"; exit 1; }
  done
done
