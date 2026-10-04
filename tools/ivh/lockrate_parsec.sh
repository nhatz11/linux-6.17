#!/bin/bash
set -u
export PARSECDIR=/root/parsec-benchmark
P=$PARSECDIR
echo 0 > /proc/sys/kernel/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1
for pkg in dedup canneal; do
  sync; echo 3 > /proc/sys/vm/drop_caches
  /root/ivh_tools/lockrate.sh "$pkg" bash -c "cd $P && ./bin/parsecmgmt -a run -p $pkg -c gcc -i native -n 16"
done
echo LOCKRATE-PARSEC-DONE
