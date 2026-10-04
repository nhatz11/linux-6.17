#!/bin/bash
# Sweep ivh_eval_cooldown_ns. At the shipped 50000 it discards 98.46% of
# prelock calls, so it -- not lock-entrance coverage -- sets the migration rate.
# Question: is 50us justified by measurement, or merely plausible?
set -u
S=/proc/sys/kernel
export PARSECDIR=/root/parsec-benchmark
source /root/ivh_tools/bench_guard.sh
CSV=cooldown_sweep_$(date +%m%d_%H%M%S).csv
echo "workload,cooldown_ns,rep,seconds,migrations" > $CSV
mig(){ python3 /root/ivh_tools/migcount.py 2>/dev/null || echo 0; }
run(){ case $1 in
  blackscholes) s=$(date +%s.%N)
     (cd $PARSECDIR && ./bin/parsecmgmt -a run -p blackscholes -c gcc -i native -n 16) >/dev/null 2>&1
     e=$(date +%s.%N); echo "$e-$s"|bc;;
  hackbench) /usr/bin/hackbench -T -g1 -f8 -l150000 2>&1 | grep -oP '^Time:\s*\K[0-9.]+';;
esac; }
for w in blackscholes hackbench; do
  echo "########## $w ##########"
  for cd_ns in PV 0 10000 25000 50000 100000 400000; do
    if [ "$cd_ns" = PV ]; then
      echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1
    else
      echo 1 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1
      echo "$cd_ns" > $S/ivh_eval_cooldown_ns
      [ "$(cat $S/ivh_eval_cooldown_ns)" = "$cd_ns" ] || { echo "cooldown write FAILED ($cd_ns)"; continue; }
    fi
    for r in 1 2 3; do
      sync; echo 3 > /proc/sys/vm/drop_caches
      m0=$(mig); v=$(run $w); m1=$(mig)
      echo "$w,$cd_ns,$r,$v,$((m1-m0))" | tee -a $CSV
    done
  done
done
echo 50000 > $S/ivh_eval_cooldown_ns
echo "WROTE $CSV"; echo COOLDOWN-SWEEP-DONE
