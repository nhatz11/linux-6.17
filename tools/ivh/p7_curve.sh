#!/bin/bash
# Gate 2 response curve. NO PV ARM -- thresholds are compared against each
# other, which also removes the capacity-settling confound (the PV arm is what
# perturbs ivh_uc_capacity; consecutive IVH arms stay settled).
#
# Prediction under test: reject = (time_left > threshold), so threshold -> 0
# must reject ~everything and migration must STOP. If migrations do not fall
# to zero at threshold 0, the knob is not controlling the decision it appears
# to control.
#
# Measured reject fractions from the 4ms/16ms/250us probe put the time_left
# distribution mostly between 250us and 4ms (53.7% > 250us, 8.0% > 4ms), so
# the discriminating region is BELOW the values swept so far.
set -u
S=/proc/sys/kernel; R="python3 /root/ivh_tools/read_ivh_counters.py"
G(){ $R "$1" 2>/dev/null | grep -oE '[0-9]+$' || echo 0; }
M(){ python3 /root/ivh_tools/migcount.py; }
VALUES="${VALUES:-0 10000 50000 100000 250000 500000 1000000 2000000 4000000 8000000 16000000 64000000}"
REPS="${REPS:-3}"
OUT="${OUT:-/root/ivh_tools/p7curve_$(date +%m%d-%H%M%S).csv}"

/root/spin_mode 2 >/dev/null 2>&1; echo 1 > $S/ivh_universal_eligible
echo 2 > $S/ivh_pv_preempt_src; echo 2 > $S/ivh_preempt_event_source
for k in ivh_pv_tier1_enable ivh_pv_tier2_enable ivh_head_bypass_enable ivh_head_bypass_runs ivh_pv_evict_enable ivh_pv_evict_lookahead ivh_pv_requeue_nosteal; do echo 1 > $S/$k; done
echo 2 > $S/ivh_pv_preempt_src; echo 0 > $S/ivh_head_bypass_hold; echo 2 > $S/ivh_pv_evict_hop_cap
echo 0 > $S/ivh_tks_sampler_ns
[ "$(cat $S/ivh_adaptive_mode)" = 2 ] || { echo "FATAL: IVH arm"; exit 1; }

stress-ng --dentry 16 -t 15s --metrics-brief >/dev/null 2>&1   # warmup, discarded
echo "thresh_ns,rep,perf,g2_evals,g2_reject,reject_pct,migrations,g1_reject" > "$OUT"
printf "%9s %11s %11s %10s %9s %11s\n" thresh perf g2_evals reject_pct migs g1_reject
VA=($VALUES); n=${#VA[@]}
for r in $(seq 1 "$REPS"); do
  off=$(( (r-1) % n )); echo "--- rep $r ---"
  for i in $(seq 0 $((n-1))); do
    t=${VA[$(( (i+off) % n ))]}
    echo "$t" > $S/ivh_time_left_threshold_ns
    [ "$(cat $S/ivh_time_left_threshold_ns)" = "$t" ] || { echo "  !! rejected $t"; continue; }
    sleep 1
    p0=$(G ivh_prelock_calls); c0=$(G ivh_prelock_cooldown_skipped)
    a0=$(G ivh_steal_imminent_capacity_reject); b0=$(G ivh_steal_imminent_time_left_reject); m0=$(M)
    v=$(stress-ng --dentry 16 -t 15s --metrics-brief 2>&1 | grep -oP 'dentry\s+\d+\s+[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+\K[0-9.]+' | tail -1)
    p1=$(G ivh_prelock_calls); c1=$(G ivh_prelock_cooldown_skipped)
    a1=$(G ivh_steal_imminent_capacity_reject); b1=$(G ivh_steal_imminent_time_left_reject); m1=$(M)
    python3 - "$t" "$r" "${v:-0}" "$p0" "$p1" "$c0" "$c1" "$a0" "$a1" "$b0" "$b1" "$m0" "$m1" "$OUT" <<'PY'
import sys
t,r,v,p0,p1,c0,c1,a0,a1,b0,b1,m0,m1,out=sys.argv[1:15]
p=int(p1)-int(p0); c=int(c1)-int(c0); a=int(a1)-int(a0); b=int(b1)-int(b0); m=int(m1)-int(m0)
ev=p-c-a; pct=100.0*b/ev if ev>0 else 0.0
open(out,'a').write(f"{t},{r},{v},{ev},{b},{pct:.2f},{m},{a}\n")
print(f"{int(t)/1e6:8g}ms {float(v):11,.0f} {ev:11,} {pct:9.1f}% {m:9,} {a:11,}")
PY
  done
done
echo 4000000 > $S/ivh_time_left_threshold_ns
echo "WROTE $OUT"; echo P7CURVE-DONE
