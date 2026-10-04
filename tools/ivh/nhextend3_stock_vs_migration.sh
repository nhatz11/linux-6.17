#!/bin/bash
# Stock (no migration) vs full IVH migration (universal_eligible=1, daemons
# on) for NHextend3 -n 16 (default loop_spin=600000, the CS length that
# historically showed the cleanest migration win, per
# ivh_state_of_the_art_2026-07-20.md §3.3). Adaptive spinning held fixed at
# spin_mode 1 (STOCK_PV) throughout so migration is the only thing varying.
set -u
ROUNDS="${1:-5}"

run_one() {
    local label="$1"
    OUT=$(/root/linux-6.17/NHextend3 -n 16 2>&1)
    RAN=$(grep -oP '^Ran for \K[0-9]+' <<< "$OUT")
    echo "$label  clock=$(date -u +%H:%M:%S)  ran_for=$RAN"
}

echo "=== NHextend3 stock vs migration, $ROUNDS rounds ==="
echo "start_clock=$(date -u +%H:%M:%S)"
for i in $(seq 1 "$ROUNDS"); do
    echo 0 > /proc/sys/kernel/ivh_universal_eligible || { echo "FATAL: sysctl write failed"; exit 1; }
    run_one "round $i  A(STOCK/no-migration)"
    echo 1 > /proc/sys/kernel/ivh_universal_eligible || { echo "FATAL: sysctl write failed"; exit 1; }
    run_one "round $i  B(IVH-migration)"
done
echo "=== done ==="
