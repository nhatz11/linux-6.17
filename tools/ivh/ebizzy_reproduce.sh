#!/bin/bash
# ebizzy_reproduce.sh -- reproduce the campaign's ebizzy win, factor by factor.
#
# The +104.3% reference (g40_sanity.sh REF) came from campaign/fullstack.sh.
# Two things in that script differ from the migration-only arm I measured:
#   1. its ivh arm is the FULL STACK -- spin_mode 2, tier1+tier2, head bypass
#      with runs=1/hold=0, evict+lookahead+nosteal at hop_cap=2;
#   2. it does `sync; echo 3 > /proc/sys/vm/drop_caches` BEFORE EVERY RUN.
# (2) is not cosmetic for ebizzy -m, which mmap/munmaps 4MB chunks: a cold page
# cache means every chunk faults and is zeroed, which is kernel work under
# mmap_lock, which is the lock IVH is supposed to repair. A warm run skips it.
#
# arm() below is COPIED VERBATIM from campaign/fullstack.sh so the comparison is
# against the real thing, not my reconstruction of it.
#
# ivh_rcu_guard is held at 1 (legal) in every arm. The campaign predates
# G-LOCK-40 and so had no guard at all, but a 4-rep A/B on 2026-10-02 put
# guard=1 at -2.89% and guard=0 at -2.40% -- throughput-neutral -- so the
# legitimate setting is used and the historical inflation is not reproduced.
set -u
S=/proc/sys/kernel
LOCK=/var/lock/ivh_clean_check.lock
exec 9>"$LOCK"; flock -n 9 || { echo "FATAL: lock held"; exit 1; }
REPS="${1:-4}"
OUT=/root/ivh_logs/ebizzy_repro_$(date +%m%d-%H%M%S).tsv
mig(){ python3 /root/ivh_tools/migcount.py 2>/dev/null || echo 0; }

arm(){ case $1 in
  pv)  echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1
       echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_pv_evict_enable
       [ "$(cat $S/ivh_adaptive_mode)" = 0 ] || { echo "pv arm FAILED"; exit 1; } ;;
  full) echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1
       echo 2 > $S/ivh_pv_preempt_src
       echo 2 > $S/ivh_preempt_event_source
       echo 1 > $S/ivh_pv_tier1_enable; echo 1 > $S/ivh_pv_tier2_enable
       echo 1 > $S/ivh_head_bypass_enable; echo 1 > $S/ivh_head_bypass_runs; echo 0 > $S/ivh_head_bypass_hold
       echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_lookahead
       echo 1 > $S/ivh_pv_requeue_nosteal; echo 2 > $S/ivh_pv_evict_hop_cap
       [ "$(cat $S/ivh_adaptive_mode)" = 2 ] || { echo "ivh arm FAILED"; exit 1; }
       [ "$(cat $S/ivh_preempt_event_source)" = 2 ] || { echo "gate2 FAILED"; exit 1; } ;;
  mig) echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1
       echo 0 > $S/ivh_head_bypass_enable; echo 0 > $S/ivh_pv_evict_enable
       echo 2 > $S/ivh_pv_preempt_src; echo 2 > $S/ivh_preempt_event_source
       echo 8 > $S/ivh_max_concurrent; echo 1 > $S/ivh_selection_trylock
       echo 1 > $S/ivh_universal_eligible
       [ "$(cat $S/ivh_adaptive_mode)" = 0 ] || { echo "mig arm FAILED"; exit 1; } ;;
esac
echo 1 > $S/ivh_rcu_guard
echo 3 > $S/ivh_cap_source
echo 1 > $S/ivh_cs_track_enabled; echo 1 > $S/ivh_slowpath_wait_measure
sleep 1; }

printf "cache\tarm\trep\trecords\tmigrations\n" > "$OUT"
for rep in $(seq 1 $REPS); do
  for cache in drop nodrop; do
    for a in pv full mig; do
      arm $a
      if [ "$cache" = drop ]; then sync; echo 3 > /proc/sys/vm/drop_caches; sleep 1; fi
      m0=$(mig)
      v=$(timeout 300 /home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304 2>/dev/null 9>&- \
            | grep -oP '^\K[0-9]+(?= records/s)' | head -1)
      m1=$(mig)
      printf "%s\t%s\t%s\t%s\t%s\n" "$cache" "$a" "$rep" "${v:-NA}" "$((m1-m0))" >> "$OUT"
      echo "  rep$rep $cache/$a = ${v:-NA} records/s  migrations=$((m1-m0))"
    done
  done
done
echo "DONE $OUT"
python3 - "$OUT" <<'PY'
import sys,statistics as st
rows=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
d={}
for c,a,r,v,mg in rows:
    if v=='NA': continue
    d.setdefault((c,a),{'v':[],'m':[]}); d[(c,a)]['v'].append(float(v)); d[(c,a)]['m'].append(int(mg))
print(f"\n{'cache':8s} {'arm':6s} {'records/s':>10s} {'CV':>7s} {'vs its PV':>11s} {'migrations':>11s}")
for c in ('drop','nodrop'):
    if (c,'pv') not in d: continue
    base=st.mean(d[(c,'pv')]['v'])
    for a in ('pv','full','mig'):
        if (c,a) not in d: continue
        v,mg=d[(c,a)]['v'],d[(c,a)]['m']
        cv=100*st.stdev(v)/st.mean(v) if len(v)>1 else 0
        print(f"{c:8s} {a:6s} {st.mean(v):10.1f} {cv:6.1f}% {100*(st.mean(v)-base)/base:+10.2f}% {st.mean(mg):11.1f}")
        print(f"                  reps {[int(x) for x in v]}  migs {mg}")
PY
