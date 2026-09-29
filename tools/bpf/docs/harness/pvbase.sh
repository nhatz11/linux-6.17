#!/bin/bash
# pvbase.sh -- enter a GENUINE stock-PV baseline.
#
# `spin_mode 1` alone is NOT sufficient. ivh_head_bypass_probe is read inside
# pv_wait_node() with no ivh_adaptive_mode gate, so a bypass arm's leftover
# probe=1 keeps firing at adaptive_mode=0. Measured 2026-09-29: 13 bypass fires
# on one hackbench run in a "stock PV" arm entered via spin_mode 1 alone; 0 with
# the probe explicitly cleared. Every IVH feature is therefore zeroed by hand
# and asserted.
set -u
S=/proc/sys/kernel
/root/spin_mode 1 >/dev/null 2>&1
for k in ivh_universal_eligible ivh_pv_tier2_enable \
         ivh_head_bypass_enable ivh_head_bypass_probe ivh_head_bypass_runs \
         ivh_pv_evict_enable ivh_pv_evict_node_stamp ivh_pv_evict_lookahead \
         ivh_pv_requeue_nosteal \
         ivh_cs_track_enabled ivh_cs_owner_enable ivh_cs_owner_clear \
         ivh_cs_owner_fast ivh_cs_scan ivh_cs_criterion \
         ivh_cs_head_probe ivh_cs_head_bail; do
    [ -w $S/$k ] && echo 0 > $S/$k
done
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
echo "stock PV baseline applied and asserted"
