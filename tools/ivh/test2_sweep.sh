#!/bin/bash
# How low can the LH fire threshold go before it starts lying?
#
# threshold = ivh_cs_tick_period * ivh_cs_owed_ticks.
# The SAME product is used for exoneration (a holder whose beat is fresher
# than the threshold is judged alive), so lowering it makes both the fire gate
# AND the liveness check more aggressive.
#
# THE STRUCTURAL LIMIT: a healthy holder publishes its beat from the TICK
# (kernel/sched/cputime.c:547), i.e. once per 1 ms. Set the threshold below
# 1 ms and a perfectly alive holder that simply has not ticked yet looks
# stale -> false positive by construction.
#
# Scored by comparing `fired` against the hold histogram's own count above the
# same threshold. Migration OFF throughout: it prevents the long holds we are
# trying to observe.
set -u
S=/proc/sys/kernel
source /root/ivh_tools/bench_guard.sh
echo 0 > $S/ivh_universal_eligible        # migration OFF
/root/spin_mode 2 >/dev/null 2>&1
echo 2 > $S/ivh_pv_preempt_src
echo 1 > $S/ivh_cs_track_enabled; echo 0 > $S/ivh_cs_criterion
echo 1 > $S/ivh_cs_owner_enable; echo 0 > $S/ivh_cs_owner_clear
echo 1 > $S/ivh_cs_head_probe;  echo 0 > $S/ivh_pv_rot_enable
rd(){ python3 /root/ivh_tools/read_ivh_counters.py "$@" 2>/dev/null; }
for SPEC in "2200000 2 2000us" "2200000 1 1000us" "1100000 1 500us" "550000 1 250us"; do
  set -- $SPEC; TP=$1; OT=$2; LBL=$3
  echo "$TP" > $S/ivh_cs_tick_period; echo "$OT" > $S/ivh_cs_owed_ticks
  echo "########## threshold $LBL  (tick_period=$TP owed_ticks=$OT) ##########"
  A=$(rd ivh_cs_long_hold ivh_cs_healthy_long ivh_cs_fired ivh_cs_abstain_young | awk '{print $3}' | tr '\n' ' ')
  timeout 180 hackbench -T -g1 -f8 -l500000 >/dev/null 2>&1
  B=$(rd ivh_cs_long_hold ivh_cs_healthy_long ivh_cs_fired ivh_cs_abstain_young | awk '{print $3}' | tr '\n' ' ')
  echo "  before: $A"
  echo "  after : $B"
  rd ivh_cs_prev_hold_hist | head -1
done
echo 2200000 > $S/ivh_cs_tick_period; echo 2 > $S/ivh_cs_owed_ticks
echo SWEEP-DONE
