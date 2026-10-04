#!/bin/bash
# Lever 1: does kernel adaptive spinning (spin_mode 2) rescue the two nulls?
# Both previous runs were spin_mode 1 in BOTH arms -- AS was never enabled.
#   pv  arm: eligible=0 spin_mode 1      ivh arm: eligible=1 spin_mode 2
set -u
S=/proc/sys/kernel
source /root/ivh_tools/bench_guard.sh
echo 2 > $S/ivh_pv_preempt_src   # TSC heartbeat; vcpu_is_preempted() is false on this host
CSV=as_test_$(date +%m%d_%H%M%S).csv
echo "workload,pair,arm,value" > $CSV
arm(){ case $1 in
  pv)  echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1
       [ "$(cat $S/ivh_adaptive_mode)" = 0 ] || exit 1;;
  ivh) echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1
       [ "$(cat $S/ivh_adaptive_mode)" = 2 ] || exit 1;;
esac; }

echo "########## tinyconfig kernel build, PV vs migration+AS ##########"
make -C /root/kernels/linux-6.14-stock O=/tmp/asb tinyconfig >/dev/null 2>&1
for p in $(seq 10); do
  [ $((p%2)) -eq 1 ] && ORDER="pv ivh" || ORDER="ivh pv"
  for a in $ORDER; do
    arm $a; sync; echo 3 > /proc/sys/vm/drop_caches
    make -C /root/kernels/linux-6.14-stock O=/tmp/asb clean >/dev/null 2>&1
    s=$(date +%s.%N)
    make -C /root/kernels/linux-6.14-stock O=/tmp/asb -j16 vmlinux >/dev/null 2>&1
    e=$(date +%s.%N); v=$(echo "$e-$s"|bc)
    echo "tinyconfig,$p,$a,$v" | tee -a $CSV
  done
done
rm -rf /tmp/asb

echo "########## psearchy, PV vs migration+AS ##########"
for p in $(seq 10); do
  [ $((p%2)) -eq 1 ] && ORDER="pv ivh" || ORDER="ivh pv"
  for a in $ORDER; do
    arm $a; sync; echo 3 > /proc/sys/vm/drop_caches
    rm -rf /root/psearchy_db; for i in $(seq 0 15); do mkdir -p /root/psearchy_db/db$i; done
    v=$( (cd /root/mosbench/psearchy && timeout 900 ./mkdb/pedsort -t /root/psearchy_db/db -c 16 -m 512 < files_6x) 2>&1 | grep -oP 'throughput:\s*\K[0-9.]+')
    echo "psearchy,$p,$a,$v" | tee -a $CSV
  done
done
echo "WROTE $CSV"; echo AS-TEST-DONE
