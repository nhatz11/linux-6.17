#!/bin/bash
# Block until the in-kernel capacity estimate (rq->ivh_uc_capacity, the signal
# the migration gate uses at ivh_cap_source=3) has recovered from the previous
# load. Rule: at least MIN_S seconds of idle, then the 16-CPU mean must hold
# steady (range of the last 4 samples, 15 s, <= BAND points). Timeout MAX_S.
# See tools/bpf/docs/ivh_sustained_load_drift_and_capacity_test_2026-09-15.md.
set -u
MIN_S=${MIN_S:-60}; MAX_S=${MAX_S:-300}; BAND=${BAND:-15}; QUIET=${QUIET:-0}
start=$(date +%s); hist=()
while :; do
    m=$(python3 /root/ivh_tools/read_vact_rq.py ivh_uc_capacity | grep -oP 'sum=\s*\K[0-9]+')
    m=$(( m / $(nproc) )); hist+=("$m"); el=$(( $(date +%s) - start ))
    [ "$QUIET" = 1 ] || echo "  wait: +${el}s capacity_mean=$m"
    n=${#hist[@]}
    if [ "$el" -ge "$MIN_S" ] && [ "$n" -ge 4 ]; then
        last=("${hist[@]:n-4:4}"); lo=${last[0]}; hi=${last[0]}
        for v in "${last[@]}"; do (( v<lo )) && lo=$v; (( v>hi )) && hi=$v; done
        if (( hi - lo <= BAND )); then echo "settled after ${el}s at capacity_mean=$m"; exit 0; fi
    fi
    [ "$el" -ge "$MAX_S" ] && { echo "TIMEOUT after ${el}s at capacity_mean=$m (not settled)"; exit 1; }
    sleep 5
done
