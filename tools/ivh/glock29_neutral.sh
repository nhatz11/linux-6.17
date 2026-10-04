#!/bin/bash
# G-LOCK-29 neutrality, all ivh_cs_* off: N rounds IVH+AS, capacity-settled wait before each.
set -u; N=${N:-6}; S=/proc/sys/kernel; W="hackbench -T -g1 -f8 -l400000"
for f in ivh_cs_owner_enable ivh_cs_owner_clear ivh_cs_head_probe ivh_cs_head_bail; do [ "$(cat $S/$f)" = 0 ] || { echo "FATAL $f"; exit 1; }; done
echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null; dmesg -n 1
for i in $(seq 1 $N); do QUIET=1 /root/ivh_tools/wait_capacity_settled.sh >/dev/null; v=$($W 2>&1 | grep -oP '^Time:\s*\K[0-9.]+'); echo "round $i time=${v}s"; done
