#!/bin/bash
# Does a smaller per-core hash table (-m) raise psearchy's CONTENDED lock rate?
# Smaller -m => more flush/merge cycles => more VFS/page-cache locking.
set -u
echo 0 > /proc/sys/kernel/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1
for m in 512 128 32; do
  sync; echo 3 > /proc/sys/vm/drop_caches
  rm -rf /root/psearchy_db; for i in $(seq 0 15); do mkdir -p /root/psearchy_db/db$i; done
  /root/ivh_tools/lockrate.sh "psearchy -m $m" bash -c \
    "cd /root/mosbench/psearchy && timeout 900 ./mkdb/pedsort -t /root/psearchy_db/db -c 16 -m $m < files_6x"
done
echo M-PROBE-DONE
