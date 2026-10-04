#!/bin/bash
# G-LOCK-50 / G1 -- measure Gate 2's input distribution UNDER LOAD.
#
# The whole point of G1: it either confirms or kills the premise behind the
# Gate-2 EWMA work for the cost of one reboot. Read the verdict at the end.
#
# Run AFTER g50_postboot.sh has completed (including the capacity settling
# wait -- an unsettled capacity changes which CPUs Gate 1 lets through, and
# therefore which population Gate 2 is consulted on).
set -u
OUT=${OUT:-/root/ivh_logs/g1_$(date +%m%d-%H%M%S)}
mkdir -p "$OUT"
S=/proc/sys/kernel
SNAP=/root/ivh_tools/g1_snap.py

command -v hackbench >/dev/null || { echo "FATAL: hackbench not on PATH"; exit 1; }
command -v dbench    >/dev/null || { echo "FATAL: dbench not on PATH"; exit 1; }
[ -d /root/dbench_test ] || { echo "FATAL: /root/dbench_test missing"; exit 1; }

echo "kernel        : $(uname -r)"
for k in ivh_cap_source ivh_capacity_threshold ivh_time_left_source \
         ivh_time_left_threshold_ns ivh_preempt_event_source ivh_vact_jump_ns \
         ivh_eval_cooldown_ns ivh_rcu_guard; do
    printf "%-28s %s\n" "$k" "$(cat $S/$k 2>/dev/null || echo MISSING)"
done | tee "$OUT/sysctls.txt"

# Assert the new symbols exist before spending 10 minutes on a workload.
# g1_snap.py exits 2 only when a REQUIRED G-LOCK-50 counter is absent.
# An optional counter being absent is a note on stderr, not an abort.
if ! python3 "$SNAP" "$OUT/pre.json" 2>"$OUT/snap_err.txt"; then
    cat "$OUT/snap_err.txt"
    echo "*** wrong kernel or unreadable counters. ABORT. ***"
    exit 1
fi
[ -s "$OUT/snap_err.txt" ] && cat "$OUT/snap_err.txt"
echo "baseline snapshot taken"

echo "--- hackbench (pipe, threads) x3 ---"
for i in 1 2 3; do
    ( cd /root && timeout 120 hackbench -T -g1 -f8 -l150000 ) 2>&1 | tee -a "$OUT/hackbench.txt"
done
echo "--- dbench 16 x3 ---"
for i in 1 2 3; do
    ( cd /root && timeout 120 dbench -F -t 15 16 -D /root/dbench_test ) 2>&1 \
        | grep -E "Throughput" | tee -a "$OUT/dbench.txt"
done

python3 "$SNAP" "$OUT/post.json"
echo
echo "================ G1 VERDICT ================"
python3 "$SNAP" --diff "$OUT/pre.json" "$OUT/post.json" | tee "$OUT/verdict.txt"
echo
echo "artifacts: $OUT"
