#!/bin/bash
# Runs the accepted 4-benchmark suite (see tools/bpf/docs/ivh_rebuild_plan.md §3.1)
# and prints a summary table in the same format as §3.2/§3.3 of that doc, so a
# new baseline can be pasted straight in.
#
# Usage: ./run_baseline.sh [REPS]   (default REPS=4, matches the doc's baseline n)
set -u

REPS="${1:-4}"
TS="$(date +%Y%m%d_%H%M%S)"
LOG="/root/linux-6.17/cvm_setup/.state/baseline_${TS}.log"
mkdir -p /root/linux-6.17/cvm_setup/.state
: > "$LOG"

log() { echo "$@" | tee -a "$LOG"; }

# --- preempt mode: must be set explicitly, every time. Reading the debugfs
# file marks the ACTIVE mode with PARENTHESES, not brackets -- do not trust a
# bare `cat` here, always WRITE it. See ivh_rebuild_plan.md §3.1 for the 15%
# measurement error this caused once. ---
echo voluntary | sudo tee /sys/kernel/debug/sched/preempt > /dev/null
active=$(sudo cat /sys/kernel/debug/sched/preempt)
log "preempt mode (active in parens): $active"
case "$active" in *'(voluntary)'*) : ;; *) log "WARNING: voluntary did not take effect"; ;; esac

log "kernel: $(uname -r)"
log "cmdline: $(cat /proc/cmdline)"
# The corunner runs OUTSIDE this guest (host-side / sibling VM), so it is
# never visible to pgrep from in here -- checking for a local "sysbench"
# process is meaningless and was previously misreported as "corunner OFF"
# regardless of actual corunner state. %steal below is the only corunner
# signal actually observable from inside the guest.
log "corunner state: not observable from inside this guest -- check host-side"
steal=$(vmstat 1 2 2>/dev/null | tail -1 | awk '{print $(NF-1)}')
log "%steal (vmstat): $steal"
log ""

mean_range_spread() {
  # args: list of numbers -> prints "mean|min|max|spread%"
  python3 -c "
import sys
vals=[float(x) for x in sys.argv[1:]]
mean=sum(vals)/len(vals)
lo,hi=min(vals),max(vals)
spread=(hi-lo)/mean*100 if mean else 0
print(f'{mean:.2f}|{lo}|{hi}|{spread:.1f}')
" "$@"
}

declare -a NH_VALS HB_VALS DB_VALS E4_VALS

log "=== NHextend3 -n 16 (metric: 'Ran for N times') ==="
for i in $(seq "$REPS"); do
  out=$(/root/linux-6.17/NHextend3 -n 16 2>&1)
  v=$(echo "$out" | grep -oP '^Ran for \K[0-9]+')
  log "  rep$i: $v"
  NH_VALS+=("$v")
done

log "=== hackbench -T -g 1 -f 8 -l 400000 ==="
for i in $(seq "$REPS"); do
  v=$(hackbench -T -g 1 -f 8 -l 400000 2>&1 | grep '^Time:' | awk '{print $2}')
  log "  rep$i: ${v}s"
  HB_VALS+=("$v")
done

log "=== dbench -F -t 12 16 -D /root/dbench_test ==="
for i in $(seq "$REPS"); do
  v=$(dbench -F -t 12 16 -D /root/dbench_test 2>&1 | grep '^Throughput' | awk '{print $2}')
  log "  rep$i: ${v} MB/s"
  DB_VALS+=("$v")
done

log "=== ebizzy mmap 4MB (the clean metric) ==="
for i in $(seq "$REPS"); do
  v=$(/home/nick/Desktop/ebizzy -S 20 -t 16 -m -s 4194304 2>&1 | head -1 | awk '{print $1}')
  log "  rep$i: ${v} rec/s"
  E4_VALS+=("$v")
done

log ""
log "=== Summary (paste into ivh_rebuild_plan.md) ==="
log "| Benchmark | Mean | Range | n | Spread |"
log "|---|---|---|---|---|"
r=$(mean_range_spread "${NH_VALS[@]}"); IFS='|' read -r m lo hi sp <<< "$r"
log "| NHextend3 -n16 (Ran for N times) | $m | $lo - $hi | $REPS | ${sp}% |"
r=$(mean_range_spread "${HB_VALS[@]}"); IFS='|' read -r m lo hi sp <<< "$r"
log "| hackbench -T -g1 -f8 -l400000 | ${m}s | $lo - ${hi}s | $REPS | ${sp}% |"
r=$(mean_range_spread "${DB_VALS[@]}"); IFS='|' read -r m lo hi sp <<< "$r"
log "| dbench -t 12 16 | $m MB/s | $lo - $hi MB/s | $REPS | ${sp}% |"
r=$(mean_range_spread "${E4_VALS[@]}"); IFS='|' read -r m lo hi sp <<< "$r"
log "| ebizzy mmap 4MB (the clean metric) | $m rec/s | $lo - $hi | $REPS | ${sp}% |"

log ""
log "Raw per-rep values:"
log "- NHextend3: ${NH_VALS[*]}"
log "- hackbench: ${HB_VALS[*]}"
log "- dbench: ${DB_VALS[*]}"
log "- ebizzy-mmap-4MB: ${E4_VALS[*]}"

log ""
log "DONE. Full log: $LOG"
