#!/bin/bash
# G-LOCK-43 verdict audit. Question: is the detector finding the RIGHT TARGET?
# Migration OFF throughout -- it prevents the preemption we must observe.
set -u
S=/proc/sys/kernel
rd(){ python3 /root/ivh_tools/read_ivh_counters.py "$@" 2>/dev/null; }
cap(){ python3 - <<'PY'
import sys; sys.path.insert(0,"/root/ivh_tools")
import read_vact_rq as r, statistics as st
sym=r.load_kallsyms(); cpus=r.online_cpus()
with open(r.KCORE,"rb") as f:
    ph=r.read_phdrs(f); offs=[r.read_u64(f,ph,sym["__per_cpu_offset"]+8*c) for c in cpus]
    v=[r.read_u64(f,ph,sym["runqueues"]+3824+o) for o in offs]
print(f"{round(st.mean(v[:8]))}/{round(st.mean(v[8:]))}")
PY
}
echo 0 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1
echo 2 > $S/ivh_pv_preempt_src
echo "capacity: $(cap)  sampler_ns=$(cat $S/ivh_tks_sampler_ns)"

echo; echo "########## ZERO SANITY (verdict on, consumers off) ##########"
echo 1 > $S/ivh_cs_verdict; echo 0 > $S/ivh_cs_owner_clear; echo 0 > $S/ivh_pv_evict_enable
timeout 40 hackbench -T -g1 -f8 -l150000 >/dev/null 2>&1
rd ivh_cs_v_flagged ivh_cs_v_unflagged ivh_evict_v ivh_cs_v_orphan ivh_cs_v_nested ivh_cs_dep_clobbered | grep -E "NONE|orphan|nested|clobbered"

echo; echo "########## ARM A -- HOLDER, tick clock ##########"
echo 1 > $S/ivh_cs_track_enabled; echo 0 > $S/ivh_cs_criterion
echo 1 > $S/ivh_cs_owner_enable;  echo 1 > $S/ivh_cs_owner_clear
echo 1 > $S/ivh_cs_head_probe;    echo 0 > $S/ivh_pv_rot_enable
A=$(rd ivh_cs_ep_events_by_end ivh_cs_v_flagged ivh_cs_v_unflagged ivh_cs_v_orphan ivh_cs_v_nested ivh_cs_dep_clobbered)
timeout 220 hackbench -T -g1 -f8 -l700000 >/dev/null 2>&1
echo "--- AFTER:"; rd ivh_cs_ep_events_by_end ivh_cs_v_flagged ivh_cs_v_unflagged ivh_cs_v_orphan ivh_cs_v_nested ivh_cs_dep_clobbered
echo "capacity: $(cap)"

echo; echo "########## ARM B -- WAITER, sampler clock 200us/400us ##########"
echo 0 > $S/ivh_cs_owner_clear
echo 0 > $S/ivh_tks_phase_pct; echo 200000 > $S/ivh_tks_sampler_ns
echo 400000 > $S/ivh_vact_jump_ns
printf "  sampler_ns=%s jump_ns=%s  (lag = 400us + 200us = 600us)\n" \
  "$(cat $S/ivh_tks_sampler_ns)" "$(cat $S/ivh_vact_jump_ns)"
echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_gap_hist
echo 1 > $S/ivh_pv_evict_age_hist; echo 1 > $S/ivh_pv_evict_node_stamp
echo 1 > $S/ivh_pv_evict_lookahead; echo 1 > $S/ivh_pv_requeue_nosteal
echo 2 > $S/ivh_pv_evict_hop_cap
timeout 220 hackbench -T -g1 -f8 -l700000 >/dev/null 2>&1
rd ivh_evict_requeued ivh_evict_v
echo "capacity: $(cap)"
echo 0 > $S/ivh_tks_sampler_ns; echo 1500000 > $S/ivh_vact_jump_ns
echo G43-AUDIT-DONE
