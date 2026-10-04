#!/bin/bash
# One-shot benchmark setup. After `git pull`, run this and you have the whole
# 10-workload suite. Everything that can be vendored IS vendored here; the two
# multi-gigabyte trees are fetched.
#
#   ./setup_benchmarks.sh            # everything
#   ./setup_benchmarks.sh apt        # just the distro packages
#   ./setup_benchmarks.sh parsec     # just PARSEC (17GB, slowest by far)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
STEP="${1:-all}"
run(){ echo "=== $* ==="; }

if [ "$STEP" = all ] || [ "$STEP" = apt ]; then
  run "distro packages"
  # hackbench lives in rt-tests; fs_mark in fsmark.
  apt-get update -qq
  apt-get install -y rt-tests fsmark dbench sysbench stress-ng \
                     build-essential git autoconf automake libtool pkg-config \
                     libpcre3-dev libevent-dev libssl-dev zlib1g-dev \
                     memcached libgsl-dev libjpeg-dev libglib2.0-dev \
                     bpftrace linux-tools-common sshpass bc
fi

if [ "$STEP" = all ] || [ "$STEP" = ebizzy ]; then
  run "ebizzy (vendored -- it is NOT packaged, it ships inside rt-tests source)"
  ( cd "$HERE/ebizzy-0.3" && make clean >/dev/null 2>&1; make ) || echo "*** ebizzy build FAILED"
  echo "  -> $HERE/ebizzy-0.3/ebizzy"
  echo "  invocation: ebizzy -S 15 -t 16 -m -s 4194304   (needs a DISCARDED WARMUP per arm)"
fi

if [ "$STEP" = all ] || [ "$STEP" = nhextend ]; then
  run "NHextend-csmin (vendored: csmin + pre-acquire stamp, AFL off)"
  ( cd "$HERE" && gcc -O2 -pthread -o NHextend-csmin NHextend-csmin.c -lm ) \
    || echo "*** NHextend build FAILED -- check it found ivh_adaptive_futex_lock_csmin.h"
  echo "  -> $HERE/NHextend-csmin"
  echo "  its wait metric is the program's own 'Total wait time' (userspace AFL lock),"
  echo "  NOT ivh_slowpath_wait_ns, which reads ~118 events of noise for it."
fi

if [ "$STEP" = all ] || [ "$STEP" = memtier ]; then
  run "memtier_benchmark (clone + build, ~40MB)"
  [ -d /root/memtier_benchmark ] || git clone --depth 1 \
      https://github.com/RedisLabs/memtier_benchmark.git /root/memtier_benchmark
  ( cd /root/memtier_benchmark && autoreconf -ivf >/dev/null 2>&1 && ./configure >/dev/null && make -j"$(nproc)" ) \
    || echo "*** memtier build FAILED"
  echo "  server MUST be tuned, hashpower >= 15 at -t 16:"
  echo "    systemctl stop memcached; pkill -9 -x memcached"
  echo "    memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=15"
fi

if [ "$STEP" = all ] || [ "$STEP" = parsec ]; then
  run "PARSEC (17GB with native inputs -- dedup, vips, bodytrack, canneal)"
  [ -d /root/parsec-benchmark ] || git clone --depth 1 \
      https://github.com/cirosantilli/parsec-benchmark.git /root/parsec-benchmark
  ( cd /root/parsec-benchmark && for p in dedup vips bodytrack canneal; do
      echo "  building $p"; ./bin/parsecmgmt -a build -p "$p" -c gcc >/dev/null 2>&1 \
        || echo "  *** $p build FAILED"
    done )
  echo "  run as: ./bin/parsecmgmt -a run -p <pkg> -c gcc -i native -n 16   (cwd /root/parsec-benchmark)"
  echo "  dedup needs the page cache dropped before EVERY run + a discarded warmup"
  echo "  (tools/ivh/parsec_ab.sh does both); vips is BIMODAL and needs ~50 pairs."
fi

if [ "$STEP" = all ] || [ "$STEP" = dbench ]; then
  run "dbench working directory"
  mkdir -p /root/dbench_test
  echo "  invocation: dbench -t 15 16 -D /root/dbench_test    <-- NO -F"
  echo "  (-F measured -1%; without it +14.66%, t=10.87)"
fi

echo
echo "=== from apt, nothing to build: hackbench (rt-tests), dbench, fs_mark (fsmark), sysbench, stress-ng"
echo "=== vendored in this repo:    ebizzy, NHextend-csmin + its AFL header"
echo "=== fetched:                  memtier_benchmark, PARSEC"
echo "=== invocation authority:     tools/ivh/campaign/benchmarks.tsv (no PARSEC rows; see ple/RUNBOOK.md)"
