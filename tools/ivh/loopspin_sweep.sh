#!/bin/bash
# loopspin_sweep.sh -- where does migration's benefit die as the CS shortens?
#
# base_slice_ns held at 1 ms: the optimum found by slice_sweep.sh
#   2.80ms +8.83%  |  1.00ms +13.59%  |  0.50ms -0.28%  |  0.10ms -12.61%
# 1 ms == the HZ=1000 tick, so the slice is enforced promptly with no wasted
# residual; below it you pay rescheduling churn without getting latency back.
#
# NHEXTEND_LOOP_SPIN sets the critical-section length. At 600000 the CS measures
# 1305.6 us (the run's own CS stats). Shortening it cuts BOTH sides: an arrival
# waits less behind a resident, but the window in which a preempted holder is
# worth migrating away from also shrinks. The project's own records mark
# loop_spin=5000 as "dead (do-no-harm only)", so the knee is between.
#
# Restores base_slice_ns on every exit path -- it is a system-wide knob.
set -u
BS=/sys/kernel/debug/sched/base_slice_ns
OLD=$(cat $BS)
restore() { echo "$OLD" > $BS 2>/dev/null; echo "### base_slice_ns restored to $(cat $BS)"; }
trap restore EXIT
echo 1000000 > $BS || { echo "FATAL: cannot set base_slice"; exit 1; }
echo "### base_slice_ns pinned at $(cat $BS) for the whole sweep"
export BT_SCRIPT=/root/ivh_tools/migcost_light.bt BTPV=1
for LS in 600000 200000 60000; do
	echo
	echo "######## NHEXTEND_LOOP_SPIN = $LS ########"
	AFL=1 THRESH=1900000 IVH_LOOP_SPIN=$LS bash /root/ivh_tools/spotlight_sleep.sh nhextend_fin 3
done
