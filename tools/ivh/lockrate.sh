#!/bin/bash
# lockrate.sh <label> <command...>  -- count every _raw_spin_lock*/rwlock call
# during a workload with the ftrace function profiler. No PMU needed.
set -u
T=/sys/kernel/debug/tracing
LBL=$1; shift
echo 0 > $T/function_profile_enabled
echo > $T/set_ftrace_filter
for f in _raw_spin_lock _raw_spin_lock_irqsave _raw_spin_lock_irq _raw_spin_lock_bh \
         _raw_spin_lock_nested _raw_spin_lock_irqsave_nested _raw_spin_lock_nest_lock \
         _raw_spin_trylock _raw_spin_trylock_bh \
         _raw_read_lock _raw_read_lock_irqsave _raw_read_lock_irq _raw_read_lock_bh \
         _raw_write_lock _raw_write_lock_irqsave _raw_write_lock_irq _raw_write_lock_bh \
         queued_spin_lock_slowpath __pv_queued_spin_lock_slowpath; do
    echo "$f" >> $T/set_ftrace_filter 2>/dev/null
done
echo 1 > $T/function_profile_enabled
S=$(date +%s.%N)
"$@" > /dev/null 2>&1
E=$(date +%s.%N)
echo 0 > $T/function_profile_enabled
DUR=$(echo "$E - $S" | bc)
echo "### $LBL   wall ${DUR}s"
cat $T/trace_stat/function* 2>/dev/null | awk -v d="$DUR" '
  /^  Function/ || /^  ---/ {next}
  NF>=2 && $2+0>0 {cnt[$1]+=$2}
  END{t=0; for(f in cnt) t+=cnt[f];
      for(f in cnt) printf "  %-34s %14d  %10.0f/s\n", f, cnt[f], cnt[f]/d;
      printf "  %-34s %14d  %10.0f/s\n","TOTAL",t,t/d}' | sort -k2 -rn
echo 0 > $T/function_profile_enabled; echo > $T/set_ftrace_filter
