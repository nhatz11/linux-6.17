#!/bin/bash
# Does replaying the trace reproduce the kernel's own counter? If yes, the
# simulator is faithful and all further sweeps can be done offline against a
# fixed timeline instead of 8s-per-cell against a moving host load.
set -u; S=/proc/sys/kernel; T=/root/ivh_tools
SECS=${SECS:-10}
P=$(cat $S/ivh_tks_phase_pct); D=$(cat $S/ivh_tks_deadband_ns); C=$(cat $S/ivh_tks_carry_ticks)
echo "live knobs: phase_pct=$P deadband=$D carry_ticks=$C idle_sub=$(cat $S/ivh_tks_idle_sub)"
getst(){ python3 $T/read_vact_rq.py ivh_tks_steal_ns 2>/dev/null \
         | sed 's/.*per-cpu=\[//;s/\].*//' | cut -d, -f$(($1+1)) | tr -d ' '; }
for cpu in "$@"; do
  a=$(getst $cpu)
  $T/vcpu_trace $cpu $SECS 1 2200000 /tmp/tr_$cpu.bin 400 2>/dev/null
  b=$(getst $cpu)
  python3 - "$cpu" "$((b-a))" "$P" "$D" "$C" <<'PY'
import sys; sys.path.insert(0,'/root/ivh_tools')
from replay_tks import load, replay, truth_ns, c2ns
cpu, kern, P, D, C = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
tr = load(f'/tmp/tr_{cpu}.bin')
span = c2ns(tr['t1']-tr['t0'], tr['khz'])
best = None
for ph in range(0, 2200000, 220000):          # unknown tick phase: scan the grid
    sim,_ = replay(tr, P, D, C, phase=ph)
    if best is None or abs(sim-kern) < abs(best[0]-kern): best = (sim, ph)
sim, ph = best
truth = truth_ns(tr, 0)
print(f"  cpu{cpu}: span={span/1e6:.1f}ms  truth={truth/1e6:9.2f}ms  "
      f"kernel={kern/1e6:9.2f}ms  replay={sim/1e6:9.2f}ms")
print(f"          replay/kernel={sim/kern if kern else 0:6.3f}   "
      f"kernel/truth={kern/truth if truth else 0:6.3f}   "
      f"replay/truth={sim/truth if truth else 0:6.3f}  (best phase {ph})")
PY
done
