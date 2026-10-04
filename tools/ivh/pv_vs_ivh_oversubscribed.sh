#!/bin/bash
# Quick oversubscribed PV vs IVH test: -g 4 -f 8 gives ~64 threads on 16
# vCPUs, so reclaimed spin cycles (tier 2's payoff) have guest-side work to
# go to, unlike the exactly-saturated -g 1 case where wall clock is blind
# to the mechanism. capacity/migration OFF.
set -u
ROUNDS="${1:-3}"
WORKLOAD="hackbench -T -g 4 -f 8 -l 200000"

run_one() {
    local label="$1"
    local ts_start=$(date +%s.%N)
    OUT=$($WORKLOAD 2>&1)
    local ts_end=$(date +%s.%N)
    HB=$(grep -oP 'Time:\s*\K[0-9.]+' <<< "$OUT")
    WALL=$(echo "$ts_end - $ts_start" | bc)
    echo "$label  clock=$(date -u +%H:%M:%S)  hackbench_time=${HB}s  wall=${WALL}s"
}

echo "=== PV vs IVH oversubscribed (-g 4 -f 8 -l 50000, ~64 threads/16 vCPUs), $ROUNDS rounds ==="
echo "start_clock=$(date -u +%H:%M:%S)"
for i in $(seq 1 "$ROUNDS"); do
    /root/spin_mode 1 >/dev/null || { echo "FATAL: spin_mode 1 failed"; exit 1; }
    run_one "round $i  A(STOCK_PV)"
    /root/spin_mode 2 >/dev/null || { echo "FATAL: spin_mode 2 failed"; exit 1; }
    run_one "round $i  B(IVH_PV)"
done
echo "=== done ==="
