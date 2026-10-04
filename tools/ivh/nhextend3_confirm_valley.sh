#!/bin/bash
# Confirm pass: stock (PV) vs IVH-migration, 8 interleaved rounds each, at
# loop_spin=10000 and loop_spin=5000, with -v -l to also capture real CS
# length (NHextend3's own "Global avg overall" ns figure). NHEXTEND_DURATION
# bumped to 20s per this project's own methodology convention (vcap window
# staleness risk at <20s). spin_mode held at 1 (STOCK_PV) throughout.
set -u
ROUNDS=8
DUR=20
LOOP_SPINS=(10000 5000)

run_one() {
    NHEXTEND_DURATION=$DUR NHEXTEND_LOOP_SPIN=$LS /root/linux-6.17/NHextend3 -n -v -l 2>&1
}

for LS in "${LOOP_SPINS[@]}"; do
    echo "=== loop_spin=$LS  (dur=${DUR}s, $ROUNDS rounds) ==="
    > /tmp/stock_$LS.txt
    > /tmp/mig_$LS.txt
    for i in $(seq 1 "$ROUNDS"); do
        echo 0 > /proc/sys/kernel/ivh_universal_eligible || { echo "FATAL"; exit 1; }
        OUT=$(run_one)
        RAN=$(grep -oP '^Ran for \K[0-9]+' <<< "$OUT")
        CS=$(grep -oP 'Global avg overall : \K[0-9]+' <<< "$OUT")
        echo "  round $i A(stock)      ran_for=$RAN  cs_avg_overall_ns=$CS"
        echo "$RAN $CS" >> /tmp/stock_$LS.txt

        echo 1 > /proc/sys/kernel/ivh_universal_eligible || { echo "FATAL"; exit 1; }
        OUT=$(run_one)
        RAN=$(grep -oP '^Ran for \K[0-9]+' <<< "$OUT")
        CS=$(grep -oP 'Global avg overall : \K[0-9]+' <<< "$OUT")
        echo "  round $i B(migration)  ran_for=$RAN  cs_avg_overall_ns=$CS"
        echo "$RAN $CS" >> /tmp/mig_$LS.txt
    done
    python3 -c "
import sys
def load(p):
    ran=[]; cs=[]
    for line in open(p):
        r,c = line.split()
        ran.append(float(r)); cs.append(float(c))
    return ran, cs
sran, scs = load('/tmp/stock_$LS.txt')
mran, mcs = load('/tmp/mig_$LS.txt')
ms, mm = sum(sran)/len(sran), sum(mran)/len(mran)
pct = (mm-ms)/ms*100
per = [(mran[i]-sran[i])/sran[i]*100 for i in range(len(sran))]
print(f'  stock ran_for  mean={ms:.0f} range={min(sran):.0f}-{max(sran):.0f}')
print(f'  migr  ran_for  mean={mm:.0f} range={min(mran):.0f}-{max(mran):.0f}')
print(f'  improvement={pct:+.1f}%  per-round=' + str([f'{p:+.1f}%' for p in per]))
print(f'  stock CS avg_overall ns: mean={sum(scs)/len(scs):.0f}  range={min(scs):.0f}-{max(scs):.0f}')
print(f'  migr  CS avg_overall ns: mean={sum(mcs)/len(mcs):.0f}  range={min(mcs):.0f}-{max(mcs):.0f}')
"
done
echo "=== confirm done ==="
