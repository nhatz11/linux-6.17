#!/bin/bash
# Interleaved A/B: STOCK_PV (no migration) vs IVH_PV+migration, alternating,
# to control for host-state drift given the huge effect size already seen.
set -u
ROUNDS="${1:-6}"
WORKLOAD="hackbench -T -g 1 -f 8 -l 400000"

set_stock() {
    /root/spin_mode 1 >/dev/null
    echo 0 > /proc/sys/kernel/ivh_universal_eligible
}
set_ivh_migration() {
    /root/spin_mode 2 >/dev/null
    echo 1 > /proc/sys/kernel/ivh_universal_eligible
}

run_one() {
    local label="$1"
    T0=$(date +%s.%N)
    OUT=$($WORKLOAD 2>&1)
    T1=$(date +%s.%N)
    HB=$(grep -oP 'Time:\s*\K[0-9.]+' <<< "$OUT")
    WALL=$(echo "$T1 - $T0" | bc)
    echo "$label: hackbench_time=${HB}s wall=${WALL}s"
}

echo "=== Interleaved A/B, $ROUNDS rounds each (A=STOCK_PV no-migration, B=IVH_PV+migration) ==="
for i in $(seq 1 "$ROUNDS"); do
    set_stock
    run_one "round $i  A(STOCK_PV)"
    set_ivh_migration
    run_one "round $i  B(IVH+MIG)"
done
echo "=== done ==="
