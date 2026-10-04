#!/bin/bash
# csmin_row.sh -- the #9 extend-sched row under the new configuration:
#   base_slice_ns = 1 ms      (the optimum: +13.59% vs +8.83% at the 2.8 ms default)
#   NHextend-csmin            holder stamps ONCE at acquisition, no in-CS republish;
#                             waiters sleep on elapsed-CS > cmin + delta (10 us)
#   NHEXTEND_CS_MIN=1         publishes the running MINIMUM into rseq, so Gate 2's
#                             time-left term is driven from csmin too
#   AFL ENABLED               required -- IVH_AFL_DISABLE=1 forces pure spin and the
#                             sleep predicate could never fire
set -u
BS=/sys/kernel/debug/sched/base_slice_ns
OLD=$(cat $BS)
restore() { echo "$OLD" > $BS 2>/dev/null; echo "### base_slice_ns restored to $(cat $BS)"; }
trap restore EXIT
echo 1000000 > $BS || { echo "FATAL"; exit 1; }
echo "### base_slice_ns = $(cat $BS)"
export BT_SCRIPT=/root/ivh_tools/migcost_light.bt BTPV=1
AFL=0 THRESH=1900000 IVH_NH_BIN=/root/linux-6.17/NHextend-csmin IVH_NH_CSMIN=1 \
	bash /root/ivh_tools/spotlight_sleep.sh nhextend_fin 3
