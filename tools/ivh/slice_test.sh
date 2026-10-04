#!/bin/bash
# slice_test.sh -- does the migration delay track EEVDF's base_slice_ns?
# Restores base_slice_ns on EVERY exit path: it is a system-wide scheduler knob.
set -u
BS=/sys/kernel/debug/sched/base_slice_ns
OLD=$(cat $BS)
restore() { echo "$OLD" > $BS 2>/dev/null; echo "  base_slice_ns restored to $(cat $BS)"; }
trap restore EXIT
echo "  base_slice_ns was $OLD"
echo "$1" > $BS || { echo "FATAL: cannot write $BS"; exit 1; }
echo "  base_slice_ns now $(cat $BS)"
export BT_SCRIPT=/root/ivh_tools/migcost_light.bt BTPV=1
AFL=1 THRESH=1900000 bash /root/ivh_tools/spotlight_sleep.sh nhextend_fin 3
