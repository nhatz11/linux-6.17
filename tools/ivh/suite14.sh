#!/bin/bash
# suite14.sh -- THE workload set for points 7 and 8 (2026-09-29).
#
# Fourteen workloads, ONE invocation each. Everything else is removed:
# no sysbench, no perf bench, no stress-ng, no fsmark/sysbench short forms,
# no second variant of anything.
#
# CONFIG AUTHORITY. campaign/benchmarks.tsv holds the CAMPAIGN config;
# eval_final.md's "suite cfg" table SUPERSEDES it where a scaled form exists.
# fsmark is the scaled form: -n 30000 runs 5.64 s in the PV arm (vs 0.48 s at
# -n 2000) and needs 1,875 MB in /dev/shm. Validated 5/5 pairs, t=24.96.
#
# PARSEC runs DIRECT, not via ./bin/parsecmgmt: the wrapper takes ~600,000
# spinlocks/s of its own and adds its shell overhead to every TIME measurement.
# Invocations are the native.runconf run_exec/run_args with NTHREADS=16, from
# each package's run/ directory where the native input is staged.
#
# Fields: name | dir | metric | command | extractor
#   metric TIME       -> lower is better, harness times the run
#   metric THROUGHPUT -> higher is better, extractor pulls the number
set -u
P=/root/parsec-benchmark

SUITE14=(
"ebizzy_mmap|/root|THROUGHPUT|/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304|grep -oP '^\K[0-9]+(?= records/s)'"
"hackbench_pipe_thr|/root|TIME_SELF|hackbench -T -g1 -f8 -l150000|grep -oP '^Time:\s*\K[0-9.]+'"
"dbench_16|/root|THROUGHPUT|dbench -F -t 15 16 -D /root/dbench_test|grep -oP 'Throughput\s+\K[0-9.]+'"
"wis_mmap2|/root/bench/will-it-scale|THROUGHPUT|./mmap2_threads -t 16 -s 15|grep -oP 'average:\K[0-9]+'"
"fsmark_tmpfs|/root|THROUGHPUT|fs_mark -d /dev/shm/fsmark -D 16 -n 30000 -s 4096 -t 16 -L 1|grep -oP '^\s*\d+\s+\d+\s+\d+\s+\K[0-9.]+'"
"psearchy|/root/mosbench/psearchy|THROUGHPUT|./mkdb/pedsort -t /root/psearchy_db/db -c 16 -m 512 < files_6x|grep -oP 'throughput: \K[0-9.]+'"
"memtier_memcached|/root|THROUGHPUT|MEMTIER_CMD|grep -oP 'Totals\s+\K[0-9.]+'"
"nhextend_full|/root|THROUGHPUT|NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=5000 /root/linux-6.17/NHextend-full -n 16|grep -oP 'Ran for \K[0-9]+'"
"schbench|/root|THROUGHPUT|/root/bench/schbench/schbench -m 2 -t 8 -r 15|grep -oP 'average rps:\s*\K[0-9.]+'"
"parsec_vips|$P/pkgs/apps/vips/run|TIME|IM_CONCURRENCY=16 $P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v|x"
"parsec_dedup|$P/pkgs/kernels/dedup/run|TIME|$P/pkgs/kernels/dedup/inst/amd64-linux.gcc/bin/dedup -c -p -v -t 16 -i FC-6-x86_64-disc1.iso -o output.dat.ddp|x"
"tinyconfig|/root|TIME|make -C /root/kernels/linux-6.14-stock O=/tmp/asb -j16 vmlinux|x"
"parsec_bodytrack|$P/pkgs/apps/bodytrack/run|TIME|$P/pkgs/apps/bodytrack/inst/amd64-linux.gcc/bin/bodytrack sequenceB_261 4 261 4000 5 0 16|x"
"parsec_canneal|$P/pkgs/kernels/canneal/run|TIME|$P/pkgs/kernels/canneal/inst/amd64-linux.gcc/bin/canneal 16 15000 2000 2500000.nets 6000|x"
)

# Per-workload prep, run OUTSIDE any measured/traced window.
prep14(){
case "$1" in
  fsmark_tmpfs) rm -rf /dev/shm/fsmark; mkdir -p /dev/shm/fsmark ;;
  dbench_16)    mkdir -p /root/dbench_test ;;
  psearchy)     rm -rf /root/psearchy_db; for i in $(seq 0 15); do mkdir -p /root/psearchy_db/db$i; done ;;
  tinyconfig)   rm -rf /tmp/asb; mkdir -p /tmp/asb
                make -C /root/kernels/linux-6.14-stock O=/tmp/asb tinyconfig >/dev/null 2>&1 ;;
  parsec_dedup) rm -f /root/parsec-benchmark/pkgs/kernels/dedup/run/output.dat.ddp ;;
  memtier_memcached)
                systemctl stop memcached >/dev/null 2>&1; sleep 1
                for p in $(pgrep -x memcached 2>/dev/null); do kill $p 2>/dev/null; done; sleep 1
                memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=15; sleep 2
                pid=$(ss -lntp 2>/dev/null | grep -oP '11211.*pid=\K[0-9]+' | head -1)
                [ -n "$pid" ] || { echo "FATAL: no memcached on 11211"; return 1; }
                grep -q -- "-t 16" /proc/$pid/cmdline 2>/dev/null || \
                  tr '\0' ' ' < /proc/$pid/cmdline | grep -q -- "-t 16" || \
                  { echo "FATAL: wrong memcached serving: $(tr '\0' ' ' < /proc/$pid/cmdline)"; return 1; } ;;
esac
return 0
}
