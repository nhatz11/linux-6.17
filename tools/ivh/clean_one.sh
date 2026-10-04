#!/bin/bash
# clean_one.sh <workload> [reps] -- ONE suite12 workload, reps x (pv, mig)
# interleaved. Arms differ only in ivh_universal_eligible; no adaptive spinning.
set -u
T=/root/ivh_tools
LOCK=/var/lock/ivh_clean_check.lock
exec 9>"$LOCK"; flock -n 9 || { echo "FATAL: another clean run holds $LOCK"; exit 1; }
source $T/suite12.sh
WANT="$1"; REPS="${2:-3}"; THRESH="${THRESH:-750000}"
OUT=/root/ivh_logs/clean_${WANT}.tsv; : > "$OUT"
for e in "${SUITE12[@]}"; do
  IFS='|' read -r n d m c x <<< "$e"
  [ "$n" = "$WANT" ] || continue
  [ "$c" = MEMTIER_CMD ] && c="$MEMTIER_CMD"
  d=$(eval echo "$d"); c=$(eval echo "\"$c\"")
  echo "### $n  [$m]  reps=$REPS  thresh=$THRESH"
  for rep in $(seq 1 $REPS); do
    for arm in pv mig; do
      if [ "$arm" = pv ]; then bash $T/pvbase.sh          >/dev/null 2>&1
      else                    bash $T/p7v2_arm.sh $THRESH >/dev/null 2>&1; fi
      sleep 1
      # 9>&- here too: memtier's prep daemonises memcached (-d), which would
      # inherit the flock fd and hold the lock forever after this script exits.
      prep12 "$n" >/dev/null 2>&1 9>&- || { echo "PREPFAIL"; continue; }
      # 9>&- : do not leak the flock fd into the benchmark, or a killed run
      # leaves its child holding the lock and every later invocation refuses.
      t0=$(date +%s%N)
      out=$( ( cd "$d" && timeout 900 bash -c "$c" ) 2>&1 9>&- )
      t1=$(date +%s%N)
      if [ "$x" = x ]; then val=$(python3 -c "print(f'{($t1-$t0)/1e9:.3f}')")
      else val=$(echo "$out" | eval $x | head -1); fi
      printf "%s\t%s\t%s\t%s\t%s\n" "$n" "$m" "$arm" "$rep" "${val:-NA}" >> "$OUT"
      echo "  rep$rep $arm = ${val:-NA}"
    done
  done
  python3 - "$OUT" "$m" <<'PY'
import sys,statistics as st
rows=[l.split('\t') for l in open(sys.argv[1]) if l.strip()]
pv=[float(r[4]) for r in rows if r[2]=='pv'  and r[4].strip() not in ('NA','')]
mg=[float(r[4]) for r in rows if r[2]=='mig' and r[4].strip() not in ('NA','')]
if len(pv)>1 and len(mg)>1:
    pm,gm=st.mean(pv),st.mean(mg)
    cvp,cvg=100*st.stdev(pv)/pm,100*st.stdev(mg)/gm
    ben=100*(pm-gm)/pm if sys.argv[2]=='TIME' else 100*(gm-pm)/pm
    worst=max(cvp,cvg); flag="OK" if worst<5 else ("MARGINAL" if worst<10 else "DIRTY")
    print(f"  --> PV {pm:.2f} (CV {cvp:.1f}%)  MIG {gm:.2f} (CV {cvg:.1f}%)  benefit {ben:+.2f}%  [{flag}]")
PY
done
