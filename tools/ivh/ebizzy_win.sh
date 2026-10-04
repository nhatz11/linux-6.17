#!/bin/bash
# ebizzy_win.sh -- reproduce noprobe.sh's ebizzy result with the REAL arm.
#
# noprobe_0929-194420.csv, probe OFF: pv 939/946/969 (mean 951),
# best 1387/1399/1347 (mean 1378) = +44.9%. That "best" arm is
#   p78_arm.sh tlt $TLT   (TLT default in noprobe.sh is 8000000)  + mc 8
# which is the FULL STACK -- 24 asserted knobs, spin_mode 2.
#
# My earlier reconstruction of "full stack" was crippled and that is why it read
# flat. It omitted, among others:
#   ivh_pv_beat_threshold=2200000  -- at the shipped 5ms, tier2 AND head bypass
#                                     fire ZERO (eval_final 11.1)
#   ivh_head_bypass_probe=1        -- enable=1 alone fires zero
#   the whole head-early-halt block (cs_owner_*, cs_scan, cs_criterion,
#                                     cs_head_probe, cs_head_bail)
#   ivh_pv_evict_node_stamp / requeue_max / evict_threshold / bypass_max
# Fire counters are asserted below so an inert mechanism cannot pass silently.
#
# Methodology copied from noprobe.sh: sync + drop_caches + sleep 1 before EVERY
# run, order rotated per rep.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
LOCK=/var/lock/ivh_clean_check.lock
exec 9>"$LOCK"; flock -n 9 || { echo "FATAL: lock held"; exit 1; }
TLT="${TLT:-8000000}"; MC="${MC:-8}"; REPS="${1:-4}"
OUT=/root/ivh_logs/ebizzy_win_$(date +%m%d-%H%M%S).tsv
CMD='/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304'
mig(){ python3 $T/migcount.py 2>/dev/null || echo 0; }
fire(){ python3 $T/read_ivh_counters.py ivh_beat_tier2_fired ivh_cs_head_bailed ivh_rot_handoffs 2>/dev/null \
        | awk -F= '{gsub(/ /,"",$1);gsub(/ /,"",$2);printf "%s=%s ",$1,$2}'; }

setarm(){ case "$1" in
  pv)   bash $T/pvbase.sh >/dev/null || return 1 ;;
  best) bash $T/p78_arm.sh tlt "$TLT" >/dev/null || return 1
        echo "$MC" > $S/ivh_max_concurrent
        [ "$(cat $S/ivh_max_concurrent)" = "$MC" ] || { echo "FATAL: mc"; return 1; } ;;
  migonly) bash $T/p7v2_arm.sh "$TLT" >/dev/null || return 1 ;;
esac; sleep 1; }

printf "arm\trep\trecords\tmigrations\n" > "$OUT"
echo "== ebizzy_win: TLT=$TLT MC=$MC reps=$REPS -> $OUT"
echo "   selector links: $(bpftool link list 2>/dev/null | grep -c 'target_btf_id')"
ARMS=(pv best migonly)
for rep in $(seq 1 $REPS); do
  off=$(( (rep-1) % 3 ))
  for i in 0 1 2; do
    a=${ARMS[$(( (i+off) % 3 ))]}
    setarm "$a" || { echo "  ARMFAIL $a"; continue; }
    sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
    m0=$(mig); f0=$(fire)
    v=$(timeout 300 bash -c "$CMD" 2>/dev/null 9>&- | grep -oP '^\K[0-9]+(?= records/s)' | head -1)
    m1=$(mig); f1=$(fire)
    printf "%s\t%s\t%s\t%s\n" "$a" "$rep" "${v:-NA}" "$((m1-m0))" >> "$OUT"
    echo "  rep$rep $a = ${v:-NA} records/s  migs=$((m1-m0))"
    [ "$a" = best ] && { echo "      fire before: $f0"; echo "      fire after : $f1"; }
  done
done
echo "DONE $OUT"
python3 - "$OUT" <<'PY'
import sys,statistics as st
rows=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
d={}
for a,r,v,mg in rows:
    if v=='NA': continue
    d.setdefault(a,{'v':[],'m':[]}); d[a]['v'].append(float(v)); d[a]['m'].append(int(mg))
base=st.mean(d['pv']['v'])
print(f"\n{'arm':9s} {'records/s':>10s} {'CV':>7s} {'vs PV':>9s} {'migrations':>11s}")
for a in ('pv','best','migonly'):
    if a not in d: continue
    v,mg=d[a]['v'],d[a]['m']
    cv=100*st.stdev(v)/st.mean(v) if len(v)>1 else 0
    print(f"{a:9s} {st.mean(v):10.1f} {cv:6.1f}% {100*(st.mean(v)-base)/base:+8.2f}% {st.mean(mg):11.1f}")
    print(f"          reps {[int(x) for x in v]}  migs {mg}")
print("\nreference (noprobe_0929-194420.csv, probe OFF): pv 951, best 1378 = +44.9%")
PY
