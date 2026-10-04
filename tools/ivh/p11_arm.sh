#!/bin/bash
# p11_arm.sh <pv | STALENESS_US> -- point 11: ALL FOUR adaptive-spinning
# mechanisms on at best capability, with ONE staleness number.
#
# ------------------------------------------------------------------------------
# WHAT "ONE NUMBER" CAN AND CANNOT MEAN HERE (read before trusting a sweep)
#
# The four mechanisms do NOT read one signal. They read three:
#
#  A. PER-CPU TSC BEAT          -> tier 2, head bypass      knob: ivh_pv_beat_threshold
#     Only GUARANTEED publisher is account_process_tick() at HZ=1000. Measured
#     on this host (kvm.c:1518, 400 samples, idle, 16 vCPUs): staleness
#     p50 251 us, p99 **3243 us**. Below that floor it misfires on LIVE vCPUs:
#     G-LOCK-44 measured 178,576 tier-2 fires per 13 s run, mean consequent halt
#     14.2 us against ~250 us real preemptions. Wants the threshold HIGH (5 ms).
#
#  B. PER-NODE STAMP            -> eviction / lock skipping  knob: ivh_pv_evict_threshold
#     Published by ivh_node_publish_in_spin() every ivh_pv_beat_publish_mask+1
#     = 4096 spin iterations ~= 47-110 us. Floor is 30-60x lower than A. Wants
#     the threshold LOW or it never fires: measured 1001 evictions/run at 500 us
#     and **exactly 0 at 5 ms** (kvm.c:1524-1527).
#
#  C. HOLDER CS HOLD DURATION   -> head early halt (is_cs_preempted)
#     Gate is ivh_cs_owed_ticks x ivh_cs_tick_period (2 x 2.2e6 cyc = 2 ms),
#     plus ivh_cs_noise_cycles (10 us) and ivh_cs_prompt_cycles (9 us). This is
#     a HOLD-duration gate, not a staleness gate.
#
#  TIER 1 has NO time threshold at all: it reads `prev->state != VCPU_RUNNING`,
#  a boolean the hypervisor maintains (qspinlock_paravirt.h:1370).
#  ivh_pv_tier1_halt_min (default 0, cycles) is an OPTIONAL halt-duration gate
#  added by G-LOCK-44; its budget is ivh_pv_spin_threshold, in ITERATIONS.
#
# kvm.c:1528 states the conclusion outright: "One knob cannot satisfy both."
# This script therefore writes the SAME number to A and B so the sweep the user
# asked for is runnable, and PRINTS each signal's own floor so the arms that sit
# inside a misfire zone are visible instead of silent. Over 50 us - 1.5 ms,
# EVERY arm is below A's 3243 us p99 floor.
# ------------------------------------------------------------------------------
set -u
S=/proc/sys/kernel
A="${1:?usage: p11_arm.sh <pv|STALENESS_US>}"
if [ "$A" = pv ]; then bash /root/ivh_tools/pvbase.sh; exit $?; fi

KHZ=$(grep -oP 'tsc: Detected \K[0-9.]+' <(dmesg 2>/dev/null) | head -1)
MHZ=${KHZ:-2200.0}
CYC=$(python3 -c "print(int(round($A * $MHZ)))")      # us -> TSC cycles
EVICT_MIN=22000; EVICT_MAX=22000000
[ "$CYC" -lt "$EVICT_MIN" ] && { echo "FATAL: ${A}us = $CYC cyc is below the evict clamp ($EVICT_MIN = 10us)"; exit 1; }
[ "$CYC" -gt "$EVICT_MAX" ] && { echo "FATAL: ${A}us = $CYC cyc is above the evict clamp ($EVICT_MAX = 10ms)"; exit 1; }

/root/spin_mode 2 >/dev/null || { echo "FATAL: spin_mode 2 failed"; exit 1; }
# --- migration (so the arm matches points 7/8) ---
echo 2 > $S/ivh_pv_preempt_src;        echo 2 > $S/ivh_preempt_event_source
echo 1 > $S/ivh_universal_eligible;    echo 0 > $S/ivh_migrate_mechanism
echo 0 > $S/ivh_rcu_guard
echo 2500000 > $S/ivh_time_left_threshold_ns; echo 8 > $S/ivh_max_concurrent
echo 1 > $S/ivh_selection_trylock;     echo 1010 > $S/ivh_capacity_threshold
echo 1 > $S/ivh_cap_writer; echo 1 > $S/ivh_act_writer; echo 2 > $S/ivh_time_left_source
echo 16000000000 > $S/ivh_ucw_max_age_ns
# --- MASK VALIDATION (audit 2026-10-03) ---
# ivh_pv_proc_beat_publish_mask (kvm.c:2303) REFUSES val < 0xff or not 2^n-1 with
# -EINVAL, and proc_doulongvec_minmax rejects rather than clamps. The write then
# silently no-ops and the arm runs at the PREVIOUS mask under a new label.
MASK="${IVH_MASK:-4095}"
case "$MASK" in 255|511|1023|2047|4095|8191|16383) ;; *)
  echo "FATAL[p11]: IVH_MASK=$MASK invalid -- kernel needs 2^n-1 and >= 255"; exit 1;; esac

# --- TIER 1: boolean prev->state, no time threshold. halt_min 0 = stock. ---
echo 1 > $S/ivh_pv_tier1_enable;       echo 32768 > $S/ivh_pv_spin_threshold
# --- TIER 1 HALT-DURATION GATE (G-LOCK-44), now ON by default -------------
# tier 1 fires on `prev->state != VCPU_RUNNING`, which VCPU_HALTED satisfies --
# and a waiter sets VCPU_HALTED on ITSELF before halting (qspinlock_paravirt.h
# :2236). So an ordinary queue halt recruits its successor, which recruits the
# next: a cascade. The file measures the damage at :1376-1381 -- 72.56% of
# tier-1 fires land on a predecessor halted only 3.7-7.4 us, one about to hand
# over the lock -- and :1407-1414 records ~2.77 tier-1 fires per tier-2 fire as
# the inference walks down the queue tail.
#
# halt_min gates the bail on how long prev has ACTUALLY been down (:1401, fed by
# prev stamping itself at :2234, which only happens when this knob is non-zero).
# It shipped at 0 and was therefore dead in every arm this project ever ran --
# ivh_tier1_halt_fresh read 0 for an entire boot.
#
# MEASURED, hackbench, n=10 balanced paired (haltmin_1004-042537.tsv):
#        halt_min=0      t1/ent 18.45%  haltrate 26.65%  vs PV wait +30.30% 8/10
#        halt_min=22000  t1/ent  1.80%  haltrate 12.11%  vs PV wait +32.27% 9/10
#                        12,089,559 suppressions; tier 1 back to PV's own 1.54%
# The DIRECT paired comparison is NULL: -2.06% +/- 11.52 se, 5/10. So this is
# adopted for NON-INFERIORITY plus mechanism -- it removes a documented
# false-positive cascade at no measured cost -- NOT as a throughput win. Do not
# cite it as one. IVH_HALT_MIN=0 restores the old behaviour for an A/B.
echo "${IVH_HALT_MIN:-22000}" > $S/ivh_pv_tier1_halt_min
echo 0 > $S/ivh_pv_trylock_relaxed
# --- TIER 2: reads signal A ---
echo 1 > $S/ivh_pv_tier2_enable
# --- HEAD BYPASS: also reads signal A. probe=1 is REQUIRED; enable=1 fires 0. ---
echo 1 > $S/ivh_head_bypass_enable;    echo 1 > $S/ivh_head_bypass_probe
echo 1 > $S/ivh_head_bypass_runs;      echo 0 > $S/ivh_head_bypass_hold
echo 4 > $S/ivh_head_bypass_max
# --- HEAD EARLY HALT (is_cs_preempted): THE THIRD TSC THRESHOLD ---
# CORRECTED 2026-10-03 by audit. The previous comment here was WRONG: it claimed
# the gate is ivh_cs_tick_period x ivh_cs_owed_ticks (:875 / :929). Those two
# sites are UNREACHABLE at ivh_cs_criterion=1, which is what we set below --
# is_cs_preempted() returns at :841 on
#       held <= o->last_cs + ivh_cs_noise_cycles
# so ivh_cs_noise_cycles IS the HEH threshold, and tick_period is dead code here.
# It was never written by this script and was found live at 550000 (250us), a
# leftover from spinsweep.sh, i.e. HEH ran at a CONSTANT 250us in every arm while
# the "swept" knob did nothing. tick_period is still set for the criterion=0 case
# but noise_cycles is the one that matters.
echo 1 > $S/ivh_cs_track_enabled; echo 1 > $S/ivh_cs_owner_enable
echo 1 > $S/ivh_cs_owner_clear;   echo 0 > $S/ivh_cs_owner_fast
echo 1 > $S/ivh_cs_scan;          echo 1 > $S/ivh_cs_criterion
echo 1 > $S/ivh_cs_head_probe;    echo 1 > $S/ivh_cs_head_bail
echo 1 > $S/ivh_cs_owed_ticks     # only meaningful at criterion=0
# --- LOCK SKIPPING: reads signal B. Best combo per ivh_skip_combo_result. ---
echo 1 > $S/ivh_pv_evict_enable;    echo 1 > $S/ivh_pv_evict_node_stamp
echo 1 > $S/ivh_pv_evict_lookahead; echo 1 > $S/ivh_pv_requeue_nosteal
echo 2 > $S/ivh_pv_evict_hop_cap;   echo 4 > $S/ivh_pv_requeue_max
echo 0 > $S/ivh_pv_skip_point
echo 1 > $S/ivh_slowpath_wait_measure
# --- THE SWEPT VALUE, written LAST to both staleness knobs ---
echo "$CYC" > $S/ivh_pv_beat_threshold      # tier2 + head bypass
echo "$CYC" > $S/ivh_pv_evict_threshold     # lock skipping
echo "$CYC" > $S/ivh_cs_tick_period         # criterion=0 path only (dead at criterion=1)
echo "$CYC" > $S/ivh_cs_noise_cycles        # THE REAL head-early-halt threshold
# Eviction's "now" must be rdtsc, not the holder's last published beat. At
# cheap_now=1 (the default) ivh_beat_now_cheap() returns a stamp that lags by up
# to one publish interval, so the effective eviction threshold becomes
# T + U(0, I_head) -- the two swept knobs confounded through one clock.
echo 0 > $S/ivh_pv_evict_cheap_now
# Publish cadence. Lower mask = the stamp/beat is refreshed MORE often, so the
# staleness signal is sharper and a sub-1ms threshold yields fewer FALSE
# positives. 4095 (default) = every 4096 spin iters ~= 50us; 255 = ~3us.
# This is the lever for making a small threshold precise instead of trigger-happy.
echo "$MASK" > $S/ivh_pv_beat_publish_mask

chk(){ [ "$(cat $S/$1)" = "$2" ] || { echo "FATAL[p11 $A]: $1 is $(cat $S/$1) want $2"; exit 1; }; }
chk ivh_adaptive_mode 2;        chk ivh_universal_eligible 1
chk ivh_pv_preempt_src 2;       chk ivh_preempt_event_source 2
chk ivh_pv_tier1_enable 1;      chk ivh_pv_spin_threshold 32768
chk ivh_pv_tier1_halt_min "${IVH_HALT_MIN:-22000}"   # dead at 0; see the block above
chk ivh_pv_tier2_enable 1
chk ivh_head_bypass_enable 1;   chk ivh_head_bypass_probe 1
chk ivh_cs_head_bail 1;         chk ivh_cs_head_probe 1
chk ivh_pv_evict_enable 1;      chk ivh_pv_evict_node_stamp 1
chk ivh_pv_evict_lookahead 1;   chk ivh_pv_requeue_nosteal 1
chk ivh_pv_beat_threshold "$CYC"
chk ivh_pv_evict_threshold "$CYC"
chk ivh_cs_tick_period "$CYC";  chk ivh_cs_owed_ticks 1
chk ivh_cs_noise_cycles "$CYC"      # the REAL HEH gate at criterion=1
chk ivh_cs_criterion 1
chk ivh_pv_evict_cheap_now 0
chk ivh_pv_beat_publish_mask "$MASK" # kernel silently refuses bad masks
chk ivh_pv_evict_hop_cap 2;     chk ivh_pv_requeue_max 4
echo "p11 arm: staleness=${A}us = $CYC cyc @ ${MHZ}MHz -> beat_threshold AND evict_threshold"
python3 - "$A" "$MASK" <<'EOF'
import sys
T=float(sys.argv[1]); M=int(sys.argv[2]); N=M+1
# measured per-iteration costs (ivh_spin_budget_duration_method_2026-10-02.md)
In=N*27.7/2200.0; Ih=N*106.1/2200.0      # publish interval, node loop / head loop
wn, wh = 412.0, 1580.0                   # spin budget per attempt, node / head
print(f"  publish interval @mask {M}: node {In:.1f}us  head {Ih:.1f}us")
print(f"  tier2/bypass FALSE-POSITIVE if T < I_head({Ih:.1f}us): "
      + ("*** YES, T=%.0fus IS IN THE MISFIRE ZONE ***" % T if T < Ih else "no"))
print(f"  tier2 UNREACHABLE if T > w_node({wn:.0f}us): "
      + ("*** YES, tier2 can barely fire ***" if T > wn else "no"))
print(f"  eviction fire rate peaks at T ~ I_node({In:.1f}us); T/I = {T/In:.0f}x "
      + ("(eviction near-dead)" if T/In > 20 else "(eviction live)"))
EOF
