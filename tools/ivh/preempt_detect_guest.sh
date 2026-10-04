#!/bin/bash
# GUEST half: how many preemptions does the TSC detector claim, per vCPU?
#   ivh_vact_jumps          - TSC gap attributed to HOST PREEMPTION
#   ivh_vact_idle_explained - TSC gap attributed to the vCPU having idled
#   ivh_tks_steal_ns        - total steal booked
# Pair with the host command (schedstat field 3 = pcount = times scheduled in).
#   host preemptions ~= pcount - (wakeups from halt)
#   on a vCPU that never idles, pcount ~= jumps
set -u
SECS=${1:-60}
echo "start_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)  window=${SECS}s"
python3 - "$SECS" <<'PY'
import sys,time
sys.path.insert(0,"/root/ivh_tools")
import read_vact_rq as r
SECS=float(sys.argv[1]); KHZ=2200000
J,IE,ST = 3984, 3992, 3912
sym=r.load_kallsyms(); cpus=r.online_cpus()
f=open(r.KCORE,"rb"); ph=r.read_phdrs(f)
offs=[r.read_u64(f,ph,sym["__per_cpu_offset"]+8*c) for c in cpus]
b=sym["runqueues"]
def snap(fld): return [r.read_u64(f,ph,b+fld+o) for o in offs]
j0,i0,s0=snap(J),snap(IE),snap(ST); t0=time.time()
time.sleep(SECS)
j1,i1,s1=snap(J),snap(IE),snap(ST); wall=time.time()-t0
print(f"end_utc wall={wall:.1f}s\n")
print(f"{'vcpu':>5}{'jumps':>10}{'jumps/s':>10}{'idle_expl':>11}{'jumps+idle':>12}"
      f"{'steal_ms':>10}{'mean preempt us':>17}")
print("-"*76)
for i,c in enumerate(cpus):
    dj=j1[i]-j0[i]; di=i1[i]-i0[i]; ds=(s1[i]-s0[i])/1e6
    mean=(ds*1000/dj) if dj else 0
    print(f"{c:>5}{dj:>10}{dj/wall:>10.1f}{di:>11}{dj+di:>12}{ds:>10.1f}{mean:>17.1f}")
PY
