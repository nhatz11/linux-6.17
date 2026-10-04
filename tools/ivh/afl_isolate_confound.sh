#!/bin/bash
# Decisive isolating test, per Opus's review: arm B' is NHextend-full with
# the adaptive lock DISABLED (IVH_AFL_DISABLE=1) -- same binary, same
# read_vcap_steal()-moved-outside-CS fix, same everything as arm C except
# the lock never actually sleeps (pure spin fallback, confirmed via stats:
# 0 sleeps). This isolates: does the valley-closing effect require the lock
# to actually sleep/wake, or does it come entirely from the CS-shortening
# instrumentation fix that's baked into NHextend-full regardless?
#
# If B' still shows the valley (losses/near-zero at these loop_spin values,
# same as NHextend3's arm B did) -> the lock itself is real and responsible.
# If B' ALSO closes the valley (matches arm C) -> the CS-shortening fix is
# what's actually responsible, not the sleep/wake mechanism.
#
# Also includes a fresh PV baseline (NHextend3, migration off) for context.
# Per-round values logged explicitly (not just means) per the review's note
# that this was missing from the previous 3-arm run.
set -u
ROUNDS=8
DUR=20
LOOP_SPINS=(100000 50000 25000)
cd /root/linux-6.17

run() {
    local bin="$1" ls="$2"; shift 2
    NHEXTEND_DURATION=$DUR NHEXTEND_LOOP_SPIN=$ls "$@" "$bin" -n 2>&1 | grep -oP '^Ran for \K[0-9]+'
}

echo 1 > /proc/sys/kernel/ivh_universal_eligible

for ls in "${LOOP_SPINS[@]}"; do
    echo "=== loop_spin=$ls ==="
    > /tmp/pv4_$ls.txt; > /tmp/bp4_$ls.txt; > /tmp/c4_$ls.txt
    for i in $(seq 1 "$ROUNDS"); do
        echo 0 > /proc/sys/kernel/ivh_universal_eligible
        v=$(run ./NHextend3 $ls); echo "  r$i A(PV)              ran_for=$v"; echo "$v" >> /tmp/pv4_$ls.txt
        echo 1 > /proc/sys/kernel/ivh_universal_eligible

        v=$(run ./NHextend-full $ls env IVH_AFL_DISABLE=1); echo "  r$i B'(IVH+fullbin,lock OFF) ran_for=$v"; echo "$v" >> /tmp/bp4_$ls.txt

        v=$(run ./NHextend-full $ls); echo "  r$i C(IVH+adaptivespin)  ran_for=$v"; echo "$v" >> /tmp/c4_$ls.txt
    done
    python3 -c "
def load(p): return [float(x) for x in open(p)]
pv, bp, c = load('/tmp/pv4_$ls.txt'), load('/tmp/bp4_$ls.txt'), load('/tmp/c4_$ls.txt')
def m(a): return sum(a)/len(a)
import statistics as st
mpv, mbp, mc = m(pv), m(bp), m(c)
per_bp = [(bp[i]-pv[i])/pv[i]*100 for i in range(len(pv))]
per_c  = [(c[i]-pv[i])/pv[i]*100 for i in range(len(pv))]
per_cvb = [(c[i]-bp[i])/bp[i]*100 for i in range(len(pv))]
def t(d):
    sd = st.stdev(d); se = sd/len(d)**0.5
    return sum(d)/len(d), se, (sum(d)/len(d))/se if se else float('inf')
mbpp, sebp, tbp = t(per_bp)
mcp, sec, tc = t(per_c)
mcvbp, secvb, tcvb = t(per_cvb)
print(f'  PV mean={mpv:.0f}')
print(f\"  B'(lock OFF, same binary) mean={mbp:.0f}  vs PV: {mbpp:+.1f}% (se={sebp:.1f}pp t={tbp:.2f})  per-round=\" + str([f'{p:+.1f}%' for p in per_bp]))
print(f'  C (lock ON)               mean={mc:.0f}  vs PV: {mcp:+.1f}% (se={sec:.1f}pp t={tc:.2f})  per-round=' + str([f'{p:+.1f}%' for p in per_c]))
print(f\"  C vs B' (the lock's OWN marginal effect, same binary both sides): {mcvbp:+.1f}% (se={secvb:.1f}pp t={tcvb:.2f})  per-round=\" + str([f'{p:+.1f}%' for p in per_cvb]))
"
done
echo "=== isolating test done ==="
