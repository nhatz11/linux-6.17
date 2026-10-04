#!/bin/bash
# p78_arm.sh <pv | tlt <ns> | mc <n>>  -- arm setter for points 7 and 8.
#
# Non-pv arms are the FULL STACK: migration + tier1 + HEH + tier2 + skip + head
# bypass. That is ladder arm 5 (mig_t1_heh_t2_sk_hb), the configuration the
# 2026-09-29 ladder verdict selected. See eval_final.md Appendix A.
#
# The SWEPT value is written LAST and asserted, because spin_mode and the
# feature block both overwrite unrelated knobs and an earlier write would be
# silently reverted -- all six arms would then run identically with no error.
set -u
S=/proc/sys/kernel
MODE="$1"; VAL="${2:-}"

if [ "$MODE" = pv ]; then bash /root/ivh_tools/pvbase.sh; exit $?; fi

/root/spin_mode 2 >/dev/null || { echo "FATAL: spin_mode 2 failed"; exit 1; }
# migration
echo 2 > $S/ivh_pv_preempt_src;        echo 2 > $S/ivh_preempt_event_source
echo 1 > $S/ivh_universal_eligible;    echo 0 > $S/ivh_migrate_mechanism
# tier1 + spin budget
echo 1 > $S/ivh_pv_tier1_enable;       echo 32768 > $S/ivh_pv_spin_threshold
echo 0 > $S/ivh_pv_tier1_halt_min;     echo 0 > $S/ivh_pv_trylock_relaxed
# tier2 (shares beat_threshold with head bypass; 5ms shipped value fires ZERO)
echo 2200000 > $S/ivh_pv_beat_threshold; echo 1 > $S/ivh_pv_tier2_enable
# head early halt
echo 1 > $S/ivh_cs_track_enabled; echo 1 > $S/ivh_cs_owner_enable
echo 1 > $S/ivh_cs_owner_clear;   echo 0 > $S/ivh_cs_owner_fast
echo 1 > $S/ivh_cs_scan;          echo 1 > $S/ivh_cs_criterion
echo 1 > $S/ivh_cs_head_probe;    echo 1 > $S/ivh_cs_head_bail
# lock skipping
echo 1 > $S/ivh_pv_evict_enable;    echo 1 > $S/ivh_pv_evict_node_stamp
echo 1 > $S/ivh_pv_evict_lookahead; echo 1 > $S/ivh_pv_requeue_nosteal
echo 2 > $S/ivh_pv_evict_hop_cap;   echo 4 > $S/ivh_pv_requeue_max
echo 0 > $S/ivh_pv_skip_point;      echo 1100000 > $S/ivh_pv_evict_threshold
# head bypass -- probe=1 is REQUIRED; enable=1 alone fires zero
echo 1 > $S/ivh_head_bypass_enable; echo 1 > $S/ivh_head_bypass_probe
echo 1 > $S/ivh_head_bypass_runs;   echo 0 > $S/ivh_head_bypass_hold
echo 4 > $S/ivh_head_bypass_max
echo 1 > $S/ivh_slowpath_wait_measure
# 2026-09-30, user decision: RCU migration ENABLED by default. G-LOCK-40's guard
# costs ebizzy 23.2pp (+54.91% -> +78.12%, n=5) because 91.9% of its migration
# candidates sit inside an RCU reader. Setting 0 restores pre-G-LOCK-40
# behaviour. RISK, stated once and accepted: this permits GFP_KERNEL allocation
# and wait_for_completion() inside a preemptible-RCU reader, extending the grace
# period by the migration latency; DEBUG_ATOMIC_SLEEP and PROVE_LOCKING are off
# so nothing warns, and the failure mode is RCU stalls under memory pressure.
[ -e $S/ivh_rcu_guard ] && echo 0 > $S/ivh_rcu_guard

# defaults for the knob NOT being swept, then the swept value LAST
TLT=4000000; MC=8
case "$MODE" in
  tlt) TLT="$VAL" ;;
  mc)  MC="$VAL" ;;
  *) echo "FATAL: unknown mode $MODE"; exit 1 ;;
esac
echo "$TLT" > $S/ivh_time_left_threshold_ns
echo "$MC"  > $S/ivh_max_concurrent

chk(){ [ "$(cat $S/$1)" = "$2" ] || { echo "FATAL[$MODE $VAL]: $1 is $(cat $S/$1) want $2"; exit 1; }; }
chk ivh_adaptive_mode 2;            chk ivh_universal_eligible 1
chk ivh_preempt_event_source 2;     chk ivh_pv_preempt_src 2
chk ivh_migrate_mechanism 0
chk ivh_pv_tier1_enable 1;          chk ivh_pv_spin_threshold 32768
chk ivh_pv_tier1_halt_min 0;        chk ivh_pv_trylock_relaxed 0
chk ivh_pv_tier2_enable 1;          chk ivh_pv_beat_threshold 2200000
chk ivh_cs_track_enabled 1;         chk ivh_cs_head_bail 1
chk ivh_cs_head_probe 1
chk ivh_pv_evict_enable 1;          chk ivh_pv_evict_node_stamp 1
chk ivh_pv_skip_point 0
chk ivh_head_bypass_enable 1;       chk ivh_head_bypass_probe 1
chk ivh_slowpath_wait_measure 1
chk ivh_time_left_threshold_ns "$TLT"
chk ivh_max_concurrent "$MC"
echo "ARM $MODE${VAL:+=$VAL} applied and asserted (24 knobs; tlt=$TLT mc=$MC)"
