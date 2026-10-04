#!/bin/bash
# G-LOCK-41 verdict audit: are the LH/LW "preempted" verdicts correct?
# Ground truth = ivh_vact_preempt_since(), the tick-driven TSC gap detector,
# host-validated per-event in evaluation.md 12 (105.7% recall >1.5ms).
# Migration OFF in both: it prevents the preemption we need to observe.
set -u
S=/proc/sys/kernel
source /root/ivh_tools/bench_guard.sh
echo 0 > $S/ivh_universal_eligible
/root/spin_mode 2 >/dev/null 2>&1
echo 2 > $S/ivh_pv_preempt_src
echo 1 > $S/ivh_cs_verdict
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
echo "capacity at start: $(cap)"

echo; echo "########## TEST 2 -- lock HOLDER (full 2x2) ##########"
echo 1 > $S/ivh_cs_track_enabled; echo 0 > $S/ivh_cs_criterion
echo 1 > $S/ivh_cs_owner_enable;  echo 1 > $S/ivh_cs_owner_clear   # hook lives inside the clear
echo 1 > $S/ivh_cs_head_probe;    echo 0 > $S/ivh_pv_rot_enable
echo 0 > $S/ivh_pv_evict_enable
printf "  criterion=%s owner_clear=%s head_probe=%s\n" \
  "$(cat $S/ivh_cs_criterion)" "$(cat $S/ivh_cs_owner_clear)" "$(cat $S/ivh_cs_head_probe)"
timeout 200 hackbench -T -g1 -f8 -l600000 >/dev/null 2>&1
python3 read_ivh_counters.py ivh_cs_fired ivh_cs_long_hold ivh_cs_healthy_long \
  ivh_cs_clears ivh_cs_v_flagged ivh_cs_v_unflagged 2>/dev/null
echo "capacity after T2: $(cap)"

echo; echo "########## TEST 3 -- lock WAITER (precision column) ##########"
echo 0 > $S/ivh_cs_owner_clear
echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_gap_hist
echo 1 > $S/ivh_pv_evict_age_hist; echo 1 > $S/ivh_pv_evict_node_stamp
echo 1 > $S/ivh_pv_evict_lookahead; echo 1 > $S/ivh_pv_requeue_nosteal
echo 2 > $S/ivh_pv_evict_hop_cap
timeout 200 hackbench -T -g1 -f8 -l600000 >/dev/null 2>&1
python3 read_ivh_counters.py ivh_evict_requeued ivh_evict_v ivh_evict_gap_hist 2>/dev/null | head -6
echo "capacity after T3: $(cap)"
echo G41-DONE
