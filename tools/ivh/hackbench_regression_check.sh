#!/bin/bash
# Regression check: does hackbench -T -g1 -f8 -l400000 (this project's own
# standard baseline workload) regress under migration ON with cap=1010,
# tleft=4000000 -- the exact settings the trio screen/3-arm confirm just
# validated for NHextend? These ARE already this project's pre-existing
# validated defaults (see ivh_state_of_the_art_2026-07-20.md sec 2.3), so
# this is a same-session sanity re-check under current host/kernel
# conditions, not new threshold-vs-threshold tuning.
set -u
ROUNDS=5
WORKLOAD="hackbench -T -g1 -f8 -l400000"

> /tmp/hb_pv.txt; > /tmp/hb_ivh.txt

for i in $(seq 1 "$ROUNDS"); do
    echo 0 > /proc/sys/kernel/ivh_universal_eligible
    t0=$(date +%s.%N); $WORKLOAD > /tmp/hb_out.txt 2>&1; t1=$(date +%s.%N)
    v=$(grep -oP '^Time:\s*\K[0-9.]+' /tmp/hb_out.txt)
    echo "  r$i PV(migration off)  time=${v}s"
    echo "$v" >> /tmp/hb_pv.txt

    echo 1 > /proc/sys/kernel/ivh_universal_eligible
    $WORKLOAD > /tmp/hb_out.txt 2>&1
    v=$(grep -oP '^Time:\s*\K[0-9.]+' /tmp/hb_out.txt)
    echo "  r$i IVH(migration on)  time=${v}s"
    echo "$v" >> /tmp/hb_ivh.txt
done

python3 -c "
def load(p): return [float(x) for x in open(p)]
pv, ivh = load('/tmp/hb_pv.txt'), load('/tmp/hb_ivh.txt')
mpv, mivh = sum(pv)/len(pv), sum(ivh)/len(ivh)
# lower is better for hackbench wall time
pct = (mpv - mivh) / mpv * 100
per = [(pv[i]-ivh[i])/pv[i]*100 for i in range(len(pv))]
print(f'PV mean={mpv:.2f}s range={min(pv):.2f}-{max(pv):.2f}')
print(f'IVH mean={mivh:.2f}s range={min(ivh):.2f}-{max(ivh):.2f}')
print(f'IVH improvement over PV: {pct:+.1f}%  (positive = IVH faster)')
print('per-round:', [f'{p:+.1f}%' for p in per])
"
echo "=== hackbench regression check done ==="
