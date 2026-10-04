#!/bin/bash
# RUN THIS ON THE HYPERVISOR HOST, not in the guest. Read-only.
#
# Host-side ground truth for per-vCPU steal and active time.
#   /proc/<tid>/schedstat = "<run_ns> <wait_ns> <timeslices>"
#     run_ns  : time this vCPU thread was ON a physical CPU  -> ACTIVE
#     wait_ns : time it was RUNNABLE but not scheduled       -> STEAL
#   (wall - run - wait) is the thread blocked/halted, i.e. guest idle.
#
# usage: ./host_truth.sh <seconds> [qemu-pid]
#   with no pid it lists the candidates and exits.
set -u
SECS=${1:-30}
PID=${2:-}

list(){ ps -eo pid,etime,args | grep -E "[q]emu-system|[q]emu-kvm" | head -20; }

if [ -z "$PID" ]; then
    echo "QEMU processes on this host -- pick the one for the GUEST UNDER TEST:"
    list
    echo
    echo "then: $0 $SECS <pid>"
    exit 0
fi

mapfile -t TIDS < <(ps -L -o tid=,comm= -p "$PID" 2>/dev/null \
                    | awk '$2 ~ /^CPU/ {print $1}')
if [ "${#TIDS[@]}" -eq 0 ]; then
    echo "no vCPU threads found for pid $PID (expected comm like 'CPU 0/KVM')" >&2
    ps -L -o tid=,comm= -p "$PID" | head; exit 1
fi
echo "pid=$PID vcpu_threads=${#TIDS[@]} window=${SECS}s"
echo "start_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"

declare -A R0 W0
T0=$(date +%s%N)
for t in "${TIDS[@]}"; do
    read -r r w _ < /proc/"$t"/schedstat 2>/dev/null || continue
    R0[$t]=$r; W0[$t]=$w
done

sleep "$SECS"

T1=$(date +%s%N); WALL=$((T1-T0))
echo "end_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)  wall_ns=$WALL"
echo
printf "%-6s %-10s %14s %14s %9s %9s %9s\n" \
       vcpu tid run_ns wait_ns active% steal% idle%
i=0
for t in "${TIDS[@]}"; do
    read -r r w _ < /proc/"$t"/schedstat 2>/dev/null || continue
    dr=$((r-${R0[$t]})); dw=$((w-${W0[$t]}))
    awk -v i=$i -v t=$t -v dr=$dr -v dw=$dw -v wall=$WALL 'BEGIN{
        a=dr*100/wall; s=dw*100/wall; idle=100-a-s; if(idle<0) idle=0;
        printf "%-6s %-10s %14d %14d %8.1f%% %8.1f%% %8.1f%%\n", i, t, dr, dw, a, s, idle}'
    i=$((i+1))
done
