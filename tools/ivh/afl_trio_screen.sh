#!/bin/bash
# Fast corner screen: 2 values each for (ivh_capacity_threshold,
# ivh_time_left_threshold_ns, IVH_AFL_SPINS) = 8 trios, evaluated against a
# one-time-measured PV baseline at 5 loop_spin values (the 3 worst "valley"
# points from the 2026-09-11 sweep, plus the two known-good endpoints as a
# sanity check that a trio doesn't break what already works).
set -u
DUR=10
LOOP_SPINS=(600000 100000 50000 25000 5000)
CAPS=(950 1020)
TLEFTS=(50000 4000000)
SPINS=(64 1024)

cd /root/linux-6.17

echo "=== measuring PV baseline (migration off) at each loop_spin ==="
echo 0 > /proc/sys/kernel/ivh_universal_eligible
declare -A PV
for ls in "${LOOP_SPINS[@]}"; do
    v=$(NHEXTEND_DURATION=$DUR NHEXTEND_LOOP_SPIN=$ls ./NHextend3 -n 2>&1 | grep -oP '^Ran for \K[0-9]+')
    PV[$ls]=$v
    echo "  loop_spin=$ls  PV=$v"
done
echo 1 > /proc/sys/kernel/ivh_universal_eligible

echo "=== corner screen: 8 trios x 5 loop_spin values ==="
for cap in "${CAPS[@]}"; do
    echo "$cap" > /proc/sys/kernel/ivh_capacity_threshold
    for tleft in "${TLEFTS[@]}"; do
        echo "$tleft" > /proc/sys/kernel/ivh_time_left_threshold_ns
        for spins in "${SPINS[@]}"; do
            echo "--- trio: cap=$cap tleft=$tleft spins=$spins ---"
            for ls in "${LOOP_SPINS[@]}"; do
                v=$(NHEXTEND_DURATION=$DUR NHEXTEND_LOOP_SPIN=$ls IVH_AFL_SPINS=$spins \
                    ./NHextend-full -n 2>&1 | grep -oP '^Ran for \K[0-9]+')
                pct=$(python3 -c "print(f'{($v-${PV[$ls]})/${PV[$ls]}*100:+.1f}')")
                echo "    loop_spin=$ls  ivh_as=$v  vs_PV=${pct}%"
            done
        done
    done
done
echo "=== screen done ==="
