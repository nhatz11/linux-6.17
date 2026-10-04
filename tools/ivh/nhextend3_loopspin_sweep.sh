#!/bin/bash
# Sweep NHEXTEND_LOOP_SPIN downward, stock vs migration, interleaved, to find
# how short a CS length still gets a consistent >=10% migration win on THIS
# (GLOCK/rebuild) kernel's migration engine. spin_mode held at 1 (STOCK_PV)
# throughout so only migration varies.
set -u
ROUNDS="${1:-3}"
DUR="${NHEXTEND_DURATION:-10}"
LOOP_SPINS=(600000 300000 150000 100000 50000 25000 10000 5000)

run_one() {
    local label="$1"
    OUT=$(NHEXTEND_DURATION=$DUR NHEXTEND_LOOP_SPIN=$LS /root/linux-6.17/NHextend3 -n 16 2>&1)
    RAN=$(grep -oP '^Ran for \K[0-9]+' <<< "$OUT")
    echo "$label  ran_for=$RAN"
}

for LS in "${LOOP_SPINS[@]}"; do
    echo "=== loop_spin=$LS  (dur=${DUR}s, $ROUNDS rounds) ==="
    STOCK=(); MIG=()
    for i in $(seq 1 "$ROUNDS"); do
        echo 0 > /proc/sys/kernel/ivh_universal_eligible || { echo "FATAL"; exit 1; }
        OUT=$(run_one "  round $i A(stock)")
        echo "$OUT"
        STOCK+=("$(grep -oP 'ran_for=\K[0-9]+' <<< "$OUT")")
        echo 1 > /proc/sys/kernel/ivh_universal_eligible || { echo "FATAL"; exit 1; }
        OUT=$(run_one "  round $i B(migration)")
        echo "$OUT"
        MIG+=("$(grep -oP 'ran_for=\K[0-9]+' <<< "$OUT")")
    done
    python3 -c "
stock=[${STOCK[*]// /,}]
mig=[${MIG[*]// /,}]
ms=sum(stock)/len(stock); mm=sum(mig)/len(mig)
pct=(mm-ms)/ms*100
per=[(mig[i]-stock[i])/stock[i]*100 for i in range(len(stock))]
print(f'  stock mean={ms:.0f} range={min(stock)}-{max(stock)}')
print(f'  migr  mean={mm:.0f} range={min(mig)}-{max(mig)}')
print(f'  improvement={pct:.1f}%  per-round=' + str([f'{p:.1f}%' for p in per]))
"
done
echo "=== sweep done ==="
