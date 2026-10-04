#!/bin/bash
# Guest half of the host-side validation. Run this in the GUEST for the same
# window as host_truth.sh on the host. No prober: with host-side ground truth
# available there is nothing to busy-spin for, so this perturbs nothing and
# covers idle vCPUs too -- both things the in-guest prober could not do.
set -u; T=/root/ivh_tools; S=/proc/sys/kernel
SECS=${1:-60}
fld(){ python3 $T/read_vact_rq.py "$1" 2>/dev/null | sed 's/.*per-cpu=\[//;s/\].*//' | tr -d ' '; }
echo "mode: sampler_ns=$(cat $S/ivh_tks_sampler_ns) duty=$(cat $S/ivh_tks_duty_pct) phase_pct=$(cat $S/ivh_tks_phase_pct) deadband=$(cat $S/ivh_tks_deadband_ns)"
echo "start_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
# per-cpu idle+iowait from /proc/stat (USER_HZ jiffies), so the guest can
# report the same active/steal/idle triple the host does. active is the
# residual: wall - idle - steal.
pstat(){ awk '/^cpu[0-9]/{print $5+$6}' /proc/stat | paste -sd,; }
A=$(fld ivh_tks_steal_ns); I0=$(pstat); T0=$(date +%s%N)
sleep "$SECS"
B=$(fld ivh_tks_steal_ns); I1=$(pstat); T1=$(date +%s%N)
C=$(fld ivh_uc_capacity)
echo "end_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)  wall_ns=$((T1-T0))"
echo
USER_HZ=$(getconf CLK_TCK)
printf "%-6s %9s %9s %9s %7s %9s\n" vcpu active% steal% idle% cap cap_steal%
python3 - "$A" "$B" "$C" "$((T1-T0))" "$I0" "$I1" "$USER_HZ" <<'PY'
import sys
a=[int(x) for x in sys.argv[1].split(',')]
b=[int(x) for x in sys.argv[2].split(',')]
c=[int(x) for x in sys.argv[3].split(',')]
wall=int(sys.argv[4])
i0=[int(x) for x in sys.argv[5].split(',')]
i1=[int(x) for x in sys.argv[6].split(',')]
hz=int(sys.argv[7])
for i,(x,y,cap) in enumerate(zip(a,b,c)):
    st=(y-x)/wall*100
    idle=(i1[i]-i0[i])*(1e9/hz)/wall*100
    act=100-st-idle
    print(f"{i:<6} {act:8.2f}% {st:8.2f}% {idle:8.2f}% {cap:7d} {(1-cap/1024)*100:8.1f}%")
PY
