#!/bin/bash
# ebizzy_tlt_sweep.sh -- is ivh_time_left_threshold_ns the whole story?
#
# 2026-10-02: ebizzy read -2.5% at THRESH=750000 (clean_one.sh's default, which
# is NOT a validated value) and +38% at 8000000, with migrations going
# 1,300 -> 32,966. p78_arm.sh's own default is 4000000 and noprobe.sh used
# 8000000; 750000 appears nowhere in the validated arms. Gate 2 rejects when
# time_left > threshold, so a SMALL threshold is the RESTRICTIVE one -- at
# 750000 the cumulative reject rate was 82.7% of consultations.
#
# Migration-only (p7v2_arm.sh), so this isolates Gate 2 from the AS stack.
# drop_caches before every run, per noprobe.sh.
set -u
T=/root/ivh_tools
LOCK=/var/lock/ivh_clean_check.lock
exec 9>"$LOCK"; flock -n 9 || { echo "FATAL: lock held"; exit 1; }
REPS="${1:-3}"
OUT=/root/ivh_logs/ebizzy_tlt_$(date +%m%d-%H%M%S).tsv
CMD='/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304'
mig(){ python3 $T/migcount.py 2>/dev/null || echo 0; }
g2(){ python3 $T/read_ivh_counters.py ivh_g2_eval ivh_steal_imminent_time_left_reject 2>/dev/null \
      | awk -F= '{gsub(/ /,"",$2); printf "%s ",$2}'; }
ARMS="pv 750000 2000000 4000000 8000000 16000000"
printf "arm\trep\trecords\tmigrations\tg2_eval\tg2_reject\n" > "$OUT"
echo "== ebizzy TLT sweep, migration-only, reps=$REPS -> $OUT"
for rep in $(seq 1 $REPS); do
  for a in $ARMS; do
    if [ "$a" = pv ]; then bash $T/pvbase.sh >/dev/null 2>&1 || continue
    else bash $T/p7v2_arm.sh "$a" >/dev/null 2>&1 || { echo "  ARMFAIL $a"; continue; }; fi
    sleep 1; sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
    m0=$(mig); set -- $(g2); e0=$1; r0=$2
    v=$(timeout 300 bash -c "$CMD" 2>/dev/null 9>&- | grep -oP '^\K[0-9]+(?= records/s)' | head -1)
    m1=$(mig); set -- $(g2); e1=$1; r1=$2
    printf "%s\t%s\t%s\t%s\t%s\t%s\n" "$a" "$rep" "${v:-NA}" "$((m1-m0))" "$((e1-e0))" "$((r1-r0))" >> "$OUT"
    de=$((e1-e0)); dr=$((r1-r0))
    pc=$([ "$de" -gt 0 ] && python3 -c "print(f'{100*$dr/$de:.1f}%')" || echo "n/a")
    echo "  rep$rep tlt=$a records=${v:-NA} migs=$((m1-m0)) gate2_reject=$pc"
  done
done
echo "DONE $OUT"
python3 - "$OUT" <<'PY'
import sys,statistics as st
rows=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
d={}
for a,r,v,mg,ev,rj in rows:
    if v=='NA': continue
    d.setdefault(a,{'v':[],'m':[],'e':[],'r':[]})
    d[a]['v'].append(float(v)); d[a]['m'].append(int(mg))
    d[a]['e'].append(int(ev)); d[a]['r'].append(int(rj))
base=st.mean(d['pv']['v'])
print(f"\n{'tlt':>10s} {'records/s':>10s} {'CV':>7s} {'vs PV':>9s} {'migrations':>12s} {'G2 reject':>10s}")
for a in ('pv','750000','2000000','4000000','8000000','16000000'):
    if a not in d: continue
    v,mg,e,r=d[a]['v'],d[a]['m'],d[a]['e'],d[a]['r']
    cv=100*st.stdev(v)/st.mean(v) if len(v)>1 else 0
    rj=f"{100*st.mean(r)/st.mean(e):.1f}%" if st.mean(e) else "n/a"
    print(f"{a:>10s} {st.mean(v):10.1f} {cv:6.1f}% {100*(st.mean(v)-base)/base:+8.2f}% {st.mean(mg):12.1f} {rj:>10s}")
    print(f"           reps {[int(x) for x in v]}")
PY
