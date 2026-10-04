#!/bin/bash
# Solidify pass: loop_spin=5000, 10 interleaved rounds, stock(PV) vs
# IVH-migration, -v -l for CS-length sanity check (should stay near-unchanged
# per the 8-round confirm, ruling out the CS-inflation confound seen at 10000).
set -u
ROUNDS=10
DUR=20
LS=5000

run_one() {
    NHEXTEND_DURATION=$DUR NHEXTEND_LOOP_SPIN=$LS /root/linux-6.17/NHextend3 -n -v -l 2>&1
}

echo "=== loop_spin=$LS  (dur=${DUR}s, $ROUNDS rounds) ==="
> /tmp/stock_5000_10r.txt
> /tmp/mig_5000_10r.txt
for i in $(seq 1 "$ROUNDS"); do
    echo 0 > /proc/sys/kernel/ivh_universal_eligible || { echo "FATAL"; exit 1; }
    OUT=$(run_one)
    RAN=$(grep -oP '^Ran for \K[0-9]+' <<< "$OUT")
    CS=$(grep -oP 'Global avg overall : \K[0-9]+' <<< "$OUT")
    echo "  round $i A(stock)      ran_for=$RAN  cs_avg_overall_ns=$CS"
    echo "$RAN $CS" >> /tmp/stock_5000_10r.txt

    echo 1 > /proc/sys/kernel/ivh_universal_eligible || { echo "FATAL"; exit 1; }
    OUT=$(run_one)
    RAN=$(grep -oP '^Ran for \K[0-9]+' <<< "$OUT")
    CS=$(grep -oP 'Global avg overall : \K[0-9]+' <<< "$OUT")
    echo "  round $i B(migration)  ran_for=$RAN  cs_avg_overall_ns=$CS"
    echo "$RAN $CS" >> /tmp/mig_5000_10r.txt
done
python3 -c "
def load(p):
    ran=[]; cs=[]
    for line in open(p):
        r,c = line.split()
        ran.append(float(r)); cs.append(float(c))
    return ran, cs
sran, scs = load('/tmp/stock_5000_10r.txt')
mran, mcs = load('/tmp/mig_5000_10r.txt')
ms, mm = sum(sran)/len(sran), sum(mran)/len(mran)
pct = (mm-ms)/ms*100
per = [(mran[i]-sran[i])/sran[i]*100 for i in range(len(sran))]
import statistics as st
sd = st.stdev(per)
se = sd / (len(per)**0.5)
t = pct/se if se else float('inf')
print(f'stock ran_for  mean={ms:.0f} range={min(sran):.0f}-{max(sran):.0f}')
print(f'migr  ran_for  mean={mm:.0f} range={min(mran):.0f}-{max(mran):.0f}')
print(f'improvement={pct:+.1f}%  per-round=' + str([f'{p:+.1f}%' for p in per]))
print(f'paired-diff sd={sd:.1f}pp  se={se:.1f}pp  t~{t:.2f}')
print(f'stock CS avg_overall ns: mean={sum(scs)/len(scs):.0f}  range={min(scs):.0f}-{max(scs):.0f}')
print(f'migr  CS avg_overall ns: mean={sum(mcs)/len(mcs):.0f}  range={min(mcs):.0f}-{max(mcs):.0f}')
"
echo "=== done ==="
