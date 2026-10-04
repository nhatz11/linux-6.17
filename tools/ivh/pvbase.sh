#!/bin/bash
# pvbase.sh -- enter a GENUINE stock-PV baseline.
#
# `spin_mode 1` alone is NOT sufficient. Several ivh_ sysctls are read on paths
# with no ivh_adaptive_mode gate, so leftovers from a previous arm keep changing
# behaviour at adaptive_mode=0. Confirmed by audit 2026-09-29:
#   ivh_head_bypass_probe   qspinlock_paravirt.h:2010  (13 fires/run measured)
#   ivh_pv_tier1_halt_min   :1372,:2232,:3775  suppresses upstream's only early bail
#   ivh_pv_trylock_relaxed  :276   changes the head's acquire sequence
#   ivh_pv_skip_point       :3930  enables deferred promotion
#   ivh_pv_preempt_src      :1516,:1246,:1283,:1655  an rdtsc store per slowpath
#                                  entry -- the documented +11.44% "pedestal"
# Everything is zeroed by hand and ASSERTED. spin_mode's exit status is checked:
# its set_sysctl() exits 1 on a failed write and require_boot exits 1 on the
# wrong boot, and swallowing that gave a silently wrong arm.
#
# ivh_cs_track_enabled is set to 1, NOT 0, deliberately -- see A.3 in
# eval_final.md. cs_exit() is the only writer of current->last_cs_ns, which is
# Gate 2's critical-section term (fair.c:13829,13852). It must be live and it
# must be IDENTICAL in the baseline and every arm, or the baseline gets a free
# pass on the cs_enter/cs_exit cost that the arms pay on every outermost lock.
set -u
S=/proc/sys/kernel
/root/spin_mode 1 >/dev/null || { echo "FATAL: spin_mode 1 failed"; exit 1; }
for k in ivh_universal_eligible ivh_pv_tier2_enable \
         ivh_head_bypass_enable ivh_head_bypass_probe ivh_head_bypass_runs \
         ivh_pv_evict_enable ivh_pv_evict_node_stamp ivh_pv_evict_lookahead \
         ivh_pv_requeue_nosteal \
         ivh_cs_owner_enable ivh_cs_owner_clear ivh_cs_owner_fast \
         ivh_cs_scan ivh_cs_criterion ivh_cs_head_probe ivh_cs_head_bail \
         ivh_pv_tier1_halt_min ivh_pv_trylock_relaxed ivh_pv_skip_point \
         ivh_migrate_mechanism; do
    [ -w $S/$k ] && echo 0 > $S/$k
done
echo 1 > $S/ivh_cs_track_enabled          # Gate 2 input, symmetric with the arms
echo 1 > $S/ivh_slowpath_wait_measure     # measurement only, no behaviour
chk(){ [ "$(cat $S/$1)" = "$2" ] || { echo "FATAL[pvbase]: $1 is $(cat $S/$1) want $2"; exit 1; }; }
chk ivh_adaptive_mode 0
chk ivh_universal_eligible 0
chk ivh_pv_tier2_enable 0
chk ivh_head_bypass_probe 0
chk ivh_head_bypass_enable 0
chk ivh_pv_evict_enable 0
chk ivh_cs_head_bail 0
chk ivh_cs_head_probe 0
# the ungated set the audit found -- set by spin_mode but never asserted before
chk ivh_pv_preempt_src 0
chk ivh_pv_spin_threshold 32768
chk ivh_pv_tier1_enable 1
chk ivh_pv_tier1_halt_min 0
chk ivh_pv_trylock_relaxed 0
chk ivh_pv_skip_point 0
chk ivh_migrate_mechanism 0
chk ivh_cs_track_enabled 1
chk ivh_slowpath_wait_measure 1
echo "stock PV baseline applied and asserted (16 knobs)"
