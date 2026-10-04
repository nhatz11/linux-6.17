#!/bin/bash
# Re-measure PV lock-acquisition rate for workloads whose point-15 number was
# taken with the WRONG invocation (see eval_final.md Appendix A). Now uses the
# campaign registry verbatim, and discards a warmup run per workload -- the
# omission that produced the fsmark 86,939/s and swaptions 685/s artifacts.
set -u
S=/proc/sys/kernel
source /root/ivh_tools/ivh_benchmarks.sh
R="python3 /root/ivh_tools/read_ivh_counters.py"
HS(){ $R ivh_cs_prev_hold_hist 2>/dev/null | grep -oE 'sum=[0-9]+' | cut -d= -f2; }
armcs(){ for k in ivh_cs_track_enabled ivh_cs_owner_enable ivh_cs_owner_clear ivh_cs_head_probe; do echo 1 > $S/$k; done; }
echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1
[ "$(cat $S/ivh_adaptive_mode)" = 0 ] || { echo "FATAL: PV arm did not take"; exit 1; }
armcs; echo 0 > $S/ivh_tks_sampler_ns
[ "$(cat $S/ivh_cs_owner_enable)" = 1 ] || { echo "FATAL: CS stamping disarmed"; exit 1; }

TARGETS="${TARGETS:-perf_sched_pipe sysbench_mutex dbench_16 wis_mmap2 schbench fsmark_tmpfs}"
printf "%-20s %8s %13s %11s  %s\n" workload "PV sec" "contended/s" "holds/s" "vs 5s floor"
for want in $TARGETS; do
  CMD=""; DIR="/root"
  for e in "${IVH_WORKLOADS[@]}"; do
    IFS='|' read -r n dir dr c x rec <<< "$e"
    [ "$n" = "$want" ] && { CMD="$c"; DIR="$dir"; break; }
  done
  [ -z "$CMD" ] && { echo "  !! $want not in registry"; continue; }
  ( cd "$DIR" && eval "$CMD" ) >/dev/null 2>&1          # warmup, discarded
  for r in 1 2 3; do
    sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
    h0=$(HS); t0=$(date +%s.%N)
    c=$(cd "$DIR" && perf stat -a -e lock:contention_begin -x, -- \
         bash -c "$CMD" 2>&1 >/dev/null | awk -F, '/contention_begin/{print $1}')
    t1=$(date +%s.%N); h1=$(HS)
    python3 - "$want" "$t0" "$t1" "${c:-0}" "$h0" "$h1" <<'PY'
import sys
n,t0,t1,c,h0,h1=sys.argv[1:7]
d=float(t1)-float(t0); cs=float(c)/d; hs=(int(h1)-int(h0))/d
flag="OK" if d>=5 else f"*** {d:.2f}s < 5s ***"
print(f"{n:20}{d:8.2f}{cs:13,.0f}{hs:11,.0f}  {flag}")
PY
  done
done
/root/spin_mode 2 >/dev/null 2>&1; echo 1 > $S/ivh_universal_eligible; echo 2 > $S/ivh_preempt_event_source
echo RECHECK-DONE
