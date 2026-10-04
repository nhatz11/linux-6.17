#!/bin/bash
# acq_suite.sh -- SYSTEM-WIDE kernel lock-acquisition rate, per workload.
#
# WHAT IS DIFFERENT FROM THE POINT-15 NUMBERS. Nothing about the instrument
# (ftrace function profiler on the _raw_spin_lock* family) -- that was already
# system-wide. What changes is COVERAGE and BOOKKEEPING:
#   (a) the suite now includes psearchy, tinyconfig, parsec_canneal,
#       parsec_dedup and memtier/memcached, none of which point 15 measured;
#   (b) PARSEC is invoked DIRECTLY (parsecmgmt itself takes ~600k locks/s and
#       swamped the application -- see acq_parsec.sh);
#   (c) an IDLE FLOOR is measured before, during and after the suite, so the
#       part of the count that belongs to background daemons and vcap_probe
#       can be subtracted instead of being attributed to the workload;
#   (d) the per-function breakdown of every run is kept in a sidecar file.
#
# WHY THE PROFILER AND NOT A PER-PROCESS COUNTER. The ftrace function profiler
# has no pid filter: it counts an entry to _raw_spin_lock on any CPU by any
# task -- workload threads, threads the workload forks, kernel threads woken on
# its behalf (kworker, jbd2, ksoftirqd), and SEPARATE SERVER PROCESSES. The
# memcached case is the clean demonstration: memtier_benchmark is the thing you
# invoke, but the lock traffic is in the 16 memcached server threads, in a
# different process entirely.
#
# OVERHEAD, STATED PLAINLY. The profiler costs time on every traced call, and
# _raw_spin_lock is called millions of times a second. Fixed-DURATION workloads
# therefore do less work while traced and fixed-WORK workloads take longer.
# The COUNTS are exact; the throughput of a traced run is NOT comparable to an
# untraced one. This harness measures lock rate, not performance.
set -u
T=/sys/kernel/debug/tracing
TOOLS=/root/ivh_tools
P=/root/parsec-benchmark
REPS="${REPS:-3}"
MAXREPS="${MAXREPS:-7}"
SPREAD="${SPREAD:-1.15}"      # max/min of acq/s that counts as "clean"
STAMP=$(date +%m%d-%H%M%S)
OUT="${OUT:-$TOOLS/acqsuite_$STAMP.csv}"
FNOUT="${OUT%.csv}_fn.txt"

FNS="_raw_spin_lock _raw_spin_lock_irqsave _raw_spin_lock_irq _raw_spin_lock_bh
     _raw_spin_lock_nested _raw_spin_lock_irqsave_nested _raw_spin_lock_nest_lock
     _raw_spin_trylock _raw_spin_trylock_bh
     _raw_read_lock _raw_read_lock_irqsave _raw_read_lock_irq _raw_read_lock_bh
     _raw_write_lock _raw_write_lock_irqsave _raw_write_lock_irq _raw_write_lock_bh
     queued_spin_lock_slowpath __pv_queued_spin_lock_slowpath"

# ---------------------------------------------------------------- profiler ---
prof_start(){
  echo 0 > $T/function_profile_enabled
  echo > $T/set_ftrace_filter
  for f in $FNS; do echo "$f" >> $T/set_ftrace_filter 2>/dev/null; done
  echo 1 > $T/function_profile_enabled      # enabling also zeroes the stats
}
prof_stop(){ echo 0 > $T/function_profile_enabled; }
prof_dump(){ cat $T/trace_stat/function* 2>/dev/null | awk '
    /^  Function/ || /^  ---/ {next}
    NF>=2 && $2+0>0 {c[$1]+=$2}
    END{for(f in c) print f, c[f]}'; }

# ------------------------------------------------------------- the registry ---
# name | dir | prep (runs OUTSIDE the traced window) | command (traced)
spec(){
case "$1" in
  sysbench_mutex_long) D=/root; PREP=":"; C="sysbench mutex --threads=16 --mutex-num=16 --mutex-locks=2000000 run";;
 fsmark_long) D=/root; PREP="rm -rf /dev/shm/fsmark; mkdir -p /dev/shm/fsmark"; C="for i in \$(seq 1 100); do rm -rf /dev/shm/fsmark; mkdir -p /dev/shm/fsmark; fs_mark -d /dev/shm/fsmark -D 16 -s 4096 -n 2000 -t 16 -L 1; done";;
 idle_floor)          D=/root; PREP=":";                          C="sleep 15";;
 stressng_dentry)     D=/root; PREP=":";                          C="stress-ng --dentry 16 -t 15s --metrics-brief";;
 perf_epoll_wait)     D=/root; PREP=":";                          C="perf bench epoll wait -t 16 -r 15";;
 hackbench_pipe_thr)  D=/root; PREP=":";                          C="hackbench -T -g1 -f8 -l150000";;
 sysbench_mutex)      D=/root; PREP=":";                          C="sysbench mutex --threads=16 --mutex-num=16 --mutex-locks=40000 run";;
 ebizzy_mmap)         D=/root; PREP=":";                          C="/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304";;
 dbench_16)           D=/root; PREP="mkdir -p /root/dbench_test";  C="dbench -F -t 15 16 -D /root/dbench_test";;
 fsmark_tmpfs)        D=/root; PREP="rm -rf /dev/shm/fsmark; mkdir -p /dev/shm/fsmark"; C="fs_mark -d /dev/shm/fsmark -D 16 -n 2000 -s 4096 -t 16 -L 1";;
 wis_mmap2)           D=/root/bench/will-it-scale; PREP=":";      C="./mmap2_threads -t 16 -s 15";;
 schbench)            D=/root; PREP=":";                          C="/root/bench/schbench/schbench -m 2 -t 8 -r 15";;
 nhextend_full)       D=/root; PREP=":";                          C="NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=5000 /root/linux-6.17/NHextend-full -n 16";;
 memtier_memcached)   D=/root; PREP="pkill -x memcached >/dev/null 2>&1; sleep 1; memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=10 >/dev/null 2>&1; sleep 2";
                      C="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=10 --ratio=1:0 -d 32 --test-time=10 --hide-histogram";;
 psearchy)            D=/root/mosbench/psearchy;
                      PREP="rm -rf /root/psearchy_db; for i in \$(seq 0 15); do mkdir -p /root/psearchy_db/db\$i; done";
                      C="bash -c './mkdb/pedsort -t /root/psearchy_db/db -c 16 -m 512 < files_6x'";;
 tinyconfig)          D=/root;
                      PREP="rm -rf /tmp/asb; mkdir -p /tmp/asb; make -C /root/kernels/linux-6.14-stock O=/tmp/asb tinyconfig >/dev/null 2>&1";
                      C="make -C /root/kernels/linux-6.14-stock O=/tmp/asb -j16 vmlinux";;
 parsec_blackscholes) D=$P/pkgs/apps/blackscholes/run;    PREP=":"; C="$P/pkgs/apps/blackscholes/inst/amd64-linux.gcc/bin/blackscholes 16 in_10M.txt prices.txt";;
 parsec_swaptions)    D=$P/pkgs/apps/swaptions/run;       PREP=":"; C="$P/pkgs/apps/swaptions/inst/amd64-linux.gcc/bin/swaptions -ns 128 -sm 1000000 -nt 16";;
 parsec_vips)         D=$P/pkgs/apps/vips/run;            PREP=":"; C="IM_CONCURRENCY=16 $P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v";;
 parsec_ferret)       D=$P/pkgs/apps/ferret/run;          PREP=":"; C="$P/pkgs/apps/ferret/inst/amd64-linux.gcc/bin/ferret corel lsh queries 50 20 16 output.txt";;
 parsec_bodytrack)    D=$P/pkgs/apps/bodytrack/run;       PREP=":"; C="$P/pkgs/apps/bodytrack/inst/amd64-linux.gcc/bin/bodytrack sequenceB_261 4 261 4000 5 0 16";;
 parsec_dedup)        D=$P/pkgs/kernels/dedup/run;        PREP="rm -f $P/pkgs/kernels/dedup/run/output.dat.ddp";
                      C="$P/pkgs/kernels/dedup/inst/amd64-linux.gcc/bin/dedup -c -p -v -t 16 -i FC-6-x86_64-disc1.iso -o output.dat.ddp";;
 parsec_canneal)      D=$P/pkgs/kernels/canneal/run;      PREP=":"; C="$P/pkgs/kernels/canneal/inst/amd64-linux.gcc/bin/canneal 16 15000 2000 2500000.nets 6000";;
 *) return 1;;
esac; return 0; }

ALL="idle_floor stressng_dentry perf_epoll_wait hackbench_pipe_thr sysbench_mutex
     ebizzy_mmap dbench_16 fsmark_tmpfs wis_mmap2 schbench nhextend_full
     memtier_memcached psearchy tinyconfig
     parsec_blackscholes parsec_swaptions parsec_vips parsec_ferret
     parsec_bodytrack parsec_dedup parsec_canneal"
LIST="${LIST:-$ALL}"

# --------------------------------------------------------------- one sample ---
one(){  # $1=workload $2=rep -> appends a CSV row, echoes acq/s
  local w=$1 r=$2
  spec "$w" || { echo "  !! unknown workload $w" >&2; return 1; }
  ( cd "$D" && eval "$PREP" ) >/dev/null 2>&1
  sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
  prof_start
  local s=$(date +%s.%N)
  ( cd "$D" && eval "$C" ) >/dev/null 2>&1
  local e=$(date +%s.%N)
  prof_stop
  prof_dump > /tmp/acq_fn.$$
  python3 - "$w" "$r" "$s" "$e" "$OUT" "$FNOUT" /tmp/acq_fn.$$ <<'PY'
import sys
w,r,s,e,out,fnout,fn = sys.argv[1:8]
dur = float(e)-float(s)
c = {}
for line in open(fn):
    p=line.split()
    if len(p)==2: c[p[0]]=c.get(p[0],0)+int(p[1])
slow = c.get('queued_spin_lock_slowpath',0)+c.get('__pv_queued_spin_lock_slowpath',0)
tot  = sum(c.values()) - slow          # slowpath is a CALLEE of _raw_spin_lock
tryl = c.get('_raw_spin_trylock',0)+c.get('_raw_spin_trylock_bh',0)
with open(out,'a') as f:
    f.write(f"{w},{r},{dur:.3f},{tot},{slow},{tryl},{tot/dur:.0f},{slow/dur:.0f}\n")
with open(fnout,'a') as f:
    f.write(f"### {w} rep{r} wall={dur:.3f}s total={tot} slowpath={slow}\n")
    for k,v in sorted(c.items(), key=lambda x:-x[1]):
        f.write(f"    {k:36} {v:14,d} {v/dur:12,.0f}/s\n")
print(f"{tot/dur:.0f}")
PY
  rm -f /tmp/acq_fn.$$
}

# -------------------------------------------------------------------- main ---
[ -w $T/function_profile_enabled ] || { echo "FATAL: no ftrace function profiler"; exit 1; }
echo "workload,rep,seconds,total_acq,slowpath,trylock,acq_per_s,slow_per_s" > "$OUT"
: > "$FNOUT"
echo "# arm: ivh_adaptive_mode=$(cat /proc/sys/kernel/ivh_adaptive_mode)  loadavg=$(cut -d' ' -f1 /proc/loadavg)" >> "$FNOUT"
echo "system-wide ftrace lock profiler | REPS=$REPS (up to $MAXREPS) | -> $OUT"

for w in $LIST; do
  spec "$w" || { echo "  !! skip unknown $w"; continue; }
  n=0
  while :; do
    n=$((n+1)); one "$w" "$n" >/dev/null || break
    [ "$n" -ge "$REPS" ] || continue
    # adaptive: keep going while the spread is wide and we have budget
    sp=$(grep "^$w," "$OUT" | awk -F, '{print $7}' | sort -n | awk 'NR==1{m=$1} END{printf "%.4f", $1/(m>0?m:1)}')
    over=$(python3 -c "print(1 if $sp > $SPREAD else 0)")
    [ "$over" = 1 ] && [ "$n" -lt "$MAXREPS" ] && continue
    break
  done
  grep "^$w," "$OUT" | python3 -c "
import sys,statistics as st
a=[];s=[];d=[]
for l in sys.stdin:
    f=l.strip().split(',')
    a.append(float(f[6])); s.append(float(f[7])); d.append(float(f[2]))
sp=max(a)/max(min(a),1)
cv=100*st.pstdev(a)/st.mean(a) if len(a)>1 else 0
print(f'  {\"$w\":21} n={len(a)} wall={st.median(d):6.1f}s  acq={st.median(a):12,.0f}/s'
      f'  slow={st.median(s):10,.0f}/s ({100*st.median(s)/max(st.median(a),1):5.2f}%)'
      f'  spread={sp:.2f}x cv={cv:.1f}%')"
done
pkill -x memcached >/dev/null 2>&1
echo "WROTE $OUT  (per-function: $FNOUT)"
echo ACQ-SUITE-DONE
