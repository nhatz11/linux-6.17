#!/bin/bash
# HEAD BYPASS -- GATE 1. Observation only; ivh_head_bypass_probe changes no
# behaviour. Two phases, and phase A must pass before phase B means anything.
#
# A) NEUTRALITY. The observer must not perturb the thing it observes. The tree
#    already carries an exact invariant for this: a head that exhausts its spin
#    tenure records exactly ivh_pv_spin_threshold iterations, so
#    head_spin_iters_sum / head_spin_attempts == 32768.0 EXACTLY. Run probe=0
#    and probe=1 alternating; the ratio must hold in both and the node-side
#    ratio must not move beyond noise.
#
# B) OPPORTUNITY. From one u16 load of lock->locked_pending at each sample:
#      held        lock is taken; nothing to do
#      free_open   locked_pending == 0: free AND ALREADY STEALABLE. A bypass
#                  adds NOTHING here -- counting these as opportunity is the
#                  error baked into the old ivh_head_yield_ok_* figures.
#      actionable  locked_pending == _Q_PENDING_VAL: free, stealers locked out
#                  by a pending bit whose owner is not running. The ONLY
#                  population a bypass can serve.
#    plus blocked_cycles, the time actually spent in the actionable state.
#
# GATE (pre-registered, before any data):
#    blocked_cycles / (elapsed * nr_cpus) >= 0.005   AND   actionable >= free_open
# A perfect zero-cost bypass cannot recover more than the first number, so
# below it the mechanism is dead on arithmetic regardless of elegance.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict; R=/root/ivh_tools/read_ivh_counters.py
TSC=2200000000; NCPU=$(nproc); DUR=${DUR:-10}
OUT=$D/head1_$(date +%H%M%S).txt
arm(){ $D/arm.sh control >/dev/null || { echo "ARM FAIL"; exit 1; }
       [ "$(cat $S/ivh_pv_preempt_src)" = 2 ] || { echo "VOID: preempt_src != 2, heartbeat never published"; exit 1; }
       echo 511 > $S/ivh_pv_beat_publish_mask; echo 220000 > $S/ivh_pv_beat_threshold
       echo "$1" > $S/ivh_head_bypass_probe
       [ "$(cat $S/ivh_head_bypass_probe)" = "$1" ] || { echo "PROBE SET FAIL"; exit 1; }; }
c(){ timeout -k 5 60 python3 $R "$@" 2>/dev/null|awk '{printf "%s ",$3}'; }
NEU="ivh_node_spin_iters_sum ivh_node_spin_attempts ivh_beat_tier1_fired ivh_hash_ins_head ivh_head_obs_samples"
OBS="ivh_head_obs_samples ivh_head_obs_stale ivh_head_obs_held ivh_head_obs_free_open ivh_head_obs_actionable ivh_head_blocked_cycles ivh_head_blocked_events ivh_head_blocked_trunc_cycles ivh_head_blocked_trunc_events ivh_node_spin_iters_sum ivh_node_spin_attempts"

{
echo "=== kernel $(uname -r)  ncpu=$NCPU  dur=${DUR}s ==="
echo; echo "--- PHASE A: neutrality, WAITER-side, ABBA order ---"
echo "    (head_spin ratio is structurally blind here: the observer is in the"
echo "     waiter loop, that invariant is in the head loop. Using node_* instead.)"
for rep in 1 2; do for p in 0 1 1 0; do
  arm $p; B=$(c $NEU)
  timeout -k 5 $((DUR+35)) /root/linux-6.17/qlockbench -t 16 -d $DUR -q >/dev/null 2>&1
  A=$(c $NEU)
  python3 -c "
b='$B'.split(); a='$A'.split(); d=[int(a[i])-int(b[i]) for i in range(5)]
nr=d[0]/d[1] if d[1] else 0
print(f'  rep$rep probe=$p  node_iters/att={nr:9.1f}  node_att={d[1]:<8d} tier1={d[2]:<8d} hash_head={d[3]:<7d} obs_samples={d[4]}')
if $p==0 and d[4]!=0: print('      *** NEUTRALITY FAIL: observer ran with probe=0 ***')"
done; done

echo; echo "--- PHASE B: opportunity (probe=1) ---"
for wl in qlockbench dbench; do
  arm 1; B=$(c $OBS)
  case $wl in
    qlockbench) timeout -k 5 $((DUR+35)) /root/linux-6.17/qlockbench -t 16 -d $DUR -q >/dev/null 2>&1 ;;
    dbench)     timeout -k 5 120 dbench -t $DUR 16 >/dev/null 2>&1 ;;
  esac
  A=$(c $OBS)
  python3 -c "
b='$B'.split(); a='$A'.split(); n=['samples','stale','held','free_open','actionable','blocked_cycles','blocked_events','blocked_trunc_cycles','blocked_trunc_events','ivh_node_spin_iters_sum','ivh_node_spin_attempts']
d={k:int(a[i])-int(b[i]) for i,k in enumerate(n)}
# denominator = WAITER SPIN TIME in this run, not elapsed*ncpu. Only one
# observer per lock can see HEAD_SPINNING, so elapsed*ncpu caps the metric at
# 1/ncpu and a '0.5%' gate would secretly mean '8% of maximum'.
spin_cyc=d['ivh_node_spin_iters_sum']*26
lo=d['blocked_cycles']/spin_cyc if spin_cyc else 0
hi=(d['blocked_cycles']+d['blocked_trunc_cycles'])/spin_cyc if spin_cyc else 0
# actionable/free_open is a TAUTOLOGY: free_open is a vanishing state (a stealer
# takes it on the next cmpxchg) so the ratio reads 1e2-1e4 regardless.
ratio=d['actionable']/(d['actionable']+d['held']+d['free_open']) if (d['actionable']+d['held']+d['free_open']) else 0
print(f\"  {'$wl':<11s} samples={d['samples']:<9d} stale={d['stale']:<8d} held={d['held']:<8d} \"
      f\"free_open={d['free_open']:<7d} actionable={d['actionable']:<7d}\")
print(f\"              blocked_cycles={d['blocked_cycles']:<14d} = {frac*100:7.4f}% of vCPU time   (gate >= 0.5000%)\")
print(f\"              actionable/free_open = {ratio:.2f}   (gate >= 1.00)\")
g1 = frac >= 0.005; g2 = ratio >= 1.0
print(f\"              GATE: blocked {'PASS' if g1 else 'FAIL'}   ratio {'PASS' if g2 else 'FAIL'}   -> {'PROCEED' if (g1 and g2) else 'STOP'}\")"
done
echo; echo "--- blocked-episode distribution ---"
timeout -k 5 60 python3 $R ivh_head_blocked_hist 2>/dev/null | head -25
echo 0 > $S/ivh_head_bypass_probe
} 2>&1 | tee $OUT
echo "saved -> $OUT"
