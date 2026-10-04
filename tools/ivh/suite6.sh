#!/bin/bash
# suite6.sh -- the six workloads for point 7 (2026-09-30), user-selected.
# REVISED 2026-10-01: NHextend at LOOP_SPIN=20000 (5000 is far below the
# documented operating point) and IVH_AFL_DISABLE=1 -- its adaptive lock is
# userspace and no sysctl disables it, so migration-only needs the env var.
# REVISED: dbench replaces fsmark, vips replaces dedup. Losing dedup also
# loses the CV-82% hazard (15.2-231.5s runs, once faked a -50% regression).
# Invocations lifted verbatim from suite12.sh, which is the config authority.
set -u
P=/root/parsec-benchmark

SUITE6=(
"memtier_memcached|/root|THROUGHPUT|MEMTIER_CMD|grep -oP 'Totals\s+\K[0-9.]+'"
"hackbench_pipe_thr|/root|TIME|hackbench -T -g1 -f8 -l150000|grep -oP '^Time:\s*\K[0-9.]+'"
"ebizzy_mmap|/root|THROUGHPUT|/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304|grep -oP '^\K[0-9]+(?= records/s)'"
"dbench_16|/root|THROUGHPUT|dbench -F -t 15 16 -D /root/dbench_test|grep -oP 'Throughput\s+\K[0-9.]+'"
"nhextend_full|/root|THROUGHPUT|IVH_AFL_DISABLE=1 NHEXTEND_DURATION=10 NHEXTEND_LOOP_SPIN=10000 /root/linux-6.17/NHextend-full -n 16|grep -oP 'Ran for \K[0-9]+'"
"parsec_vips|$P/pkgs/apps/vips/run|TIME|IM_CONCURRENCY=16 $P/pkgs/apps/vips/inst/amd64-linux.gcc/bin/vips im_benchmark orion_18000x18000.v output.v|x"
)

MEMTIER_CMD="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=100 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"

prep6(){
case "$1" in
  dbench_16)    mkdir -p /root/dbench_test ;;
  parsec_vips)  rm -f /root/parsec-benchmark/pkgs/apps/vips/run/output.v ;;
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
