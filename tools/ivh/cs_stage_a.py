#!/usr/bin/env python3
"""Stage A delta report: cs_stage_a.py before.json after.json [threshold]"""
import json, sys
a=json.load(open(sys.argv[1])); b=json.load(open(sys.argv[2]))
for _k in list(b):
    a.setdefault(_k, b[_k] if not isinstance(b[_k], (int,)) else 0); thr=int(sys.argv[3]) if len(sys.argv)>3 else 0
def d(k):
    x,y=a[k],b[k]
    if isinstance(x,list):
        if x and isinstance(x[0],list): return [[q-p for p,q in zip(r1,r2)] for r1,r2 in zip(x,y)]
        return [q-p for p,q in zip(x,y)]
    return y-x
S=lambda k:d(k)
p1=["ivh_cs_abstain_tenure","ivh_cs_abstain_hashed","ivh_cs_abstain_late","ivh_cs_abstain_noprev","ivh_cs_abstain_rot","ivh_cs_abstain_tag","ivh_cs_abstain_skew","ivh_cs_abstain_young","ivh_cs_abstain_nolastcs","ivh_cs_long_hold"]
p2=["ivh_cs_abstain_nohz","ivh_cs_abstain_retag","ivh_cs_healthy_long","ivh_cs_fired"]
cc=S("ivh_cs_check_calls"); lh=S("ivh_cs_long_hold")
print("check_calls", cc, "| partition1 sum", sum(S(k) for k in p1), "dev", cc-sum(S(k) for k in p1))
for k in p1: print(f"   {k:34s} {S(k):>12d}  {100*S(k)/max(cc,1):6.2f}%")
print("long_hold", lh, "| partition2 sum", sum(S(k) for k in p2), "dev", lh-sum(S(k) for k in p2))
for k in p2: print(f"   {k:34s} {S(k):>12d}")
for k in ["ivh_cs_stamps","ivh_cs_clears","ivh_cs_stamp_overwrote","ivh_cs_ep_events","ivh_cs_tenure0_enter","ivh_cs_tenure0_hashed","ivh_cs_tenure0_hashed_released","ivh_cs_tenure0_late","ivh_cs_shadow_gate_pass_released","ivh_head_spin_attempts","ivh_halt_from_head","ivh_cs_fast_lookup_hit","ivh_cs_fast_lookup_miss","ivh_cs_scan_hit","ivh_cs_scan_miss","ivh_cs_bail_suppressed","ivh_rot_stop_halted","ivh_rot_splice_done","ivh_rot_splice_blocked_tail","ivh_rot_splice_blocked_starve","ivh_cs_head_bailed","ivh_head_spin_bail_attempts"]:
    print(f"{k:36s} {S(k):>12d}")
att=S("ivh_head_spin_attempts")
if att: print("avg head spin iters per exhaustion", S("ivh_head_spin_iters_sum")/att, "threshold", thr)
eb=S("ivh_cs_ep_events_by_end"); ec=S("ivh_cs_ep_cycles")
names=["ACQUIRED","HOLDER_CHANGED","EXHAUST"]
for i,n in enumerate(names):
    if eb[i]: print(f"episodes {n:15s} n={eb[i]:>8d} mean={ec[i]/eb[i]/2200:9.1f} us")
if S("ivh_cs_ep_events"): print("fires per episode", S("ivh_cs_fired")/S("ivh_cs_ep_events"))
if lh: print("healthy_long/long_hold", S("ivh_cs_healthy_long")/lh)
def hist(name,row=None):
    h=d(name)
    if row is not None:
        h = h[row] if isinstance(h[0], list) else h[row*32:(row+1)*32]
    tot=sum(h)
    if not tot: return "empty"
    c=0
    for i,v in enumerate(h):
        c+=v
        if c>=tot/2: return f"n={tot} p50~2^{i} cyc ({(1<<i)/2200:.1f} us)"
print("tenure_hist undetected", hist("ivh_cs_tenure_hist",0)); print("tenure_hist detected  ", hist("ivh_cs_tenure_hist",1))
print("prev_hold_hist", hist("ivh_cs_prev_hold_hist")); print("prompt_hist", hist("ivh_cs_prompt_hist"))
for i,n in enumerate(names): print(f"ep_hist {n:15s}", hist("ivh_cs_ep_hist",i))
hc=S("ivh_head_halt_cycles"); he=S("ivh_head_halt_events")
for i,n in enumerate(["EXHAUST","CS"]):
    if he[i]: print(f"head halt {n}: n={he[i]} mean={hc[i]/he[i]/2200:.1f} us  hist {hist('ivh_head_halt_hist',i)}")
