#!/bin/bash
# WAITER side. Upstream pv_wait_early tier 1 halts on prev->state != RUNNING
# (prev HALTED). For prev being HOST-PREEMPTED while still VCPU_RUNNING,
# upstream has NO mechanism -- that halt can only come from tier 2, our TSC
# heartbeat staleness test. So attribution is unambiguous here.
#
# Control: tier2 on vs off, same load, alternating.
# Evidence that tier 2 targets correctly = the DURATION of the halts it
# causes. A halt taken because prev is genuinely preempted should last on the
# order of a real deschedule (~250-450us here). At the old broken 100us
# threshold tier-2 halts averaged 11.8us -- 20x too short.
set -u
S=/proc/sys/kernel
set_(){ echo "$2" > $S/$1 2>/dev/null; }
set_ ivh_tks_sampler_ns 0            # MANDATORY: sampler perturbs throughput
set_ ivh_adaptive_mode 2; set_ ivh_pv_tier1_enable 1
set_ ivh_pv_beat_threshold 11000000  # the FIXED threshold (5ms)
set_ ivh_pv_spin_threshold 32768
set_ ivh_cs_react_hist 0; set_ ivh_cs_head_bail 0
R="python3 /root/ivh_tools/read_ivh_counters.py"
g(){ $R ivh_node_halt_cycles ivh_node_halt_events 2>/dev/null \
     | awk '/^ivh_node_halt/{printf "%s %s\n",$2,$NF}' | tr '\n' '|'; }
arm(){
  set_ ivh_pv_tier2_enable "$1"; sleep 1
  local A=$(g)
  timeout 150 hackbench -T -g1 -f8 -l250000 >/dev/null 2>&1
  local B=$(g)
  python3 - "$1" "$A" "$B" <<'PY'
import re,sys
MHZ=2200.0
def p(s):
    d={}
    for tok in s.split('|'):
        f=tok.split()
        if len(f)==2:
            lab=f[0].strip('[]')
            d.setdefault(lab,[]).append(int(f[1]))
    return d
a,b=p(sys.argv[2]),p(sys.argv[3])
out=[]
for c in ("TIER1","TIER2","EXHAUST"):
    if c not in a or c not in b or len(a[c])<2: continue
    cy=b[c][0]-a[c][0]; ev=b[c][1]-a[c][1]
    out.append(f"{c}: n={ev:<7d} mean={cy/max(ev,1)/MHZ:7.1f}us")
print(f"tier2={sys.argv[1]}   " + "  |  ".join(out))
PY
}
echo "real deschedule scale on this host: ~250-450us. Old broken threshold gave tier2 halts of 11.8us."
for r in 1 2; do arm 1; arm 0; done
set_ ivh_pv_tier2_enable 1
echo WAITER-DONE
