#!/bin/bash
# clean_all.sh [reps] -- all 12 suite12 workloads, pv vs MIGRATION-ALONE,
# interleaved with order rotation, at the agreed ivh_time_left_threshold_ns.
#
# Differences from the 2026-10-01 campaign, both of which invalidated it:
#   THRESH is 2500000, not 750000. 750000 throttled Gate 2 to 82.7-98% reject
#   and cost ebizzy 43pp; it appears in no validated arm.
#   sync + drop_caches + sleep 1 before EVERY run, as campaign/noprobe.sh and
#   campaign/fullstack.sh both do. Omitting it made ebizzy's PV arm look like it
#   had a warm-up drift.
set -u
T=/root/ivh_tools
LOCK=/var/lock/ivh_clean_check.lock
exec 9>"$LOCK"; flock -n 9 || { echo "FATAL: another run holds $LOCK"; exit 1; }
source $T/suite12.sh
REPS="${1:-3}"; THRESH="${THRESH:-2500000}"
STAMP=$(date +%m%d-%H%M%S)
OUT=/root/ivh_logs/clean_all_${STAMP}.tsv
printf "workload\tmetric\tarm\trep\tvalue\tmigrations\n" > "$OUT"
echo "### clean_all  reps=$REPS  THRESH=$THRESH  migration-alone (spin_mode 1)"
echo "### host corunner + 8-starved/8-healthy split; uc_cap 0-7 = $(awk 'NR==3{print $11}' /proc/ivh_cpu_stats)"
echo "### -> $OUT"
mig(){ python3 $T/migcount.py 2>/dev/null || echo 0; }

for e in "${SUITE12[@]}"; do
  IFS='|' read -r n d m c x <<< "$e"
  # ONLY="a b c" restricts the run to those workloads (resume after an abort)
  if [ -n "${ONLY:-}" ]; then case " $ONLY " in *" $n "*) ;; *) continue;; esac; fi
  [ "$c" = MEMTIER_CMD ] && c="$MEMTIER_CMD"
  d=$(eval echo "$d"); c=$(eval echo "\"$c\"")
  echo "########## $n [$m] ##########"
  for rep in $(seq 1 $REPS); do
    # rotate order so a monotone drift cannot be read as an arm effect
    if [ $((rep % 2)) -eq 1 ]; then ORDER="pv mig"; else ORDER="mig pv"; fi
    for arm in $ORDER; do
      if [ "$arm" = pv ]; then bash $T/pvbase.sh >/dev/null 2>&1 || { echo "ARMFAIL pv"; continue; }
      else bash $T/p7v2_arm.sh "$THRESH" >/dev/null 2>&1 || { echo "ARMFAIL mig"; continue; }; fi
      sleep 1
      prep12 "$n" >/dev/null 2>&1 9>&- || { echo "  PREPFAIL $n"; continue; }
      sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      m0=$(mig)
      t0=$(date +%s%N)
      out=$( ( cd "$d" && timeout 900 bash -c "$c" ) 2>&1 9>&- )
      t1=$(date +%s%N)
      m1=$(mig)
      if [ "$x" = x ]; then val=$(python3 -c "print(f'{($t1-$t0)/1e9:.3f}')")
      else val=$(echo "$out" | eval $x | head -1); fi
      printf "%s\t%s\t%s\t%s\t%s\t%s\n" "$n" "$m" "$arm" "$rep" "${val:-NA}" "$((m1-m0))" >> "$OUT"
      echo "  rep$rep $arm = ${val:-NA}  migs=$((m1-m0))"
    done
  done
  python3 - "$OUT" "$n" "$m" <<'PY'
import sys,statistics as st
rows=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
rows=[r for r in rows if r[0]==sys.argv[2]]
pv=[float(r[4]) for r in rows if r[2]=='pv'  and r[4] not in ('NA','')]
mg=[float(r[4]) for r in rows if r[2]=='mig' and r[4] not in ('NA','')]
mi=[int(r[5])   for r in rows if r[2]=='mig']
if len(pv)>1 and len(mg)>1:
    p,g=st.mean(pv),st.mean(mg)
    cvp,cvg=100*st.stdev(pv)/p,100*st.stdev(mg)/g
    ben=100*(p-g)/p if sys.argv[3]=='TIME' else 100*(g-p)/p
    worst=max(cvp,cvg); flag="OK" if worst<5 else ("MARGINAL" if worst<10 else "DIRTY")
    print(f"  ==> PV {p:.2f} (CV {cvp:.1f}%)  MIG {g:.2f} (CV {cvg:.1f}%)  "
          f"benefit {ben:+.2f}%  migs {st.mean(mi):.0f}  [{flag}]")
PY
done
echo "########## ALL DONE $(date -Is) -> $OUT ##########"
