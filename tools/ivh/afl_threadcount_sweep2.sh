#!/bin/bash
# Does the adaptive-spin increment (IVH-alone vs IVH+adaptivespin) shrink as
# thread count drops? Fixed loop_spin=600000 (the CS length just reconfirmed
# clean). Migration stays ON throughout; only thread count and lock
# mechanism vary. Previous attempt at loop_spin=5000 was interrupted with
# 2 suspect data points -- this is a clean restart at a different CS length.
set -u
ROUNDS=3
DUR=20
LS=600000
THREADS=(16 8 4 2 1)
cd /root/linux-6.17

echo 1 > /proc/sys/kernel/ivh_universal_eligible

run() {
    local bin="$1" n="$2"
    NHEXTEND_DURATION=$DUR NHEXTEND_LOOP_SPIN=$LS "$bin" -n "$n" 2>&1 | grep -oP '^Ran for \K[0-9]+'
}

for n in "${THREADS[@]}"; do
    echo "=== threads=$n ==="
    > /tmp/th2_ivh_$n.txt; > /tmp/th2_as_$n.txt
    for i in $(seq 1 "$ROUNDS"); do
        v=$(run ./NHextend3 $n); echo "  r$i IVH      ran_for=$v"; echo "$v" >> /tmp/th2_ivh_$n.txt
        v=$(run ./NHextend-full $n); echo "  r$i IVH+AS   ran_for=$v"; echo "$v" >> /tmp/th2_as_$n.txt
    done
    python3 -c "
def load(p): return [float(x) for x in open(p)]
ivh, as_ = load('/tmp/th2_ivh_$n.txt'), load('/tmp/th2_as_$n.txt')
def m(a): return sum(a)/len(a)
mivh, mas = m(ivh), m(as_)
per = [(as_[i]-ivh[i])/ivh[i]*100 for i in range(len(ivh))]
print(f'  IVH mean={mivh:.0f}  IVH+AS mean={mas:.0f}  improvement={(mas-mivh)/mivh*100:+.1f}%  per-round=' + str([f'{p:+.1f}%' for p in per]))
"
done
echo "=== threadcount sweep 2 done ==="
