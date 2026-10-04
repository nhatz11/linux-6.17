#!/bin/bash
# TEST 2 -- lock-HOLDER preemption detection.
#
# The claim: when the queue head marks the holder preempted, it really is.
#
# NOT tested by "flagged holders have p99 CS lengths" -- criterion 0 fires on
# `held > ivh_cs_tick_period * ivh_cs_owed_ticks` (2 ms), so a long hold is
# guaranteed by construction. That restates the gate.
#
# Tested instead by SEPARABILITY, as ivh_tsc_beat.h:1430 specifies:
#   ivh_cs_prev_hold_hist -- ALL contended holds        (denominator)
#   ivh_cs_ep_hist        -- the FIRED population       (numerator)
# If fired sits inside the bulk of normal holds we are firing on ordinary slow
# code; if it sits above p99.9 we are firing on anomalies.
#
# Config notes:
#  - criterion MUST be 0. Criterion 1 (what spin_mode 6/7 set) never reads the
#    heartbeat; ivh_cs_healthy_long is structurally 0 there and it makes no
#    preemption claim at all (source: qspinlock_paravirt.h:830-832).
#  - ivh_cs_owner_clear MUST be 0 or prev_hold_hist is never recorded
#    (qspinlock_paravirt.h:3696) -- the denominator would be empty.
#  - ivh_pv_rot_enable MUST be 0, same gate.
set -u
S=/proc/sys/kernel
source /root/ivh_tools/bench_guard.sh
echo 0 > $S/ivh_universal_eligible   # MIGRATION OFF -- it prevents the very
                                    # preemption this test must observe
/root/spin_mode 2 >/dev/null 2>&1  # keep IVH lock path (cs tracking lives there)
echo 2 > $S/ivh_pv_preempt_src
echo 1 > $S/ivh_cs_track_enabled
echo 0 > $S/ivh_cs_criterion
echo 1 > $S/ivh_cs_owner_enable
echo 0 > $S/ivh_cs_owner_clear
echo 1 > $S/ivh_cs_scan
echo 1 > $S/ivh_cs_head_probe
echo 0 > $S/ivh_pv_rot_enable
echo "config:"
printf "  %-24s %s\n" ivh_universal_eligible "$(cat $S/ivh_universal_eligible)  <- MUST be 0"
for f in ivh_cs_criterion ivh_cs_owner_enable ivh_cs_owner_clear ivh_cs_scan \
         ivh_cs_head_probe ivh_pv_rot_enable ivh_cs_owed_ticks ivh_cs_tick_period; do
  printf "  %-24s %s\n" $f "$(cat $S/$f)"; done
echo "  fire threshold = owed_ticks x tick_period = $(( $(cat $S/ivh_cs_owed_ticks) * $(cat $S/ivh_cs_tick_period) / 2200 )) us"
echo
echo "=== hackbench 90s, contended half ==="
timeout 150 hackbench -T -g1 -f8 -l500000 >/dev/null 2>&1
echo
python3 /root/ivh_tools/read_ivh_counters.py ivh_cs_check_calls ivh_cs_long_hold \
  ivh_cs_healthy_long ivh_cs_fired ivh_cs_abstain_nohz ivh_cs_abstain_retag \
  ivh_cs_abstain_young ivh_cs_stamps 2>/dev/null
echo TEST2-DONE
