#!/bin/bash
# Validation: CYCLES x (capacity-settled wait, then ONE IVH+AS hackbench round).
set -u
CYCLES=${CYCLES:-6}; S=/proc/sys/kernel; W="hackbench -T -g1 -f8 -l400000"
echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null; dmesg -n 1
for i in $(seq 1 $CYCLES); do
    QUIET=1 /root/ivh_tools/wait_capacity_settled.sh
    v=$($W 2>&1 | grep -oP '^Time:\s*\K[0-9.]+')
    echo "cycle $i time=${v}s"
done
