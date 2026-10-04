#!/bin/bash
set -u
echo 0 > /proc/sys/kernel/ivh_slowpath_wait_measure
WORKLOAD="hackbench -T -g 1 -f 8 -l 400000"

run_block() {
    local label="$1" mode="$2" n="$3"
    echo "=== $label block (spin_mode $mode) ==="
    /root/spin_mode "$mode" >/dev/null
    for i in $(seq 1 "$n"); do
        T0=$(date +%s.%N)
        OUT=$($WORKLOAD 2>&1)
        T1=$(date +%s.%N)
        HB=$(grep -oP 'Time:\s*\K[0-9.]+' <<< "$OUT")
        WALL=$(echo "$T1 - $T0" | bc)
        echo "  run $i: hackbench_time=${HB}s wall=${WALL}s"
    done
}

run_block "STOCK_PV" 1 5
echo
run_block "IVH_PV" 2 5
