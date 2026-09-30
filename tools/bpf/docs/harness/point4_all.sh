#!/bin/bash
# point4_all.sh -- migratable-thread census across the 12-workload suite.
set -u
T=/root/ivh_tools
source $T/suite12.sh
WINDOW="${WINDOW:-10}"
bash $T/p78_arm.sh tlt 10000000 >/dev/null 2>&1; echo 8 > /proc/sys/kernel/ivh_max_concurrent
echo "arm: full stack, tlt=10ms, cap=8, rcu_guard=$(cat /proc/sys/kernel/ivh_rcu_guard)"
declare -A COMM=( [ebizzy_mmap]=ebizzy [hackbench_pipe_thr]=hackbench [dbench_16]=dbench
  [wis_mmap2]=mmap2_threads [fsmark_tmpfs]=fs_mark [memtier_memcached]=memtier_benchma
  [nhextend_full]=NHextend-full [parsec_vips]=vips [parsec_dedup]=dedup
  [tinyconfig]=make [parsec_bodytrack]=bodytrack [parsec_canneal]=canneal )
for e in "${SUITE12[@]}"; do
  IFS='|' read -r n d m c x <<< "$e"
  [ "$c" = MEMTIER_CMD ] && c="$MEMTIER_CMD"
  prep12 "$n" >/dev/null 2>&1 || { echo "!! prep $n failed"; continue; }
  WARMUP=$([ "$n" = parsec_canneal ] && echo 25 || echo 0) bash $T/point4.sh "${COMM[$n]}" "$d" "$c" "$WINDOW" 2>&1 | sed "s/=== ${COMM[$n]} ===/=== $n (comm=${COMM[$n]}) ===/"
done
