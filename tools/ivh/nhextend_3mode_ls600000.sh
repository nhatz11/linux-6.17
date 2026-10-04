#!/bin/bash
# Same PV/IVH/IVH+adaptivespin comparison as the loop_spin=5000 batches, but
# at loop_spin=600000 (~1.6ms CS, the original NHextend3 default) to see if
# the futex-adaptive lock's benefit generalizes to a much longer CS.
set -u
ROUNDS=3
DUR=20
export NHEXTEND_LOOP_SPIN=600000
cd /root/linux-6.17

run() {
    local bin="$1"
    NHEXTEND_DURATION=$DUR "$bin" -n -v -l 2>&1 | grep -oP '^Ran for \K[0-9]+'
}

> /tmp/pv_ls6.txt; > /tmp/ivh_ls6.txt; > /tmp/ivh_as_ls6.txt

for i in $(seq 1 "$ROUNDS"); do
    echo 0 > /proc/sys/kernel/ivh_universal_eligible
    v=$(run ./NHextend3); echo "round $i PV                ran_for=$v"; echo "$v" >> /tmp/pv_ls6.txt

    echo 1 > /proc/sys/kernel/ivh_universal_eligible
    v=$(run ./NHextend3); echo "round $i IVH               ran_for=$v"; echo "$v" >> /tmp/ivh_ls6.txt

    v=$(run ./NHextend-full); echo "round $i IVH+adaptivespin ran_for=$v"; echo "$v" >> /tmp/ivh_as_ls6.txt
done

python3 -c "
def load(p): return [float(x) for x in open(p)]
pv, ivh, ivh_as = load('/tmp/pv_ls6.txt'), load('/tmp/ivh_ls6.txt'), load('/tmp/ivh_as_ls6.txt')
def stats(a): return sum(a)/len(a), min(a), max(a)
mpv,lpv,hpv = stats(pv); mivh,livh,hivh = stats(ivh); mas,las,has = stats(ivh_as)
print(f'PV             mean={mpv:.0f} range={lpv:.0f}-{hpv:.0f}')
print(f'IVH            mean={mivh:.0f} range={livh:.0f}-{hivh:.0f}  vs PV: {(mivh-mpv)/mpv*100:+.1f}%')
print(f'IVH+adaptspin  mean={mas:.0f} range={las:.0f}-{has:.0f}  vs PV: {(mas-mpv)/mpv*100:+.1f}%  vs IVH: {(mas-mivh)/mivh*100:+.1f}%')
per_ivh = [(ivh[i]-pv[i])/pv[i]*100 for i in range(len(pv))]
per_as  = [(ivh_as[i]-ivh[i])/ivh[i]*100 for i in range(len(pv))]
print('per-round IVH vs PV:', [f'{p:+.1f}%' for p in per_ivh])
print('per-round IVH+AS vs IVH:', [f'{p:+.1f}%' for p in per_as])
"
echo "=== done ==="
