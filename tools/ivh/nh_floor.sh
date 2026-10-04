#!/bin/bash
# floor.tsv -- where is the migration-only floor, and does AFL move it?
# 5s runs, 2 rounds, loop_spin x IVH_AFL_DISABLE x arm.
set -u; cd /root; . /root/ivh_tools/nh_common.sh
OUT=${OUT:-/root/ivh_logs/floor.tsv}; : > "$OUT"
nhl(){ env IVH_AFL_DISABLE=$2 NHEXTEND_DURATION=5 NHEXTEND_LOOP_SPIN=$1 \
       timeout 60 /root/linux-6.17/NHextend-full -l -n 16 2>&1; }
for r in 1 2; do
  for sp in 25000 50000 100000 200000 300000 600000; do
    for afl in 1 0; do          # 1 = AFL disabled (pure busy-wait), 0 = AFL enabled
      setpv;  o=$(nhl $sp $afl)
      printf "%s\t%s\tpv\t%s\t%s\n" $sp $afl "$(g "$o" 'Ran for \K[0-9]+')" \
        "$(g "$o" 'HOLD-only preempted      : [0-9]+ / [0-9]+  \(\K[0-9.]+')" >> "$OUT"
      setivh; o=$(nhl $sp $afl)
      printf "%s\t%s\tivh\t%s\t%s\n" $sp $afl "$(g "$o" 'Ran for \K[0-9]+')" \
        "$(g "$o" 'HOLD-only preempted      : [0-9]+ / [0-9]+  \(\K[0-9.]+')" >> "$OUT"
    done
  done
  echo "  round $r done"
done
