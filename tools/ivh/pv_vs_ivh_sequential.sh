#!/bin/bash
# Sequential-block PV vs IVH adaptive-spinning test, capacity/migration OFF:
# all N STOCK_PV runs first, then all N IVH_PV runs (as opposed to interleaved).
set -u
N="${1:-6}"
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

echo "=== PV vs IVH sequential blocks, $N runs each, capacity/migration OFF ==="
echo "start_clock=$(date -u +%H:%M:%S)"
cat /proc/sys/kernel/ivh_universal_eligible /proc/sys/kernel/ivh_cap_source

/root/spin_mode 1 >/dev/null
echo "--- STOCK_PV block ---"
for i in $(seq 1 "$N"); do
    run_one "STOCK_PV run $i"
done

/root/spin_mode 2 >/dev/null
echo "--- IVH_PV block ---"
for i in $(seq 1 "$N"); do
    run_one "IVH_PV run $i"
done

echo "=== done ==="
