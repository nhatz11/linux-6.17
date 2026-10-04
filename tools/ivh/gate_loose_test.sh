#!/bin/bash
# Is MY_ivh_atc's destination capacity gate too strict under full contention?
# Arms (5 consecutive IVH+AS rounds each, capacity-settled wait before each arm):
#   N = normal gate  (IVH_CAP_HARDFLOOR 700, IVH_CAP_TOPBAND 50)  -- scratch rebuild of the running source
#   L = loose gate   (IVH_CAP_HARDFLOOR 500, IVH_CAP_TOPBAND 250)
# Order N L L N, then a PV control arm. Early exit if L looks bad.
set -u
ROUNDS=${ROUNDS:-5}; S=/proc/sys/kernel; W="hackbench -T -g1 -f8 -l400000"
B=/tmp/claude-0/-root-linux-6-17/b98a4d93-d606-4bb7-bd13-7031a5eea896/scratchpad/atc_build
ORIG=/root/kernels/linux-6.17-vanilla/tools/bpf/MY_ivh_atc
OUT=/root/ivh_tools/gate_loose_$(date +%H%M%S); PV_REF=52.97
log() { echo "$*" | tee -a $OUT.log; }

start_atc() {   # $1 = binary path
    echo 0 > $S/ivh_universal_eligible
    pkill -9 -x MY_ivh_atc; for i in $(seq 50); do pgrep -x MY_ivh_atc >/dev/null || break; sleep 0.2; done
    setsid nohup "$1" > /root/ivh_logs/atc.log 2>&1 < /dev/null &
    for i in $(seq 40); do bpftool map lookup name ivh_cfg key 0 0 0 0 >/dev/null 2>&1 && break; sleep 0.25; done
    bpftool map update name ivh_cfg key 0 0 0 0 value "$(cat $S/ivh_cap_source)" 0 0 0 || { log "FATAL: ivh_cfg update failed"; exit 1; }
    [ "$(pgrep -xc MY_ivh_atc)" = 1 ] || { log "FATAL: MY_ivh_atc count=$(pgrep -xc MY_ivh_atc)"; exit 1; }
}
restore() {
    log "--- restoring original MY_ivh_atc and IVH+AS ---"
    start_atc "$ORIG"; echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null
    log "restored: atc=$(pgrep -xc MY_ivh_atc) eligible=$(cat $S/ivh_universal_eligible) adaptive_mode=$(cat $S/ivh_adaptive_mode)"
}
trap restore EXIT
dmesg -n 1; D0=$(dmesg | grep -ciE 'soft lockup|rcu.*stall|hung task')

run_arm() {   # $1 = label N|L|P
    local lab=$1
    case $lab in
      N) start_atc $B/normal/MY_ivh_atc; echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null ;;
      L) start_atc $B/loose/MY_ivh_atc;  echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null ;;
      P) echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null ;;
    esac
    QUIET=1 /root/ivh_tools/wait_capacity_settled.sh | tee -a $OUT.log
    python3 /root/ivh_tools/drift_snap.py "$lab:start" >> $OUT.snaps.jsonl
    ARM_T=()
    for r in $(seq 1 $ROUNDS); do
        v=$(timeout 150 $W 2>&1 | grep -oP '^Time:\s*\K[0-9.]+'); v=${v:-TIMEOUT}
        python3 /root/ivh_tools/drift_snap.py "$lab:r$r:$v" >> $OUT.snaps.jsonl
        log "arm $lab round $r time=${v}s"
        [ "$v" = TIMEOUT ] && { log "EARLY EXIT: round timed out (>150s) in arm $lab"; exit 1; }
        if [ "$(dmesg | grep -ciE 'soft lockup|rcu.*stall|hung task')" != "$D0" ]; then
            log "EARLY EXIT: kernel lockup/stall warning appeared in arm $lab"; exit 1; fi
        ARM_T+=("$v")
    done
}
median() { printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END{print (NR%2)?a[(NR+1)/2]:(a[NR/2]+a[NR/2+1])/2}'; }

run_arm N; N1=$(median "${ARM_T[@]}"); log "N1 median=${N1}s"
run_arm L; L1=$(median "${ARM_T[@]}"); log "L1 median=${L1}s"
if awk -v l=$L1 -v n=$N1 -v p=$PV_REF 'BEGIN{exit !(l > 1.15*n || l > p)}'; then
    log "EARLY EXIT: loose gate bad (L1 median $L1 vs N1 $N1, PV ref $PV_REF)"; exit 0; fi
run_arm L; L2=$(median "${ARM_T[@]}"); log "L2 median=${L2}s"
if awk -v l=$L2 -v n=$N1 -v p=$PV_REF 'BEGIN{exit !(l > 1.15*n || l > p)}'; then
    log "EARLY EXIT: loose gate bad on repeat (L2 median $L2 vs N1 $N1)"; exit 0; fi
run_arm N; N2=$(median "${ARM_T[@]}"); log "N2 median=${N2}s"
run_arm P; P1=$(median "${ARM_T[@]}"); log "P median=${P1}s"
log "SUMMARY N1=$N1 L1=$L1 L2=$L2 N2=$N2 PV=$P1"
