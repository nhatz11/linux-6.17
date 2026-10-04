#!/bin/bash
# ebizzy_rcu_ab.sh -- pv vs migration-with-guard vs migration-without-guard.
#
# p7v2_arm.sh sets ivh_rcu_guard=0, justified in its comment for NHextend. For
# ebizzy that setting is the ILLEGAL configuration G-LOCK-40 exists to prevent:
# 100% of ebizzy's migration candidates sit inside an RCU read-side critical
# section (396,962/396,962 measured 2026-10-02, depth 2 dominant), and
# bpf_sched_pre_lock_migrate() then does a GFP_KERNEL allocation and a blocking
# wait_for_completion() inside that reader.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
LOCK=/var/lock/ivh_clean_check.lock
exec 9>"$LOCK"; flock -n 9 || { echo "FATAL: lock held"; exit 1; }
REPS="${1:-4}"
CMD='/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304'
OUT=/root/ivh_logs/ebizzy_rcu_ab_$(date +%m%d-%H%M%S).tsv
printf "arm\trep\trecords\tmigdone\tspin_ns\n" > "$OUT"
mig(){ python3 $T/migcount.py 2>/dev/null || echo 0; }
sp(){ python3 $T/read_ivh_counters.py ivh_slowpath_wait_ns ivh_slowpath_halt_ns 2>/dev/null \
      | awk -F= '{gsub(/ /,"",$2); s+=($1~/wait/? $2 : -$2)} END{print s}'; }

bash $T/p7v2_arm.sh 750000 >/dev/null 2>&1; sleep 1
echo "(warm-up)"; timeout 300 bash -c "$CMD" >/dev/null 2>&1 9>&-

for rep in $(seq 1 $REPS); do
  for arm in pv mig_guard1 mig_guard0; do
    case $arm in
      pv)         bash $T/pvbase.sh >/dev/null 2>&1 ;;
      mig_guard1) bash $T/p7v2_arm.sh 750000 >/dev/null 2>&1; echo 1 > $S/ivh_rcu_guard ;;
      mig_guard0) bash $T/p7v2_arm.sh 750000 >/dev/null 2>&1; echo 0 > $S/ivh_rcu_guard ;;
    esac
    g=$(cat $S/ivh_rcu_guard); sleep 1
    m0=$(mig); s0=$(sp)
    out=$(timeout 300 bash -c "$CMD" 2>&1 9>&-)
    m1=$(mig); s1=$(sp)
    v=$(echo "$out" | grep -oP '^\K[0-9]+(?= records/s)' | head -1)
    printf "%s\t%s\t%s\t%s\t%s\n" "$arm" "$rep" "${v:-NA}" "$((m1-m0))" "$((s1-s0))" >> "$OUT"
    echo "  rep$rep $arm (guard=$g) records=${v:-NA} migrations=$((m1-m0))"
  done
done
echo "DONE $OUT"
python3 - "$OUT" <<'PY'
import sys,statistics as st
rows=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
d={}
for a,r,v,mg,sp in rows:
    if v=='NA': continue
    d.setdefault(a,{'v':[],'m':[],'s':[]})
    d[a]['v'].append(float(v)); d[a]['m'].append(int(mg)); d[a]['s'].append(int(sp)/1e9)
base=st.mean(d['pv']['v'])
print(f"\n{'arm':12s} {'records/s':>11s} {'CV':>7s} {'vs PV':>9s} {'migrations':>12s} {'mig CV':>8s} {'spin s':>8s}")
for a in ('pv','mig_guard1','mig_guard0'):
    if a not in d: continue
    v,mg,sp=d[a]['v'],d[a]['m'],d[a]['s']
    cv=100*st.stdev(v)/st.mean(v) if len(v)>1 else 0
    mcv=100*st.stdev(mg)/st.mean(mg) if len(mg)>1 and st.mean(mg) else 0
    print(f"{a:12s} {st.mean(v):11.1f} {cv:6.1f}% {100*(st.mean(v)-base)/base:+8.2f}% "
          f"{st.mean(mg):12.1f} {mcv:7.1f}% {st.mean(sp):8.4f}")
    print(f"             reps: {[int(x) for x in v]}  migs: {mg}")
PY
