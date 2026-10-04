#!/bin/bash
set -u
S=/proc/sys/kernel
echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1   # PV baseline
P=/root/parsec
run_parsec(){ (cd $P && ./bin/parsecmgmt -a run -p $1 -c gcc -i native -n 16); }
sync; echo 3 > /proc/sys/vm/drop_caches
/root/ivh_tools/lockrate.sh "dedup (win +86.9%)"    bash -c "cd $P && ./bin/parsecmgmt -a run -p dedup -c gcc -i native -n 16"
sync; echo 3 > /proc/sys/vm/drop_caches
/root/ivh_tools/lockrate.sh "canneal (dead +0.1%)"  bash -c "cd $P && ./bin/parsecmgmt -a run -p canneal -c gcc -i native -n 16"
sync; echo 3 > /proc/sys/vm/drop_caches
/root/ivh_tools/lockrate.sh "ferret (win +15.2%)"   bash -c "cd $P && ./bin/parsecmgmt -a run -p ferret -c gcc -i native -n 16"
sync; echo 3 > /proc/sys/vm/drop_caches
rm -rf /root/psearchy_db; for i in $(seq 0 15); do mkdir -p /root/psearchy_db/db$i; done
/root/ivh_tools/lockrate.sh "psearchy (dead +0.8%)" bash -c "cd /root/mosbench/psearchy && ./mkdb/pedsort -t /root/psearchy_db/db -c 16 -m 512 < files_6x"
echo LOCKRATE-SUITE-DONE
