#!/bin/bash
# g52_ab.sh -- the vcap-vs-kernel A/B. Run AFTER g52_postboot.sh.
#
# Two outputs, and the second is the one that answers the /goal:
#   1. throughput per arm (is vcap's capacity at least as good?)
#   2. the capacity and active-time SIGNALS themselves, kernel vs vcap,
#      sampled at the same instant on the same boot
set -u
S=/proc/sys/kernel
OUT=${OUT:-/root/ivh_logs/g52_$(date +%m%d-%H%M%S)}; mkdir -p "$OUT"
REPS=${REPS:-3}
ARMS=${ARMS:-"pv A B C D"}

pgrep -x vcap >/dev/null || { echo "FATAL: vcap not running"; exit 1; }
echo "kernel: $(uname -r)"; echo "vcap: $(pgrep -a vcap | grep -v probe)"
echo "reps=$REPS arms=$ARMS" | tee "$OUT/config.txt"

run_bench() {
  case "$1" in
    hackbench) ( cd /root && timeout 120 hackbench -T -g1 -f8 -l150000 ) 2>&1 \
                 | grep -oP '^Time:\s*\K[0-9.]+' ;;
    dbench)    ( cd /root && timeout 120 dbench -F -t 15 16 -D /root/dbench_test ) 2>&1 \
                 | grep -oP 'Throughput\s+\K[0-9.]+' ;;
    ebizzy)    ( cd /root && timeout 60 /home/nick/Desktop/ebizzy -S 6 -t 16 -m -s 4194304 ) 2>&1 \
                 | grep -oP '^\K[0-9]+(?= records)' ;;
  esac
}

for arm in $ARMS; do
  bash /root/ivh_tools/g52_arm.sh "$arm" || exit 1
  sleep 3
  cat /proc/ivh_cpu_stats > "$OUT/stats_$arm.txt"   # cat: full file, no truncation
  for b in hackbench dbench ebizzy; do
    for r in $(seq "$REPS"); do
      v=$(run_bench "$b")
      echo "$arm $b $r $v" | tee -a "$OUT/raw.tsv"
    done
  done
done

bash /root/ivh_tools/g52_arm.sh A >/dev/null
echo; echo "================= RESULTS ================="
python3 - "$OUT" <<'PY'
import sys, statistics, collections, os
out = sys.argv[1]
d = collections.defaultdict(list)
for ln in open(os.path.join(out, "raw.tsv")):
    a, b, r, v = ln.split()
    try: d[(a, b)].append(float(v))
    except ValueError: pass
HI = {"dbench", "ebizzy"}          # higher is better; hackbench is a TIME
benches = ["hackbench", "dbench", "ebizzy"]
arms = [a for a in ["pv","A","B","C","D"] if any((a,b) in d for b in benches)]
print(f"{'bench':<10} " + " ".join(f"{a:>12}" for a in arms))
for b in benches:
    row = f"{b:<10} "
    for a in arms:
        v = d.get((a,b))
        row += f"{statistics.median(v):>12.2f} " if v else f"{'-':>12} "
    print(row)
print()
print("vs PV  (+ = better: throughput gain, or time saved for hackbench)")
for b in benches:
    pv = d.get(("pv",b))
    if not pv: continue
    p = statistics.median(pv); row = f"{b:<10} "
    for a in arms:
        if a == "pv": row += f"{'baseline':>12} "; continue
        v = d.get((a,b))
        if not v: row += f"{'-':>12} "; continue
        m = statistics.median(v)
        pct = (p-m)/p*100 if b not in HI else (m-p)/p*100
        row += f"{pct:>+11.2f}% "
    print(row)
PY
echo
echo "=== capacity / active signal, per arm ==="
for arm in $ARMS; do
  [ -f "$OUT/stats_$arm.txt" ] || continue
  printf "%-4s cap: " "$arm"; awk 'NR>2 {printf "%s ",$11}' "$OUT/stats_$arm.txt"
  printf "\n%-4s act: " "$arm"; awk 'NR>2 {printf "%.2fms ",$10/1e6}' "$OUT/stats_$arm.txt"; echo
done
echo; echo "artifacts: $OUT"
