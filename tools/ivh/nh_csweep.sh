#!/bin/bash
# nh_csweep.sh -- how short can NHextend's critical section get and still pay?
#
# Context: suite12.sh runs NHEXTEND_LOOP_SPIN=5000 (~13us CS). The docs'
# validated operating point is 600000 (~1.6ms CS) -- 120x longer. At 13us the
# host never preempts a lock holder (measured: 0/360,325 host-preempted CS),
# so neither migration nor the AFL adaptive lock has anything to repair and
# both read as inert. This finds the knee.
#
# Config held at cvm_setup/nhextend_full_best_config.sh throughout; only
# ivh_universal_eligible (migration) and IVH_AFL_DISABLE (userspace adaptive
# lock) toggle, which is exactly how that config describes the two mechanisms.
set -u
S=/proc/sys/kernel
OUT=${OUT:-/root/ivh_logs/nhcs_$(date +%m%d-%H%M%S)}; mkdir -p "$OUT"
SPINS=${SPINS:-"600000 300000 150000 75000 40000 20000 10000 5000"}
ROUNDS=${ROUNDS:-2}
DUR=${DUR:-10}

pgrep -xc MY_ivh_atc >/dev/null || { echo "FATAL: MY_ivh_atc not running"; exit 1; }
[ "$(cat $S/ivh_cap_writer)" = 0 ] || { echo "FATAL: cap_writer must be 0 (in-kernel capacity)"; exit 1; }
echo -e "loop_spin\tmode\tround\tvalue" > "$OUT/raw.tsv"

nh(){ env IVH_AFL_DISABLE=$1 NHEXTEND_DURATION=$DUR NHEXTEND_LOOP_SPIN=$2 \
      timeout 180 /root/linux-6.17/NHextend-full -n 16 2>&1 | grep -oP 'Ran for \K[0-9]+'; }

printf "%-10s %-10s %-10s %-10s %-9s %-9s\n" "loop_spin" "PV" "IVH" "IVH+AS" "IVH%" "IVH+AS%"
for sp in $SPINS; do
  for r in $(seq "$ROUNDS"); do
    echo 0 > $S/ivh_universal_eligible; sleep 2
    v=$(nh 1 "$sp"); echo -e "$sp\tpv\t$r\t$v" >> "$OUT/raw.tsv"
    echo 1 > $S/ivh_universal_eligible; sleep 2
    v=$(nh 1 "$sp"); echo -e "$sp\tivh\t$r\t$v" >> "$OUT/raw.tsv"
    sleep 2
    v=$(nh 0 "$sp"); echo -e "$sp\tivh_as\t$r\t$v" >> "$OUT/raw.tsv"
  done
  python3 - "$OUT/raw.tsv" "$sp" <<'PY'
import sys, statistics, collections
d=collections.defaultdict(list)
for ln in open(sys.argv[1]):
    p=ln.rstrip().split('\t')
    if p[0]=='loop_spin' or p[0]!=sys.argv[2] or not p[3]: continue
    d[p[1]].append(float(p[3]))
if 'pv' in d:
    pv=statistics.median(d['pv']); iv=statistics.median(d.get('ivh',[0])); ia=statistics.median(d.get('ivh_as',[0]))
    print(f"{int(sys.argv[2]):<10} {pv:<10.0f} {iv:<10.0f} {ia:<10.0f} "
          f"{(iv-pv)/pv*100:>+8.1f}% {(ia-pv)/pv*100:>+8.1f}%")
PY
done
echo 1 > $S/ivh_universal_eligible
echo; echo "artifacts: $OUT"
