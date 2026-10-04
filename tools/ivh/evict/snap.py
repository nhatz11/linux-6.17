#!/usr/bin/env python3
# One flat CSV line of the counters the eviction dissection needs.
import subprocess,sys,re
SIMPLE=["ivh_node_spin_iters_sum","ivh_node_spin_attempts",
        "ivh_node_spin_success_iters_sum","ivh_node_spin_success_attempts",
        "ivh_evict_marked","ivh_evict_requeued","ivh_evict_steal_ok",
        "ivh_evict_ok_while_skipped","ivh_evict_hop_cap","ivh_evict_halt_race",
        "ivh_head_spin_enter","ivh_head_arm","ivh_halt_from_head",
        "ivh_head_foreign_hash","ivh_hash_dup_head","ivh_hash_dup_kick",
        "ivh_xchg_tail_calls","ivh_xchg_tail_nonempty",
        "ivh_requeue_none_won","ivh_requeue_none_fellback",
        "ivh_camp_entries","ivh_camp_trips","ivh_camp_exit_win",
        "ivh_camp_exit_empty","ivh_camp_exit_pending",
        "ivh_evict_lookahead_refused","ivh_evict_promo_unknown"]
ARR=[("ivh_node_halt_events","TOTAL"),("ivh_node_halt_cycles","TOTAL"),
     ("ivh_head_halt_events","TOTAL"),("ivh_head_halt_cycles","TOTAL")]
names=SIMPLE+sorted({a for a,_ in ARR})
out=subprocess.run(["python3","/root/ivh_tools/read_ivh_counters.py"]+names,
                   capture_output=True,text=True,timeout=120).stdout
vals={}
for ln in out.splitlines():
    m=re.match(r'\s*(\S+)\s*\[\s*(\S+)\s*\]\s*=\s*(\d+)',ln)
    if m: vals[(m.group(1),m.group(2))]=int(m.group(3)); continue
    m=re.match(r'\s*(\S+)\s*=\s*(\d+)',ln)
    if m: vals[m.group(1)]=int(m.group(2))
row=[str(vals.get(k,0)) for k in SIMPLE]+[str(vals.get(k,0)) for k in ARR]
print(",".join(row))
