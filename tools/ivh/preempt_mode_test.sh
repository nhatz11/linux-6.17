#!/bin/bash
# Test the user's theory: under IVH migration, does preempt=voluntary beat
# preempt=lazy (the kernel default) for hackbench? Theory: migration
# concentrates load onto destination vCPUs (moving threads away from
# throttled sources), over-stressing that subset; under lazy preemption,
# lock handoff to a waiter on an over-stressed vCPU is slower (fewer
# reschedule points) than under voluntary.
set -u
ROUNDS=5
WORKLOAD="hackbench -T -g1 -f8 -l400000"

> /tmp/pm_lazy.txt; > /tmp/pm_vol.txt

for i in $(seq 1 "$ROUNDS"); do
    echo lazy > /sys/kernel/debug/sched/preempt
    v=$($WORKLOAD 2>&1 | grep -oP '^Time:\s*\K[0-9.]+')
    echo "  r$i lazy       time=${v}s"
    echo "$v" >> /tmp/pm_lazy.txt

    echo voluntary > /sys/kernel/debug/sched/preempt
    v=$($WORKLOAD 2>&1 | grep -oP '^Time:\s*\K[0-9.]+')
    echo "  r$i voluntary  time=${v}s"
    echo "$v" >> /tmp/pm_vol.txt
done

python3 -c "
def load(p): return [float(x) for x in open(p)]
lazy, vol = load('/tmp/pm_lazy.txt'), load('/tmp/pm_vol.txt')
def m(a): return sum(a)/len(a)
mlazy, mvol = m(lazy), m(vol)
pct = (mlazy - mvol) / mlazy * 100  # positive = voluntary faster (lower time)
per = [(lazy[i]-vol[i])/lazy[i]*100 for i in range(len(lazy))]
print(f'lazy      mean={mlazy:.2f}s range={min(lazy):.2f}-{max(lazy):.2f}')
print(f'voluntary mean={mvol:.2f}s range={min(vol):.2f}-{max(vol):.2f}')
print(f'voluntary improvement over lazy: {pct:+.1f}%')
print('per-round:', [f'{p:+.1f}%' for p in per])
"
echo "=== preempt mode test done ==="
