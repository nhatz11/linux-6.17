#!/bin/bash
# GUEST half of the last_active ACCURACY check (not prediction quality).
# Emits exactly the two numbers the host must confirm:
#   burst COUNT     : ivh_vact_jumps            vs host  d_pcount
#   burst DURATION  : mean sampled burst        vs host  d_run_ns / d_pcount
set -u
SECS=${1:-30}
echo "=== RUN THE HOST COMMAND NOW, SAME WINDOW ==="
echo "start_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)  window=${SECS}s"
python3 - "$SECS" <<'PY'
import sys,time,statistics as st
sys.path.insert(0,"/root/ivh_tools")
import read_vact_rq as r
SECS=float(sys.argv[1]); CYC_US=2200.0
LA,J,IE = 3976,3984,3992
sym=r.load_kallsyms(); cpus=r.online_cpus()
f=open(r.KCORE,"rb"); ph=r.read_phdrs(f)
offs=[r.read_u64(f,ph,sym["__per_cpu_offset"]+8*c) for c in cpus]
b=sym["runqueues"]
def snap(fld): return [r.read_u64(f,ph,b+fld+o) for o in offs]
seq={c:[] for c in cpus}; last={c:None for c in cpus}
j0,i0=snap(J),snap(IE); t0=time.time()
while time.time()-t0<SECS:
    v=snap(LA)
    for k,c in enumerate(cpus):
        if v[k]!=last[c] and v[k]>0: seq[c].append(v[k]/CYC_US); last[c]=v[k]
j1,i1=snap(J),snap(IE); wall=time.time()-t0
print(f"wall={wall:.1f}s\n")
print(f"{'vcpu':>5}{'GUEST jumps':>13}{'idle_expl':>11}{'jumps+idle':>12}"
      f"{'GUEST mean burst':>18}{'median':>10}{'sampled':>9}")
print("-"*80)
for k,c in enumerate(cpus):
    dj=j1[k]-j0[k]; di=i1[k]-i0[k]; s=seq[c]
    if dj<5: continue
    print(f"{c:>5}{dj:>13}{di:>11}{dj+di:>12}{st.mean(s):>17.1f}us{st.median(s):>9.1f}{len(s):>9}")
print("""
COMPARE AGAINST HOST:
  count     guest (jumps + idle_expl)   ==  host d_pcount
  duration  guest mean burst            ==  host d_run_ns / d_pcount
agreement on BOTH means last_active measures the right thing.""")
PY
