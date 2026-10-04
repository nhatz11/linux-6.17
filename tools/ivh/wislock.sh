#!/bin/bash
# wislock.sh -- does IVH win on will-it-scale's LOCK testcases once the harness
#               stops pinning threads to single CPUs?
#
# Context: the Sept campaign recorded wis_lock1 -9.9% (0/3) and wis_lock2 -9.1%
# (0/3) as REGRESSIONS, while wis_mmap1/mmap2 were the only CANDIDATEs (+11%).
# Point 4 then showed will-it-scale pins every worker to one CPU
# (Cpus_allowed_list 0,1,2,...), so cpumask_weight==1 and Gate 3 (fair.c:13938)
# rejects 99% of acquisitions -- migration is structurally impossible.
# will-it-scale's -n flag disables that pinning ("No affinity"), so this asks
# whether the regression is the pinning or the workload.
#
# mmap2 is the control: it was a win WHILE pinned, so if unpinning changes it
# too, the effect is not specific to the lock testcases.
#
# Detached + resumable: rows append as produced, summary rewritten per combo,
# completed (combo,arm) pairs skipped on restart.
set -u
S=/proc/sys/kernel
T=/root/ivh_tools
W=/root/bench/will-it-scale
REPS="${REPS:-5}"
OUT="${OUT:-$T/wislock_data.csv}"
SUM="${SUM:-$T/wislock_summary.txt}"
DONE="${DONE:-$T/wislock.DONE}"

COMBOS=(
"lock1_pinned|./lock1_threads -t 16 -s 15"
"lock1_unpinned|./lock1_threads -t 16 -s 15 -n"
"lock2_pinned|./lock2_threads -t 16 -s 15"
"lock2_unpinned|./lock2_threads -t 16 -s 15 -n"
"mmap2_pinned|./mmap2_threads -t 16 -s 15"
"mmap2_unpinned|./mmap2_threads -t 16 -s 15 -n"
)
log(){ echo "[$(date +%H:%M:%S)] $*"; }
setarm(){
  case "$1" in
    pv)  bash $T/pvbase.sh >/dev/null || return 1 ;;
    ivh) bash $T/p78_arm.sh tlt 10000000 >/dev/null || return 1; echo 8 > $S/ivh_max_concurrent ;;
  esac
  bpftool map lookup name ivh_cfg key 0 0 0 0 2>/dev/null \
    | grep -q "\"value\": $(cat $S/ivh_cap_source)" \
    || { log "FATAL: ivh_cfg mismatch -- selector reads flat capacity"; return 1; }
  return 0
}
have(){ local n; n=$(grep -c "^$1,$2," "$OUT" 2>/dev/null); n=${n:-0}; [ "$n" -ge "$3" ] 2>/dev/null; }
summarize(){
  python3 - "$OUT" > "$SUM" <<'PY'
import sys,csv,statistics as st,os
p=sys.argv[1]
if not os.path.exists(p): sys.exit()
rows=list(csv.DictReader(open(p)))
combos=[]
for r in rows:
    if r['combo'] not in combos: combos.append(r['combo'])
print(f"{'combo':20}{'n':>3}{'PV':>13}{'IVH':>13}{'change':>10}{'migs':>10}")
print("-"*69)
for c in combos:
    g=lambda a:[float(x['avg']) for x in rows if x['combo']==c and x['arm']==a and float(x['avg'])>0]
    mg=[float(x['migs']) for x in rows if x['combo']==c and x['arm']=='ivh']
    pv,iv=g('pv'),g('ivh')
    if not(pv and iv): continue
    d=100*(st.median(iv)-st.median(pv))/st.median(pv)
    star=" <<<" if d>=5 else ""
    print(f"{c:20}{len(pv):>3}{st.median(pv):>13,.0f}{st.median(iv):>13,.0f}{d:>+9.2f}%{st.median(mg) if mg else 0:>10,.0f}{star}")
print("\nSept campaign: wis_lock1 -9.9% (0/3), wis_lock2 -9.1% (0/3), wis_mmap2 +11.2% (8/8) -- all PINNED")
PY
}
[ -f "$OUT" ] || echo "combo,arm,rep,avg,migs" > "$OUT"
rm -f "$DONE"
log "wislock start: ${#COMBOS[@]} combos x {pv,ivh} x $REPS reps"
for c in "${COMBOS[@]}"; do
  IFS='|' read -r name cmd <<< "$c"
  need=1; for a in pv ivh; do have "$name" "$a" "$REPS" || need=0; done
  [ "$need" = 1 ] && { log "skip $name"; continue; }
  for r in $(seq 1 "$REPS"); do
    for i in 0 1; do
      a=$([ $(( (r+i) % 2 )) -eq 0 ] && echo pv || echo ivh)
      have "$name" "$a" "$REPS" && continue
      setarm "$a" || continue
      sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      m0=$(python3 $T/migcount.py 2>/dev/null||echo 0)
      v=$( cd $W && eval "$cmd" 2>&1 | grep -oP 'average:\K[0-9]+' | tail -1 )
      m1=$(python3 $T/migcount.py 2>/dev/null||echo 0)
      echo "$name,$a,$r,${v:-0},$((m1-m0))" >> "$OUT"
    done
  done
  summarize; log "done $name"
done
summarize
bash $T/pvbase.sh >/dev/null 2>&1
date > "$DONE"
log "WISLOCK-DONE -> $OUT   summary: $SUM"
