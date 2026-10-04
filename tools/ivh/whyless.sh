#!/bin/bash
# whyless.sh -- why is the IVH arm 31% below the September campaign?
#
# The campaign (kernel 6.17.0-G-LOCK-30-csfast+) recorded ebizzy_mmap
# pv=978.0 ivh=2008.5 (+104.3%). Today (G-LOCK-48-skipcheck) the same workload
# reads pv=946.0 full-stack=1387.0 (+46.6%). PV barely moved (-3.3%); the IVH
# arm fell 30.9%. Two candidate causes, and this separates them:
#
#   A pv       stock PV                            (pvbase.sh)
#   B mig_t1   EXACTLY the campaign's ivh arm:     spin_mode 2 + universal_eligible=1
#              and nothing else. spin_mode 2 leaves beat_threshold=11000000 (5 ms)
#              where tier 2 fires ZERO, and never touches cs_head_bail,
#              evict_enable or head_bypass_probe. So this is migration + tier 1.
#   C full     migration + t1 + HEH + t2(1 ms) + skip + head bypass, tlt=8ms mc=8
#
# If B ~ 2008 and C ~ 1387 -> the added mechanisms cost the win (CONFIG).
# If B ~ C ~ 1387          -> something regressed in G-LOCK 40-48 (KERNEL).
# No probes in any arm.
set -u
S=/proc/sys/kernel
T=/root/ivh_tools
source $T/suite14.sh
R="python3 $T/read_ivh_counters.py"
REPS="${REPS:-5}"
WL="${WL:-ebizzy_mmap}"
OUT="${OUT:-$T/whyless_$(date +%m%d-%H%M%S).csv}"
CTRS="ivh_slowpath_wait_ns ivh_node_spin_iters_sum ivh_node_spin_success_iters_sum ivh_head_spin_iters_sum ivh_head_spin_iters_bail_sum ivh_beat_tier1_fired ivh_beat_tier2_fired ivh_cs_head_bailed ivh_head_bypass_fired ivh_evict_marked"

setarm(){
  case "$1" in
    pv)     bash $T/pvbase.sh >/dev/null || return 1 ;;
    mig_t1) /root/spin_mode 2 >/dev/null || return 1
            echo 1 > $S/ivh_universal_eligible
            # spin_mode 2 does NOT clear these; the campaign never had them set
            # (fresh boot, defaults 0), so zero them to reproduce its EFFECTIVE state
            for k in ivh_cs_head_bail ivh_cs_head_probe ivh_cs_owner_enable \
                     ivh_cs_owner_clear ivh_cs_owner_fast ivh_cs_scan ivh_cs_criterion \
                     ivh_pv_evict_enable ivh_pv_evict_node_stamp ivh_pv_evict_lookahead \
                     ivh_pv_requeue_nosteal ivh_head_bypass_enable ivh_head_bypass_probe \
                     ivh_head_bypass_runs ivh_pv_tier1_halt_min ivh_pv_trylock_relaxed \
                     ivh_pv_skip_point; do echo 0 > $S/$k 2>/dev/null; done
            echo 2 > $S/ivh_pv_preempt_src; echo 2 > $S/ivh_preempt_event_source
            echo 1 > $S/ivh_slowpath_wait_measure
            # assert this really is mig+t1: the three extras OFF, tier2 gate shut
            for k in ivh_cs_head_bail ivh_cs_head_probe ivh_pv_evict_enable ivh_head_bypass_probe; do
              [ "$(cat $S/$k)" = 0 ] || { echo "FATAL: $k=$(cat $S/$k) want 0"; return 1; }; done
            [ "$(cat $S/ivh_pv_beat_threshold)" = 11000000 ] || { echo "FATAL: beat_threshold not 5ms"; return 1; }
            echo 1 > $S/ivh_cs_track_enabled
            [ "$(cat $S/ivh_pv_tier1_enable)" = 1 ] || { echo "FATAL: tier1 off"; return 1; } ;;
    mig_only) # MIGRATION ON THE STOCK PV LOCK PATH: adaptive_mode=0, so none of
            # the 2107 lines qspinlock_paravirt.h gained since G-LOCK-30 run.
            # ivh_pre_lock() has NO adaptive_mode gate, so migration still works.
            bash $T/pvbase.sh >/dev/null || return 1
            echo 1 > $S/ivh_universal_eligible
            echo 2 > $S/ivh_pv_preempt_src; echo 2 > $S/ivh_preempt_event_source
            [ "$(cat $S/ivh_adaptive_mode)" = 0 ] || { echo "FATAL: mode not 0"; return 1; }
            [ "$(cat $S/ivh_universal_eligible)" = 1 ] || { echo "FATAL: not eligible"; return 1; } ;;
    mig_t1_nocs) /root/spin_mode 2 >/dev/null || return 1
            echo 1 > $S/ivh_universal_eligible
            echo 2 > $S/ivh_pv_preempt_src; echo 2 > $S/ivh_preempt_event_source
            echo 1 > $S/ivh_slowpath_wait_measure
            for k in ivh_cs_head_bail ivh_cs_head_probe ivh_cs_owner_enable \
                     ivh_cs_owner_clear ivh_cs_owner_fast ivh_cs_scan ivh_cs_criterion \
                     ivh_pv_evict_enable ivh_pv_evict_node_stamp ivh_pv_evict_lookahead \
                     ivh_pv_requeue_nosteal ivh_head_bypass_enable ivh_head_bypass_probe \
                     ivh_head_bypass_runs ivh_pv_tier1_halt_min ivh_pv_trylock_relaxed \
                     ivh_pv_skip_point; do echo 0 > $S/$k 2>/dev/null; done
            # THE VARIABLE: cs_enter/cs_exit did not exist at G-LOCK-30, so
            # current->last_cs_ns was permanently 0 and Gate 2 saw time_left=runway.
            echo 0 > $S/ivh_cs_track_enabled
            [ "$(cat $S/ivh_cs_track_enabled)" = 0 ] || { echo "FATAL: cs_track still on"; return 1; } ;;
    full)   bash $T/p78_arm.sh tlt 8000000 >/dev/null || return 1
            echo 8 > $S/ivh_max_concurrent ;;
  esac
  return 0
}
lookup(){ for e in "${SUITE14[@]}"; do IFS='|' read -r n d m c x <<< "$e"
  [ "$n" = "$1" ] && { D="$d"; M="$m"; C="$c"; X="$x"; return 0; }; done; return 1; }
lookup "$WL" || { echo "FATAL: $WL not in suite"; exit 1; }
[ "$C" = MEMTIER_CMD ] && C="/root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=10 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"

ARMS=(pv mig_only mig_t1 full)
echo "workload,arm,rep,perf,dur_s,migs,wait_ns,node_iters,head_iters,t1,t2,heh,hb,evict" > "$OUT"
echo "== whyless: $WL, 3 arms x $REPS reps, NO probes -> $OUT"
for r in $(seq 1 "$REPS"); do
  off=$(( (r-1) % 4 ))
  for i in 0 1 2 3; do
    a=${ARMS[$(( (i+off) % 4 ))]}
    setarm "$a" || { echo "  !! arm $a failed"; continue; }
    bpftool link list 2>/dev/null | grep -q "target_btf_id 66718" || { echo "FATAL: selector gone"; exit 1; }
    prep14 "$WL" >/dev/null 2>&1 || continue
    sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
    $R $CTRS > /tmp/w0.$$ 2>/dev/null; m0=$(python3 $T/migcount.py 2>/dev/null||echo 0)
    t0=$(date +%s.%N)
    if [ "$M" = TIME ]; then ( cd "$D" && eval "$C" ) >/dev/null 2>&1; v=""
    else v=$( ( cd "$D" && eval "$C" 2>&1 ) | eval "$X" | tail -1 ); fi
    t1=$(date +%s.%N)
    m1=$(python3 $T/migcount.py 2>/dev/null||echo 0); $R $CTRS > /tmp/w1.$$ 2>/dev/null
    python3 - "$WL" "$a" "$r" "${v:-}" "$t0" "$t1" "$((m1-m0))" /tmp/w0.$$ /tmp/w1.$$ "$OUT" <<'PY'
import sys,re
wl,a,r,v,t0,t1,mig,f0,f1,out=sys.argv[1:11]
dur=float(t1)-float(t0)
c=lambda p:{m.group(1):int(m.group(2)) for m in (re.match(r'\s*(\S+)\s*=\s*(\d+)',l) for l in open(p)) if m}
c0,c1=c(f0),c(f1); D=lambda k:c1.get(k,0)-c0.get(k,0)
perf=v if v else f"{dur:.4f}"
open(out,'a').write(f"{wl},{a},{r},{perf},{dur:.3f},{mig},{D('ivh_slowpath_wait_ns')},"
 f"{D('ivh_node_spin_iters_sum')+D('ivh_node_spin_success_iters_sum')},"
 f"{D('ivh_head_spin_iters_sum')+D('ivh_head_spin_iters_bail_sum')},"
 f"{D('ivh_beat_tier1_fired')},{D('ivh_beat_tier2_fired')},{D('ivh_cs_head_bailed')},"
 f"{D('ivh_head_bypass_fired')},{D('ivh_evict_marked')}\n")
print(f"  {a:>8} r{r} perf={perf:>12} migs={mig:>7} wait={D('ivh_slowpath_wait_ns')/1e9:7.3f}s "
      f"t1={D('ivh_beat_tier1_fired'):>7} t2={D('ivh_beat_tier2_fired'):>6} heh={D('ivh_cs_head_bailed'):>6} "
      f"hb={D('ivh_head_bypass_fired'):>4} ev={D('ivh_evict_marked'):>5}")
PY
    rm -f /tmp/w0.$$ /tmp/w1.$$
  done
done
bash $T/pvbase.sh >/dev/null 2>&1
echo "WROTE $OUT"; echo WHYLESS-DONE
