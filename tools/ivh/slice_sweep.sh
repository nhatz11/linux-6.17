#!/bin/bash
# slice_sweep.sh -- EEVDF base_slice_ns vs migration benefit.
#
# At 100 us the delay fell 1485 -> 625 us but migration went from +12.90% to
# about -12%: cutting the delay destroyed the benefit. This sweeps the middle,
# including the default as an in-sitting reference so the comparison is not
# cross-sitting (PV's own level has swung 28% between sittings tonight).
#
# NO_HRTICK + HZ=1000 means slice exhaustion is caught on the 1 ms tick, so
# anything below ~1 ms is expected to be tick-bound at ~500-600 us of delay.
set -u
BS=/sys/kernel/debug/sched/base_slice_ns
OLD=$(cat $BS)
restore() { echo "$OLD" > $BS 2>/dev/null; echo "### base_slice_ns restored to $(cat $BS)"; }
trap restore EXIT
export BT_SCRIPT=/root/ivh_tools/migcost_light.bt BTPV=1
for S in 2800000 1000000 500000; do
	echo "$S" > $BS || { echo "FATAL: cannot set $S"; exit 1; }
	echo
	echo "######## base_slice_ns = $(cat $BS) ($(python3 -c "print(f'{$S/1e6:.2f}')") ms) ########"
	AFL=1 THRESH=1900000 bash /root/ivh_tools/spotlight_sleep.sh nhextend_fin 3
done
