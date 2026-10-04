#!/bin/bash
# G-LOCK-40 deep probe on the FOUR NEUTRALS. 6 pairs each, ABBA-alternated,
# PV vs migration+AS (the full config the paper uses, and the best shot at
# rescuing them). The RCU fix REMOVES migrations taken from inside RCU readers;
# on VFS/dcache-heavy workloads those were blocking in a reader and extending
# grace periods, so this is where a gain would show up if anywhere.
#
# Baselines to beat (G-LOCK-39, sampler off):
#   psearchy     migration-alone +0.82% ns   migration+AS +1.54% ns
#   tinyconfig   migration-alone +0.99% sig  migration+AS +1.71% (median -0.49%) ns
#   canneal      migration-alone +0.14% ns
#   blackscholes migration-alone +1.07% ns
set -u
S=/proc/sys/kernel
export PARSECDIR=/root/parsec-benchmark
source /root/ivh_tools/bench_guard.sh
echo 2 > $S/ivh_pv_preempt_src
CSV=g40_neutrals_$(date +%m%d_%H%M%S).csv
echo "workload,pair,arm,value" > $CSV
arm(){ case $1 in
  pv)  echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1
       [ "$(cat $S/ivh_adaptive_mode)" = 0 ] || exit 1;;
  ivh) echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1
       [ "$(cat $S/ivh_adaptive_mode)" = 2 ] || exit 1;;
esac; }
run(){ case $1 in
  canneal|blackscholes)
      s=$(date +%s.%N)
      (cd $PARSECDIR && ./bin/parsecmgmt -a run -p $1 -c gcc -i native -n 16) >/dev/null 2>&1
      e=$(date +%s.%N); echo "$e-$s" | bc ;;
  psearchy)
      rm -rf /root/psearchy_db; for i in $(seq 0 15); do mkdir -p /root/psearchy_db/db$i; done
      (cd /root/mosbench/psearchy && timeout 900 ./mkdb/pedsort -t /root/psearchy_db/db -c 16 -m 512 < files_6x) 2>&1 \
        | grep -o "throughput: [0-9.]*" | tail -1 | awk '{print $2}' ;;
  tinyconfig)
      make -C /root/kernels/linux-6.14-stock O=/tmp/g40b clean >/dev/null 2>&1
      s=$(date +%s.%N)
      make -C /root/kernels/linux-6.14-stock O=/tmp/g40b -j16 vmlinux >/dev/null 2>&1
      e=$(date +%s.%N); echo "$e-$s" | bc ;;
esac; }
make -C /root/kernels/linux-6.14-stock O=/tmp/g40b tinyconfig >/dev/null 2>&1
for w in blackscholes psearchy tinyconfig canneal; do
  echo "########## $w ##########"
  for p in $(seq 6); do
    [ $((p%2)) -eq 1 ] && ORDER="pv ivh" || ORDER="ivh pv"
    for a in $ORDER; do
      arm $a; sync; echo 3 > /proc/sys/vm/drop_caches
      v=$(run $w); echo "$w,$p,$a,$v" | tee -a $CSV
    done
  done
done
rm -rf /tmp/g40b
echo "WROTE $CSV"; echo G40-NEUTRALS-DONE
