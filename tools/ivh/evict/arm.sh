#!/bin/bash
# Arms for the G-LOCK-33 eviction A/B. Sysctls only, no reboot between arms.
set -u; S=/proc/sys/kernel
w() { echo "$2" > $S/$1 2>/dev/null || echo "  (FAILED $1=$2)"; }
common() {   # IVH node spinning, tier1+exhaustion, NO tier2, all is_cs_preempted head knobs off
  w ivh_pv_evict_enable 0; w ivh_pv_rot_enable 0
  w ivh_cs_head_bail 0; w ivh_cs_head_probe 0; w ivh_cs_scan 0
  w ivh_cs_owner_fast 0; w ivh_cs_owner_clear 0; w ivh_cs_owner_enable 0; w ivh_cs_criterion 0
  w ivh_pv_skip_point 0
  w ivh_adaptive_mode 2; w ivh_pv_tier1_enable 1; w ivh_pv_spin_threshold 32768
  w ivh_pv_preempt_src 2; w ivh_pv_beat_threshold 220000; w ivh_pv_beat_publish_mask 4095
  w ivh_pv_irqoff_halt 0; w ivh_adaptive_irqoff_bail_gate 0
  w ivh_pv_tier2_enable 0     # tier2 and eviction are ANTAGONISTIC: tier2 halts the
}                             # very waiters eviction is allowed to act on.
case "$1" in
  pv)      /root/spin_mode 1 >/dev/null 2>&1 ;;
  control) common ;;
  # Isolates the HEARTBEAT TAX: identical to control except preempt_src=0, which
  # switches off ivh_tsc_beat_publish() in pv_init_node() -- an rdtsc plus a
  # store to a remotely-read cacheline on EVERY contended queue entry, paid by
  # 100% of acquisitions while eviction acts on 0.05-0.46% of them. PV skips it
  # entirely. control_nobeat vs control = the tax; control_nobeat vs pv = what
  # adaptive_mode=2 + tier1 cost on their own. Eviction cannot run here (its
  # gate requires preempt_src=2), which is exactly the point.
  control_nobeat) common; w ivh_pv_preempt_src 0 ;;
  evict)   common; w ivh_pv_evict_hop_cap "${HOPCAP:-1}"; w ivh_pv_requeue_max "${REQMAX:-1}"; w ivh_pv_evict_enable 1 ;;
  # Eviction with tier 1 OFF. Tier 1 halts a waiter whose PREDECESSOR is not
  # VCPU_RUNNING, which cascades sleep down the queue -- and eviction may never
  # pass a halted waiter. Measured: stop_halted 10,732 vs acted 6,769, i.e. more
  # opportunities are lost to sleepers than are ever acted on. Non-head waiters
  # then halt only on threshold exhaustion.
  # THE CONTROL that separates the two effects: tier 1 off, eviction OFF.
  # If this alone matches evict_nt1, the win is cascade-removal and eviction
  # contributes nothing.
  nt1_only) common; w ivh_pv_tier1_enable 0 ;;
  evict_nt1) common; w ivh_pv_tier1_enable 0; w ivh_pv_evict_hop_cap "${HOPCAP:-1}"; w ivh_pv_requeue_max "${REQMAX:-1}"; w ivh_pv_evict_enable 1 ;;
  *) echo "usage: $0 <pv|control|evict>" >&2; exit 1 ;;
esac
# verify the arm actually took
M=$(cat $S/ivh_adaptive_mode); E=$(cat $S/ivh_pv_evict_enable); T=$(cat $S/ivh_pv_tier2_enable)
case "$1" in
  pv)      [ "$M" = 0 ] && [ "$E" = 0 ] || { echo "ARM FAIL pv"; exit 1; } ;;
  control) [ "$M" = 2 ] && [ "$E" = 0 ] && [ "$T" = 0 ] || { echo "ARM FAIL control"; exit 1; } ;;
  control_nobeat) [ "$M" = 2 ] && [ "$E" = 0 ] && [ "$(cat $S/ivh_pv_preempt_src)" = 0 ] || { echo "ARM FAIL control_nobeat"; exit 1; } ;;
  evict)   [ "$M" = 2 ] && [ "$E" = 1 ] && [ "$T" = 0 ] && [ "$(cat $S/ivh_pv_tier1_enable)" = 1 ] || { echo "ARM FAIL evict"; exit 1; } ;;
  nt1_only) [ "$M" = 2 ] && [ "$E" = 0 ] && [ "$(cat $S/ivh_pv_tier1_enable)" = 0 ] || { echo "ARM FAIL nt1_only"; exit 1; } ;;
  evict_nt1) [ "$M" = 2 ] && [ "$E" = 1 ] && [ "$T" = 0 ] && [ "$(cat $S/ivh_pv_tier1_enable)" = 0 ] || { echo "ARM FAIL evict_nt1"; exit 1; } ;;
esac
