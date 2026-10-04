#!/bin/bash
# Black-box migration screen: for each workload, interleave
#   A = migration OFF (ivh_universal_eligible=0)
#   B = migration ON  (ivh_universal_eligible=1)
# Everything else (adaptive spinning, tiers, thresholds) is held CONSTANT,
# so the only variable is the migration engine.
#
# Usage: mig_screen.sh [rounds] [workload-filter-regex]
set -u
ROUNDS="${1:-3}"
FILTER="${2:-.}"
OUT="/root/ivh_tools/screen/results_$(date +%Y%m%d_%H%M%S).csv"
LOG="${OUT%.csv}.log"

mig() { echo "$1" > /proc/sys/kernel/ivh_universal_eligible; }
ORIG_MIG=$(cat /proc/sys/kernel/ivh_universal_eligible)
trap 'mig "$ORIG_MIG"; echo "restored ivh_universal_eligible=$ORIG_MIG"' EXIT

# name | direction(hi=higher-better, lo=lower-better) | command | extractor
WORKLOADS=(
"hackbench_pipe_thr|lo|hackbench -T -g 8 -f 20 -l 2000|grep -oP 'Time:\s*\K[0-9.]+'"
"hackbench_sock_thr|lo|hackbench -T -s 512 -g 8 -f 20 -l 2000|grep -oP 'Time:\s*\K[0-9.]+'"
"hackbench_pipe_proc|lo|hackbench -p -g 8 -f 20 -l 2000|grep -oP 'Time:\s*\K[0-9.]+'"
"perf_sched_messaging|lo|perf bench sched messaging -g 20 -l 1000|grep -oP 'Total time:\s*\K[0-9.]+'"
"perf_sched_pipe|hi|perf bench sched pipe -l 400000|grep -oP '^\s*\K[0-9]+(?= ops/sec)'"
"perf_futex_hash|hi|perf bench futex hash -t 16 -r 5|grep -oP 'Averaged\s+\K[0-9]+'"
"perf_futex_wake|lo|perf bench futex wake -t 16 -r 10|grep -oP 'Averaged\s+\K[0-9.]+'"
"perf_futex_wake_par|lo|perf bench futex wake-parallel -t 16 -r 10|grep -oP 'Averaged\s+\K[0-9.]+'"
"perf_futex_requeue|lo|perf bench futex requeue -t 16 -r 10|grep -oP 'Averaged\s+\K[0-9.]+'"
"perf_futex_lockpi|hi|perf bench futex lock-pi -t 16 -r 5|grep -oP 'Averaged\s+\K[0-9]+'"
"perf_epoll_wait|hi|perf bench epoll wait -t 16 -r 5|grep -oP 'Averaged\s+\K[0-9]+'"
"perf_syscall_basic|hi|perf bench syscall basic -l 2000000|grep -oP '\K[0-9]+(?= ops/sec)'"
"sysbench_threads|hi|sysbench threads --threads=32 --thread-locks=4 --time=20 run|grep -oP 'total number of events:\s*\K[0-9]+'"
"sysbench_mutex|lo|sysbench mutex --threads=32 --mutex-num=16 --mutex-locks=20000 run|grep -oP 'total time:\s*\K[0-9.]+'"
"sysbench_memory|hi|sysbench memory --threads=16 --time=20 run|grep -oP 'total number of events:\s*\K[0-9]+'"
"dbench_16|hi|dbench -t 30 -D /root/dbench_test 16|grep -oP 'Throughput\s+\K[0-9.]+'"
)

echo "workload,round,arm,value" > "$OUT"
echo "=== migration screen: $ROUNDS rounds, filter='$FILTER' -> $OUT ===" | tee -a "$LOG"

for w in "${WORKLOADS[@]}"; do
    IFS='|' read -r NAME DIR CMD EXT <<< "$w"
    [[ "$NAME" =~ $FILTER ]] || continue
    # sanity: does it run and does the extractor fire?
    mig 1
    probe=$(eval "$CMD" 2>&1 | eval "$EXT" | head -1)
    if [[ -z "$probe" ]]; then
        echo "SKIP $NAME (no metric extracted)" | tee -a "$LOG"; continue
    fi
    for r in $(seq 1 "$ROUNDS"); do
        for arm in A B; do
            [[ $arm == A ]] && mig 0 || mig 1
            v=$(eval "$CMD" 2>&1 | eval "$EXT" | head -1)
            echo "$NAME,$r,$arm,${v:-NA}" >> "$OUT"
            echo "$NAME r$r $arm=${v:-NA}" >> "$LOG"
        done
    done
    echo "done $NAME" | tee -a "$LOG"
done
echo "=== screen complete: $OUT ===" | tee -a "$LOG"
