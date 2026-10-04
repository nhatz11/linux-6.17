#!/bin/bash
# Interleaved PV vs IVH, capacity/migration OFF, HALVED hackbench workload
# (-l 200000 instead of 400000) to shorten runs under current host contention.
set -u
ROUNDS="${1:-5}"
WORKLOAD="hackbench -T -g 1 -f 8 -l 200000"

run_one() {
    local label="$1"
    local ts_start=$(date +%s.%N)
    OUT=$($WORKLOAD 2>&1)
    local ts_end=$(date +%s.%N)
    HB=$(grep -oP 'Time:\s*\K[0-9.]+' <<< "$OUT")
    WALL=$(echo "$ts_end - $ts_start" | bc)
    echo "$label  clock=$(date -u +%H:%M:%S)  hackbench_time=${HB}s  wall=${WALL}s"
}

echo "=== PV vs IVH, $ROUNDS rounds each, HALF workload (-l 200000), capacity/migration OFF ==="
echo "start_clock=$(date -u +%H:%M:%S)"
for i in $(seq 1 "$ROUNDS"); do
    /root/spin_mode 1 >/dev/null
    run_one "round $i  A(STOCK_PV)"
    /root/spin_mode 2 >/dev/null
    run_one "round $i  B(IVH_PV)"
done
echo "=== done ==="
