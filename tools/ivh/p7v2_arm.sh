#!/bin/bash
# p7v2_arm.sh <pv|THRESH_NS> -- point 7 rerun: MIGRATION ONLY, no adaptive spinning.
#
# Differs from p78_arm.sh (which armed the FULL stack at spin_mode 2):
#   spin_mode 1  -> kernel tier1/tier2 OFF.  Migration is the only mechanism.
#   NHextend additionally needs IVH_AFL_DISABLE=1 (its adaptive lock is
#   userspace and no sysctl touches it) -- the runner sets that.
#
# preempt_event_source=2 is NOT optional: at 0 Gate 2 reads a dead paravirt
# field and migration never fires at all.  Asserted below.
set -u
S=/proc/sys/kernel
A="$1"

if [ "$A" = pv ]; then bash /root/ivh_tools/pvbase.sh >/dev/null || exit 1; exit 0; fi

/root/spin_mode 1 >/dev/null || { echo "FATAL: spin_mode 1 failed"; exit 1; }
# G-LOCK-39 steal-estimator calibration. These do NOT persist across reboot
# and boot defaults flatten the estimator -- nhextend_full_best_config.sh sets
# them and p7v2_arm.sh did not, so every arm before 2026-10-01 ran with
# deadband=1000/phase_pct=0 instead of 50000/100.
# ivh_rcu_guard: boot default is 1, which BLOCKS migration whenever
# rcu_preempt_depth()>0. NHextend's threads are in RCU read-side sections at
# the pre-lock syscall, so guard=1 cost 3/4 of the preemption reduction:
# measured 2026-10-01, preempted CS 20.6% -> 16.6% at guard=1 vs 20.6% -> 5.3%
# at guard=0, and throughput +4.6% vs +14.9%. p7v2_arm.sh never set it.
echo 0 > $S/ivh_rcu_guard
# CORRECTED 2026-10-01: deadband=1000 / phase_pct=0, per cvm_setup/goto_mode.sh
# :80,:122 and its check_cal assert :292-294, validated 2026-09-22/23 against a
# SCHED_FIFO wall-clock prober. The 50000/100 pair in
# nhextend_full_best_config.sh is SUPERSEDED: at deadband=50000 the estimator
# reads kernel/truth = 0.00 for fine-grained steal (blind), and phase_pct=100
# adds a whole tick to every booked coarse event (~20x inflation).
echo 1000 > $S/ivh_tks_deadband_ns
echo 0    > $S/ivh_tks_phase_pct
echo 0     > $S/ivh_tks_idle_sub
echo 8     > $S/ivh_tks_carry_ticks
echo 2 > $S/ivh_pv_preempt_src
echo 2 > $S/ivh_preempt_event_source
echo 0 > $S/ivh_migrate_mechanism
echo 8 > $S/ivh_max_concurrent
echo 1 > $S/ivh_selection_trylock
echo 1010 > $S/ivh_capacity_threshold
echo 1 > $S/ivh_cs_track_enabled          # Gate 2's CS term; must match pvbase
echo 1 > $S/ivh_slowpath_wait_measure     # method B input
echo 1 > $S/ivh_cap_writer; echo 1 > $S/ivh_act_writer   # vcap capacity + active
echo 2 > $S/ivh_time_left_source                          # EWMA
echo 16000000000 > $S/ivh_ucw_max_age_ns                  # > vcap's 5.2s loop
echo "$A" > $S/ivh_time_left_threshold_ns                 # swept value LAST
echo 1 > $S/ivh_universal_eligible

chk(){ [ "$(cat $S/$1)" = "$2" ] || { echo "FATAL: $1 is $(cat $S/$1), want $2"; exit 1; }; }
chk ivh_rcu_guard 0
chk ivh_adaptive_mode 0
chk ivh_universal_eligible 1
chk ivh_preempt_event_source 2
chk ivh_cap_writer 1
chk ivh_act_writer 1
chk ivh_time_left_source 2
chk ivh_time_left_threshold_ns "$A"
