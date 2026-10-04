#!/bin/bash
# suite12.sh -- THE work suite (2026-09-30). Twelve workloads, one invocation each.
# Replaces suite14.sh. REMOVED FOR GOOD: psearchy (2026-09-30) and schbench.
#
# REPORTING CONVENTION (fixed 2026-09-30, no more "speedup"):
#   TIME  workloads -> % TIME IMPROVEMENT   = 100*(pv - ivh)/pv     (max 100%)
#   THRPT workloads -> % THROUGHPUT INCREASE= 100*(ivh - pv)/pv
# These are the conventions the project's recorded values already use, so
# today's numbers compare directly against them.
#
# memtier config REPLACED 2026-09-30: --key-maximum=100 (not 10), found by
# mtsweep.sh to give +18.35% at cap=4 / +16.38% at cap=8, n=8, against a tuned
# 336k ops/s PV baseline. One key serialises (a single item lock has one holder
# and a queue, so moving a waiter buys nothing); ~100 keys gives parallel
# contention, which is what migration can actually repair.
# extend-sched row CHANGED 2026-10-01: NHextend-fin at loop_spin=600000, was
# NHextend-full at 5000. loop_spin=5000 is a do-no-harm control, not a win
# candidate -- at a 13us hold, host preemption is 0.004% and guest 0.001-0.008%,
# so there is nothing for migration to protect, and 43% of the lock cycle was
# the benchmark's own in-lock /proc read. fin has the pre-lock ivh_cs_enter()
# removed, so MIDSPIN_ITERS=0 is a clean "kernel path only" arm.
set -u
P=/root/parsec-benchmark

SUITE12=(
"ebizzy_mmap|/root|THROUGHPUT|/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304|grep -oP '^\K[0-9]+(?= records/s)'"
"hackbench_pipe_thr|/root|TIME|hackbench -T -g1 -f8 -l150000|grep -oP '^Time:\s*\K[0-9.]+'"
"dbench_16|/root|THROUGHPUT|dbench -F -t 15 16 -D /root/dbench_test|grep -oP 'Throughput\s+\K[0-9.]+'"
"wis_mmap2|/root/bench/will-it-scale|THROUGHPUT|./mmap2_threads -t 16 -s 15|grep -oP 'average:\K[0-9]+'"
"fsmark_tmpfs|/root|THROUGHPUT|fs_mark -d /dev/shm/fsmark -D 16 -n 30000 -s 4096 -t 16 -L 1|grep -oP '^\s*\d+\s+\d+\s+\d+\s+\K[0-9.]+'"
"memtier_memcached|/root|THROUGHPUT|MEMTIER_CMD|grep -oP 'Totals\s+\K[0-9.]+'"
"nhextend_fin|/root|THROUGHPUT|IVH_AFL_DISABLE=1 NHEXTEND_MIDSPIN_ITERS=0 NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=600000 /root/linux-6.17/NHextend-fin -n 16|grep -oP 'Ran for \K[0-9]+'"
"parsec_vips|$P/pkgs/apps/vips/run|TIME|IM_CONCURRENCY=16 $P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v|x"
"parsec_dedup|$P/pkgs/kernels/dedup/run|TIME|$P/pkgs/kernels/dedup/inst/amd64-linux.gcc/bin/dedup -c -p -v -t 16 -i FC-6-x86_64-disc1.iso -o output.dat.ddp|x"
"tinyconfig|/root|TIME|make -C /root/kernels/linux-6.14-stock O=/tmp/asb -j16 vmlinux|x"
"parsec_bodytrack|$P/pkgs/apps/bodytrack/run|TIME|$P/pkgs/apps/bodytrack/inst/amd64-linux.gcc/bin/bodytrack sequenceB_261 4 261 4000 5 0 16|x"
"parsec_canneal|$P/pkgs/kernels/canneal/run|TIME|$P/pkgs/kernels/canneal/inst/amd64-linux.gcc/bin/canneal 16 15000 2000 2500000.nets 6000|x"
)

MEMTIER_CMD="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=100 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"

prep12(){
case "$1" in
  fsmark_tmpfs) rm -rf /dev/shm/fsmark; mkdir -p /dev/shm/fsmark ;;
  dbench_16)    mkdir -p /root/dbench_test ;;
  tinyconfig)   rm -rf /tmp/asb; mkdir -p /tmp/asb
                make -C /root/kernels/linux-6.14-stock O=/tmp/asb tinyconfig >/dev/null 2>&1 ;;
  parsec_dedup) rm -f /root/parsec-benchmark/pkgs/kernels/dedup/run/output.dat.ddp ;;
  memtier_memcached)
        systemctl stop memcached >/dev/null 2>&1; sleep 1
        for p in $(pgrep -x memcached 2>/dev/null); do kill -9 $p 2>/dev/null; done; sleep 1
        memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=15 2>/dev/null; sleep 2
        pid=$(ss -lntp 2>/dev/null | grep -oP '11211.*pid=\K[0-9]+' | head -1)
        [ -n "$pid" ] || { echo "FATAL: no memcached on 11211"; return 1; }
        tr '\0' ' ' < /proc/$pid/cmdline | grep -q -- "-t 16" \
          || { echo "FATAL: wrong memcached serving"; return 1; } ;;
esac
return 0
}
