#!/bin/bash
# 3-arm confirm across all 8 loop_spin values from the 2026-09-11 sweep:
#   A: PV (migration off, NHextend3)
#   B: IVH-alone (migration on, NHextend3, SAME cap/tleft as chosen trio) --
#      NEW same-day/same-host-state data point, did not exist before this run.
#      Needed to isolate: does the valley-closing effect (seen in the corner
#      screen, where ALL 8 trios showed huge wins even at loop_spin=100000/
#      50000/25000) come from cap/tleft retuning, or from the adaptive-spin
#      lock itself (present only in arm C)?
#   C: IVH+adaptivespin (migration on, NHextend-full, same cap/tleft, IVH_AFL_SPINS=256)
# Chosen trio: cap=1010, tleft=4000000 (both == already-validated defaults --
# the corner screen showed <3pp spread across all 8 trios tested, so trio
# choice does not meaningfully differentiate outcome; using the
# already-validated defaults is the most defensible choice here).
set -u
ROUNDS=3
DUR=20
LOOP_SPINS=(600000 300000 150000 100000 50000 25000 10000 5000)
cd /root/linux-6.17

run() {
    local bin="$1" ls="$2"
    NHEXTEND_DURATION=$DUR NHEXTEND_LOOP_SPIN=$ls IVH_AFL_SPINS=256 "$bin" -n 2>&1 | grep -oP '^Ran for \K[0-9]+'
}

for ls in "${LOOP_SPINS[@]}"; do
    echo "=== loop_spin=$ls ==="
    > /tmp/pv3_$ls.txt; > /tmp/ivh3_$ls.txt; > /tmp/as3_$ls.txt
    for i in $(seq 1 "$ROUNDS"); do
        echo 0 > /proc/sys/kernel/ivh_universal_eligible
        v=$(run ./NHextend3 $ls); echo "  r$i A(PV)   ran_for=$v"; echo "$v" >> /tmp/pv3_$ls.txt

        echo 1 > /proc/sys/kernel/ivh_universal_eligible
        v=$(run ./NHextend3 $ls); echo "  r$i B(IVH)  ran_for=$v"; echo "$v" >> /tmp/ivh3_$ls.txt

        v=$(run ./NHextend-full $ls); echo "  r$i C(IVH+AS) ran_for=$v"; echo "$v" >> /tmp/as3_$ls.txt
    done
    python3 -c "
def load(p): return [float(x) for x in open(p)]
pv, ivh, as_ = load('/tmp/pv3_$ls.txt'), load('/tmp/ivh3_$ls.txt'), load('/tmp/as3_$ls.txt')
def m(a): return sum(a)/len(a)
mpv, mivh, mas = m(pv), m(ivh), m(as_)
print(f'  PV mean={mpv:.0f}  IVH mean={mivh:.0f} ({(mivh-mpv)/mpv*100:+.1f}% vs PV)  IVH+AS mean={mas:.0f} ({(mas-mpv)/mpv*100:+.1f}% vs PV, {(mas-mivh)/mivh*100:+.1f}% vs IVH)')
"
done
echo "=== 3-arm confirm done ==="
