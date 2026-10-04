#!/bin/bash
# Definitive in-guest validation of the kernel's steal-time AND active-time
# estimates, all 16 vCPUs, against wall-clock ground truth from a SCHED_FIFO
# busy-spin prober that also records its gap timeline for offline replay.
#
# steal : d(rq->ivh_tks_steal_ns)            vs summed host-preemption gaps
# active: rq->ivh_uc_capacity  (EMA of used/avail, used = avail - steal)
#         rq->ivh_uc_capacity_acct (independent kcpustat USER+NICE+SYS path)
#                                            vs (span - all gaps)/span
# Capacity is an EMA (~10.5s half-life) so it is sampled only in the back half
# of each 20s run, after it has tracked the prober's arrival.
set -u; S=/proc/sys/kernel; T=/root/ivh_tools
SECS=${SECS:-20}; CPUS=${CPUS:-"0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15"}
OUT=$T/validate_all_$(date +%m%d-%H%M%S).csv
echo "# knobs phase_pct=$(cat $S/ivh_tks_phase_pct) deadband=$(cat $S/ivh_tks_deadband_ns) carry=$(cat $S/ivh_tks_carry_ticks) idle_sub=$(cat $S/ivh_tks_idle_sub) min_steal_ns=$(cat $S/ivh_uc_min_steal_ns)" > $OUT
echo "cpu,span_ns,true_steal_ns,true_local_ns,true_rt_ns,true_active_ns,kern_steal_ns,cap,cap_acct,d_windows,d_extended" >> $OUT
fld(){ python3 $T/read_vact_rq.py "$1" 2>/dev/null | sed 's/.*per-cpu=\[//;s/\].*//' | cut -d, -f$(($2+1)) | tr -d ' '; }
for c in $CPUS; do
  s0=$(fld ivh_tks_steal_ns $c); w0=$(fld ivh_uc_windows $c); e0=$(fld ivh_uc_extended $c)
  $T/vcpu_trace $c $SECS 1 2200000 /tmp/va_$c.bin 400 2>/dev/null &
  tp=$!
  sleep $((SECS*3/5))
  capsum=0; acctsum=0; n=0
  for i in 1 2 3 4 5; do
    capsum=$((capsum + $(fld ivh_uc_capacity $c)))
    acctsum=$((acctsum + $(fld ivh_uc_capacity_acct $c)))
    n=$((n+1)); sleep 1
  done
  wait $tp
  s1=$(fld ivh_tks_steal_ns $c); w1=$(fld ivh_uc_windows $c); e1=$(fld ivh_uc_extended $c)
  python3 - "$c" "$((s1-s0))" "$((capsum/n))" "$((acctsum/n))" "$((w1-w0))" "$((e1-e0))" >> $OUT <<'PY'
import sys; sys.path.insert(0,'/root/ivh_tools')
from replay_tks import load, c2ns, ns2c
c,ks,cap,acct,dw,de = sys.argv[1:7]
tr=load(f'/tmp/va_{c}.bin'); khz=tr['khz']
span=c2ns(tr['t1']-tr['t0'],khz)
lo,hi=ns2c(50_000,khz),ns2c(10_000_000,khz)
host=sum(c2ns(l,khz) for _,l in tr['gaps'] if lo<=l<hi)
loc =sum(c2ns(l,khz) for _,l in tr['gaps'] if l<lo)
rt  =sum(c2ns(l,khz) for _,l in tr['gaps'] if l>=hi)
active = span - host - loc - rt
print(f"{c},{span},{host},{loc},{rt},{active},{ks},{cap},{acct},{dw},{de}")
PY
  printf "  cpu%-3s done\n" $c
done
echo "DONE -> $OUT"
