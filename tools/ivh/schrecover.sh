#!/bin/bash
# schrecover.sh -- can schbench be brought back to a relevant (4-5%) gain?
#
# The campaign recorded +7.4% with the arm `spin_mode 2` + universal_eligible=1
# and NOTHING else (run_campaign.sh:36-37): tier2 inert at the 5 ms shipped
# beat_threshold, no HEH, no skip, no head bypass, tlt=4ms, cap=8.
# The rcuknob run gave only +0.32% -- but that used the FULL stack at
# tlt=8ms/cap=8, which points 7 and 8 both showed is schbench's WORST corner
# (it prefers tlt=500us, +2.40%, and cap=1, +2.25%).
#
# Arms (all interleaved, no probes, RCU guard left at its safe default):
#   pv            stock PV
#   camp          the campaign arm verbatim: mig + tier1 only
#   camp_tight    campaign arm at schbench's own optimum: tlt=500us, cap=1
#   full_tight    full stack at tlt=500us, cap=1
set -u
S=/proc/sys/kernel
T=/root/ivh_tools
REPS="${REPS:-8}"
OUT="${OUT:-$T/schrec_$(date +%m%d-%H%M%S).csv}"
CMD="/root/bench/schbench/schbench -m 2 -t 8 -r 15"
EXT="grep -oP 'average rps:\s*\K[0-9.]+'"
R="python3 $T/read_ivh_counters.py"
CTRS="ivh_slowpath_wait_ns ivh_beat_tier1_fired ivh_beat_tier2_fired ivh_cs_head_bailed ivh_head_bypass_fired ivh_evict_marked"

camparm(){   # the campaign's ivh arm, verbatim, plus the two knobs it implied
  /root/spin_mode 2 >/dev/null || return 1
  echo 1 > $S/ivh_universal_eligible
  echo 2 > $S/ivh_pv_preempt_src; echo 2 > $S/ivh_preempt_event_source
  echo 1 > $S/ivh_slowpath_wait_measure
  for k in ivh_cs_head_bail ivh_cs_head_probe ivh_cs_owner_enable ivh_cs_owner_clear \
           ivh_cs_owner_fast ivh_cs_scan ivh_cs_criterion ivh_pv_evict_enable \
           ivh_pv_evict_node_stamp ivh_pv_evict_lookahead ivh_pv_requeue_nosteal \
           ivh_head_bypass_enable ivh_head_bypass_probe ivh_head_bypass_runs \
           ivh_pv_tier1_halt_min ivh_pv_trylock_relaxed ivh_pv_skip_point; do
    echo 0 > $S/$k 2>/dev/null; done
  [ "$(cat $S/ivh_pv_beat_threshold)" = 11000000 ] || return 1   # tier2 inert, as recorded
  [ "$(cat $S/ivh_pv_tier1_enable)" = 1 ] || return 1
}
setarm(){
  case "$1" in
    pv)         bash $T/pvbase.sh >/dev/null || return 1 ;;
    camp)       camparm || return 1; echo 4000000 > $S/ivh_time_left_threshold_ns; echo 8 > $S/ivh_max_concurrent ;;
    camp_tight) camparm || return 1; echo  500000 > $S/ivh_time_left_threshold_ns; echo 1 > $S/ivh_max_concurrent ;;
    full_tight) bash $T/p78_arm.sh tlt 500000 >/dev/null || return 1; echo 1 > $S/ivh_max_concurrent ;;
    mig_only_tight) # migration on the STOCK PV lock path: adaptive_mode=0, so none of
            # the 2107 lines qspinlock_paravirt.h gained since G-LOCK-30 execute.
            # ivh_pre_lock() has no adaptive_mode gate, so migration still runs.
            bash $T/pvbase.sh >/dev/null || return 1
            echo 1 > $S/ivh_universal_eligible
            echo 2 > $S/ivh_pv_preempt_src; echo 2 > $S/ivh_preempt_event_source
            echo 500000 > $S/ivh_time_left_threshold_ns; echo 1 > $S/ivh_max_concurrent
            [ "$(cat $S/ivh_adaptive_mode)" = 0 ] || return 1 ;;
  esac
  echo 1 > $S/ivh_rcu_guard
  return 0
}
ARMS=(pv camp_tight mig_only_tight); N=${#ARMS[@]}
echo "arm,rep,rps,dur_s,migs,wait_ns,t1,t2,heh,hb,evict" > "$OUT"
echo "== schrecover: $N arms x $REPS reps -> $OUT"
for r in $(seq 1 "$REPS"); do
  off=$(( (r-1) % N ))
  for i in $(seq 0 $((N-1))); do
    a=${ARMS[$(( (i+off) % N ))]}
    setarm "$a" || { echo "  !! $a failed"; continue; }
    bpftool map lookup name ivh_cfg key 0 0 0 0 2>/dev/null | grep -q "\"value\": $(cat $S/ivh_cap_source)" \
      || { echo "FATAL: ivh_cfg mismatch -- selector would read flat capacity"; exit 1; }
    sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
    $R $CTRS > /tmp/s0.$$ 2>/dev/null; m0=$(python3 $T/migcount.py 2>/dev/null||echo 0)
    t0=$(date +%s.%N); v=$( eval "$CMD" 2>&1 | eval "$EXT" | tail -1 ); t1=$(date +%s.%N)
    m1=$(python3 $T/migcount.py 2>/dev/null||echo 0); $R $CTRS > /tmp/s1.$$ 2>/dev/null
    python3 - "$a" "$r" "${v:-0}" "$t0" "$t1" "$((m1-m0))" /tmp/s0.$$ /tmp/s1.$$ "$OUT" <<'PY'
import sys,re
a,r,v,t0,t1,mig,f0,f1,out=sys.argv[1:10]
c=lambda p:{m.group(1):int(m.group(2)) for m in (re.match(r'\s*(\S+)\s*=\s*(\d+)',l) for l in open(p)) if m}
c0,c1=c(f0),c(f1); D=lambda k:c1.get(k,0)-c0.get(k,0)
open(out,'a').write(f"{a},{r},{v},{float(t1)-float(t0):.3f},{mig},{D('ivh_slowpath_wait_ns')},"
 f"{D('ivh_beat_tier1_fired')},{D('ivh_beat_tier2_fired')},{D('ivh_cs_head_bailed')},"
 f"{D('ivh_head_bypass_fired')},{D('ivh_evict_marked')}\n")
print(f"  {a:>11} r{r} rps={v:>9} migs={mig:>7} wait={D('ivh_slowpath_wait_ns')/1e9:5.2f}s "
      f"t1={D('ivh_beat_tier1_fired'):>7} t2={D('ivh_beat_tier2_fired'):>6} heh={D('ivh_cs_head_bailed'):>5} hb={D('ivh_head_bypass_fired'):>4}")
PY
    rm -f /tmp/s0.$$ /tmp/s1.$$
  done
done
python3 - "$OUT" <<'PY'
import sys,csv,statistics as st
rows=list(csv.DictReader(open(sys.argv[1])))
g=lambda a,k='rps':[float(r[k]) for r in rows if r['arm']==a and float(r['rps'])>0]
pv=g('pv'); print()
for a in ['pv','camp_tight','mig_only_tight']:
    v=g(a)
    if not v: continue
    d=f"{100*(st.median(v)-st.median(pv))/st.median(pv):+7.2f}%" if a!='pv' else "   --   "
    print(f"  {a:11} n={len(v)} median={st.median(v):8.1f} {d}  CV={100*st.pstdev(v)/st.mean(v):4.1f}%  migs={st.median(g(a,'migs')):>8,.0f}")
print("  campaign reference (schbench): pv 3170.7  ivh 3390.1  -> +6.9% (recorded +7.4%)")
PY
bash /root/ivh_tools/pvbase.sh >/dev/null 2>&1
echo SCHREC-DONE
