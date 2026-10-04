#!/bin/bash
# Where do the 92% go? Funnel from handoff to mark.
#   handoffs        -> ivh_rot_handoffs
#   walks entered   -> ivh_evict_walks      (pre-filter said "maybe")
#   walks that acted-> ivh_evict_walks_acted
#   marks           -> ivh_evict_marked
# and the refusal reasons inside the walk:
#   stop_halted / tail_stop / cap_refused / halt_race / lookahead_refused
set -u
S=/proc/sys/kernel
set_(){ echo "$2" > $S/$1 2>/dev/null; }
set_ ivh_tks_sampler_ns 0; set_ ivh_adaptive_mode 2
set_ ivh_pv_tier1_enable 1; set_ ivh_pv_tier2_enable 0
set_ ivh_pv_preempt_src 2
set_ ivh_pv_evict_enable 1; set_ ivh_pv_evict_node_stamp 1
set_ ivh_pv_evict_threshold 1100000
set_ ivh_pv_evict_debug 1          # arm the pre-filter's accounting
set_ ivh_skipcheck_hist 1
set_ ivh_pv_evict_hop_cap "${HOP:-2}"
set_ ivh_pv_evict_lookahead "${LA:-1}"
echo "hop_cap=$(cat $S/ivh_pv_evict_hop_cap) lookahead=$(cat $S/ivh_pv_evict_lookahead) thr=$(( $(cat $S/ivh_pv_evict_threshold)/2200 ))us"
R="python3 /root/ivh_tools/read_ivh_counters.py"
N="ivh_rot_handoffs ivh_evict_walks ivh_evict_walks_acted ivh_evict_marked ivh_evict_stop_halted ivh_evict_tail_stop ivh_evict_cap_refused ivh_evict_halt_race ivh_evict_lookahead_refused"
g(){ $R $N 2>/dev/null | awk '/^ivh_/{printf "%s=%s\n",$1,$NF}'; }
g > /tmp/ef_a.txt
timeout 150 hackbench -T ${WL:--g1} -f8 -l250000 >/dev/null 2>&1
g > /tmp/ef_b.txt
set_ ivh_skipcheck_hist 0; set_ ivh_pv_evict_enable 0; set_ ivh_pv_evict_debug 0
python3 - <<'PY'
def p(f):
    d={}
    for ln in open(f):
        k,_,v=ln.strip().partition('=')
        if v.isdigit(): d[k]=int(v)
    return d
a,b=p('/tmp/ef_a.txt'),p('/tmp/ef_b.txt')
d={k:b.get(k,0)-a.get(k,0) for k in b}
order=["ivh_rot_handoffs","ivh_evict_walks","ivh_evict_walks_acted","ivh_evict_marked"]
print("\nFUNNEL")
prev=None
for k in order:
    v=d.get(k,0)
    frac=f"  ({100.0*v/prev:.2f}% of previous)" if prev else ""
    print(f"  {k:26s} {v:10d}{frac}")
    prev=v if v else None
print("\nREFUSALS INSIDE THE WALK")
for k in ("ivh_evict_stop_halted","ivh_evict_tail_stop","ivh_evict_cap_refused",
          "ivh_evict_halt_race","ivh_evict_lookahead_refused"):
    print(f"  {k:26s} {d.get(k,0):10d}")
PY
echo FUNNEL-DONE
