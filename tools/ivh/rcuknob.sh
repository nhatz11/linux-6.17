#!/bin/bash
# rcuknob.sh -- does the G-LOCK-40 RCU guard account for the halved IVH win?
#
# Three arms, INTERLEAVED on ONE boot, no probes:
#   pv       stock PV (pvbase.sh)
#   guard1   migration + full stack, ivh_rcu_guard=1  -- shipping behaviour
#   guard0   migration + full stack, ivh_rcu_guard=0  -- pre-G-LOCK-40, UNSAFE
#
# Prediction from the 2026-09-29 investigation: ebizzy_mmap recovers from ~1420
# toward the campaign's 2008.5, because 91.9% of its migration candidates sit
# inside an RCU reader (92% are __pte_offset_map_lock on the anonymous
# page-fault path, which holds rcu_read_lock by construction). hackbench should
# barely move: only 0.9% of its candidates are in a reader.
#
# SAFETY: guard=0 permits a GFP_KERNEL allocation and a wait_for_completion()
# inside a preemptible-RCU reader. DEBUG_ATOMIC_SLEEP and PROVE_LOCKING are off
# so nothing warns; the symptom is RCU stalls under memory pressure. It is set
# ONLY around a measured run and restored to 1 immediately after, including on
# any early exit.
set -u
S=/proc/sys/kernel
T=/root/ivh_tools
source $T/suite14.sh
R="python3 $T/read_ivh_counters.py"
REPS="${REPS:-5}"
WL="${WL:-ebizzy_mmap}"
OUT="${OUT:-$T/rcuknob_$(date +%m%d-%H%M%S).csv}"
CTRS="ivh_slowpath_wait_ns ivh_prelock_calls ivh_prelock_cooldown_skipped ivh_steal_imminent_capacity_reject ivh_steal_imminent_time_left_reject"
restore(){ echo 1 > $S/ivh_rcu_guard 2>/dev/null; }
trap restore EXIT INT TERM
[ -e $S/ivh_rcu_guard ] || { echo "FATAL: no ivh_rcu_guard -- not the G-LOCK-49 kernel"; exit 1; }

lookup(){ for e in "${SUITE14[@]}"; do IFS='|' read -r n d m c x <<< "$e"
  [ "$n" = "$1" ] && { D="$d"; M="$m"; C="$c"; X="$x"; return 0; }; done; return 1; }
lookup "$WL" || { echo "FATAL: $WL not in suite"; exit 1; }
[ "$C" = MEMTIER_CMD ] && C="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=10 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"

setarm(){
  case "$1" in
    pv)     bash $T/pvbase.sh >/dev/null || return 1; echo 1 > $S/ivh_rcu_guard ;;
    guard1) bash $T/p78_arm.sh tlt 8000000 >/dev/null || return 1
            echo 8 > $S/ivh_max_concurrent; echo 1 > $S/ivh_rcu_guard ;;
    guard0) bash $T/p78_arm.sh tlt 8000000 >/dev/null || return 1
            echo 8 > $S/ivh_max_concurrent; echo 0 > $S/ivh_rcu_guard ;;
  esac
  local want=1; [ "$1" = guard0 ] && want=0
  [ "$(cat $S/ivh_rcu_guard)" = "$want" ] || { echo "FATAL: rcu_guard=$(cat $S/ivh_rcu_guard) want $want"; return 1; }
  return 0
}
ARMS=(pv guard1 guard0)
echo "workload,arm,rep,perf,dur_s,migs,wait_ns,prelock,cooldown_skipped,g1rej,g2rej" > "$OUT"
echo "== rcuknob: $WL, 3 arms x $REPS reps, NO probes -> $OUT"
for r in $(seq 1 "$REPS"); do
  off=$(( (r-1) % 3 ))
  for i in 0 1 2; do
    a=${ARMS[$(( (i+off) % 3 ))]}
    setarm "$a" || { restore; continue; }
    bpftool link list 2>/dev/null | grep -q "target_btf_id" || { echo "FATAL: selector gone"; restore; exit 1; }
    prep14 "$WL" >/dev/null 2>&1 || { restore; continue; }
    sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
    $R $CTRS > /tmp/k0.$$ 2>/dev/null; m0=$(python3 $T/migcount.py 2>/dev/null||echo 0)
    t0=$(date +%s.%N)
    if [ "$M" = TIME ]; then ( cd "$D" && eval "$C" ) >/dev/null 2>&1; v=""
    else v=$( ( cd "$D" && eval "$C" 2>&1 ) | eval "$X" | tail -1 ); fi
    t1=$(date +%s.%N)
    m1=$(python3 $T/migcount.py 2>/dev/null||echo 0); $R $CTRS > /tmp/k1.$$ 2>/dev/null
    echo 1 > $S/ivh_rcu_guard        # restore immediately after the measured run
    python3 - "$WL" "$a" "$r" "${v:-}" "$t0" "$t1" "$((m1-m0))" /tmp/k0.$$ /tmp/k1.$$ "$OUT" <<'PY'
import sys,re
wl,a,r,v,t0,t1,mig,f0,f1,out=sys.argv[1:11]
dur=float(t1)-float(t0)
c=lambda p:{m.group(1):int(m.group(2)) for m in (re.match(r'\s*(\S+)\s*=\s*(\d+)',l) for l in open(p)) if m}
c0,c1=c(f0),c(f1); D=lambda k:c1.get(k,0)-c0.get(k,0)
perf=v if v else f"{dur:.4f}"
open(out,'a').write(f"{wl},{a},{r},{perf},{dur:.3f},{mig},{D('ivh_slowpath_wait_ns')},"
  f"{D('ivh_prelock_calls')},{D('ivh_prelock_cooldown_skipped')},"
  f"{D('ivh_steal_imminent_capacity_reject')},{D('ivh_steal_imminent_time_left_reject')}\n")
print(f"  {a:>7} r{r} perf={perf:>12} migs={mig:>8} prelock={D('ivh_prelock_calls'):>12,} wait={D('ivh_slowpath_wait_ns')/1e9:6.2f}s")
PY
    rm -f /tmp/k0.$$ /tmp/k1.$$
  done
done
restore
python3 - "$OUT" <<'PY'
import sys,csv,statistics as st
rows=list(csv.DictReader(open(sys.argv[1])))
g=lambda a,k='perf':[float(r[k]) for r in rows if r['arm']==a]
pv=g('pv')
print()
for a in ['pv','guard1','guard0']:
    v=g(a)
    if not v: continue
    d=f"{100*(st.median(v)-st.median(pv))/st.median(pv):+7.2f}%" if a!='pv' and pv else "   --   "
    print(f"  {a:7} n={len(v)} median={st.median(v):9.1f} {d}  CV={100*st.pstdev(v)/st.mean(v):4.1f}%  "
          f"migs={st.median(g(a,'migs')):>9,.0f}  prelock={st.median(g(a,'prelock')):>13,.0f}")
if g('guard1') and g('guard0'):
    print(f"\n  guard0 vs guard1: {100*(st.median(g('guard0'))-st.median(g('guard1')))/st.median(g('guard1')):+.2f}%"
          f"   prelock ratio {st.median(g('guard0','prelock'))/max(st.median(g('guard1','prelock')),1):.1f}x")
print("  campaign reference (ebizzy_mmap): pv 978.0  ivh 2008.5  -> +104.3%")
PY
echo "rcu_guard restored to $(cat /proc/sys/kernel/ivh_rcu_guard)"
echo RCUKNOB-DONE
