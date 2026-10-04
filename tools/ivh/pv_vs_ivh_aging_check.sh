#!/bin/bash
# Interleaved PV vs IVH adaptive-spinning test, capacity/migration OFF,
# with wall-clock timestamps on each run to check whether STOCK_PV drifts
# upward over calendar time while IVH_PV stays flat ("VM aging" theory).
set -u
ROUNDS="${1:-6}"
WORKLOAD="hackbench -T -g 1 -f 8 -l 400000"

run_one() {
    local label="$1"
    local ts_start=$(date +%s.%N)
    OUT=$($WORKLOAD 2>&1)
    local ts_end=$(date +%s.%N)
    HB=$(grep -oP 'Time:\s*\K[0-9.]+' <<< "$OUT")
    WALL=$(echo "$ts_end - $ts_start" | bc)
    echo "$label  clock=$(date -u +%H:%M:%S)  hackbench_time=${HB}s  wall=${WALL}s"
}

echo "=== PV vs IVH aging check, $ROUNDS rounds each, capacity/migration OFF ==="
echo "start_clock=$(date -u +%H:%M:%S)"
cat /proc/sys/kernel/ivh_universal_eligible /proc/sys/kernel/ivh_cap_source /proc/sys/kernel/ivh_uc_enabled
for i in $(seq 1 "$ROUNDS"); do
    /root/spin_mode 1 >/dev/null
    run_one "round $i  A(STOCK_PV)"
    /root/spin_mode 2 >/dev/null
    run_one "round $i  B(IVH_PV)"
done
echo "=== done ==="
