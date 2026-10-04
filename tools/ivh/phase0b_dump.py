#!/usr/bin/env python3
"""Dump Phase 0/0b IVH counters as JSON for before/after delta math."""
import sys, json
sys.path.insert(0, "/root/ivh_tools")
import read_ivh_counters as R

SCALARS = ["ivh_rot_handoffs", "ivh_rot_preempted", "ivh_rot_no_live",
           "ivh_rot_tail_stop", "ivh_rot_idle_unknown", "ivh_rot_idle_backward",
           "ivh_rot_idle_capped", "ivh_rot_steals", "ivh_halt_from_node",
           "ivh_beat_tier2_fired",
           # is_cs_preempted() Stage A
           "ivh_cs_stamps", "ivh_cs_clears", "ivh_cs_stamp_overwrote",
           "ivh_cs_check_calls", "ivh_cs_abstain_noprev", "ivh_cs_abstain_rot",
           "ivh_cs_abstain_tag", "ivh_cs_abstain_skew", "ivh_cs_abstain_young",
           "ivh_cs_abstain_nohz", "ivh_cs_long_hold", "ivh_cs_healthy_long",
           "ivh_cs_fired", "ivh_cs_ep_events", "ivh_cs_abstain_tenure",
           "ivh_cs_abstain_hashed", "ivh_cs_abstain_late", "ivh_cs_abstain_retag",
           "ivh_cs_tenure0_enter", "ivh_cs_tenure0_hashed",
           "ivh_cs_tenure0_hashed_released", "ivh_cs_tenure0_late",
           "ivh_cs_shadow_gate_pass_released",
           # is_cs_preempted() Stage B
           "ivh_cs_head_bailed", "ivh_head_spin_iters_bail_sum",
           "ivh_head_spin_attempts", "ivh_head_spin_iters_sum", "ivh_halt_from_head",
           "ivh_cs_fast_lookup_hit", "ivh_cs_fast_lookup_miss",
           "ivh_rot_stop_halted", "ivh_cs_scan_hit", "ivh_cs_scan_miss",
           "ivh_cs_abstain_nolastcs", "ivh_cs_bail_suppressed",
           "ivh_rot_splice_ok", "ivh_rot_splice_done", "ivh_rot_splice_blocked_tail", "ivh_rot_splice_blocked_starve",
           "ivh_head_spin_bail_attempts"]
ARRAYS = {n: R.ARRAY_COUNTERS[n] for n in
          ("ivh_rot_depth_hist", "ivh_rot_idle_hist",
           "ivh_rot_idle_cycles", "ivh_rot_idle_events",
           "ivh_node_halt_hist",
           # is_cs_preempted() Stage A
           "ivh_cs_ep_events_by_end", "ivh_cs_ep_cycles", "ivh_cs_ep_hist",
           "ivh_cs_tenure_cycles", "ivh_cs_tenure_hist",
           "ivh_cs_prev_hold_hist", "ivh_cs_prompt_hist",
           # is_cs_preempted() Stage B
           "ivh_head_halt_cycles", "ivh_head_halt_events",
           "ivh_head_halt_hist")}

sym = R.load_kallsyms(); cpus = R.online_cpus(); out = {}
with open(R.KCORE, "rb") as f:
    ph = R.read_phdrs(f)
    offs = [R.read_u64(f, ph, sym["__per_cpu_offset"] + 8 * c) for c in cpus]
    for n in SCALARS:
        out[n] = sum(R.read_u64(f, ph, sym[n] + o) for o in offs) if n in sym else None
    for n, shape in ARRAYS.items():
        out[n] = R.read_array_counter(f, ph, sym[n], offs, shape) if n in sym else None
json.dump(out, open(sys.argv[1], "w"))
