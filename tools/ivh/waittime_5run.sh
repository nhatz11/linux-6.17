#!/bin/bash
set -u
echo 1 > /proc/sys/kernel/ivh_slowpath_wait_measure
WORKLOAD="hackbench -T -g 1 -f 8 -l 400000"
COUNTER_PY=/root/ivh_tools/read_ivh_counters.py

get_ns() { python3 "$COUNTER_PY" ivh_slowpath_wait_ns | grep -oP '=\s*\K[0-9]+'; }
get_ev() { python3 "$COUNTER_PY" ivh_slowpath_wait_events | grep -oP '=\s*\K[0-9]+'; }

for i in 1 2 3 4 5; do
    NS_B=$(get_ns); EV_B=$(get_ev)
    T0=$(date +%s.%N)
    OUT=$($WORKLOAD 2>&1)
    T1=$(date +%s.%N)
    NS_A=$(get_ns); EV_A=$(get_ev)

    HB_TIME=$(grep -oP 'Time:\s*\K[0-9.]+' <<< "$OUT")
    WALL=$(echo "$T1 - $T0" | bc)
    NS_D=$((NS_A - NS_B))
    EV_D=$((EV_A - EV_B))
    AVG=$(awk -v n="$NS_D" -v e="$EV_D" 'BEGIN{if(e>0) printf "%.0f", n/e; else print "n/a"}')
    echo "run $i: hackbench_time=${HB_TIME}s wall=${WALL}s | wait_ns_delta=$NS_D events_delta=$EV_D avg_wait_ns=$AVG"
done
