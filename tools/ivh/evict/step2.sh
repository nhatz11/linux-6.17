#!/bin/bash
# STEP 2: the trylock relaxation ALONE. No bypass exists yet.
#
# trylock_clear_pending() as shipped can only acquire from {locked=0,pending=1}.
# Relaxed, it also accepts locked_pending==0 -- matching upstream's own
# _Q_PENDING_BITS != 8 variant, which already behaves this way. Required before
# any head bypass, because otherwise a third party clearing pending locks the
# head out of its entire remaining tenure.
#
# It is ALSO a mild fairness change on its own: pending stops being a reliable
# "a head is spinning" flag for pv_hybrid_queued_unfair_trylock(), so stealers
# get in a little more often while a live head runs. That is why it is measured
# by itself -- bundling it with the mechanism would recreate the two-variable
# confound behind the bogus -7.75% eviction result.
#
# Both arms are the SAME kernel and the SAME arm.sh config; only
# ivh_pv_trylock_relaxed differs. Order randomized per block (the fixed-order
# sweep earlier today produced a 7x "effect" that was pure drift).
#
# GATE: ops CI must include 0, and >1ms must not regress significantly.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict
BLOCKS=${BLOCKS:-12}; DUR=${DUR:-10}
OUT=$D/step2_$(date +%H%M%S).csv; META=${OUT%.csv}.meta
{ echo "kernel=$(uname -r)"; echo "blocks=$BLOCKS dur=$DUR"
  for f in $S/ivh_*; do echo "$(basename $f)=$(cat $f 2>/dev/null)"; done; } > $META
echo "blk,wl,relaxed,ops,iters,hit,p50,p99,p999,p9999,max,over1ms" > $OUT

setarm(){ $D/arm.sh control >/dev/null || { echo "ARM FAIL"; exit 1; }
  echo 511 > $S/ivh_pv_beat_publish_mask; echo 220000 > $S/ivh_pv_beat_threshold
  echo 0 > $S/ivh_head_bypass_probe        # keep the observer OUT of the A/B
  echo "$1" > $S/ivh_pv_trylock_relaxed
  [ "$(cat $S/ivh_pv_trylock_relaxed)" = "$1" ] || { echo "SET FAIL"; exit 1; }; }

for b in $(seq 1 $BLOCKS); do
  for r in $(printf "0\n1\n" | shuf); do
    setarm $r
    Q=$(timeout -k 5 $((DUR+35)) /root/linux-6.17/qlockbench -t 16 -d $DUR -Q 2>&1|tail -1)
    echo "$b,qlockbench,$r,$Q" >> $OUT
    setarm $r
    T=$(timeout -k 5 120 dbench -t $DUR 16 2>&1|awk '/^Throughput/{print $2}')
    echo "$b,dbench,$r,${T:-0},0,0,0,0,0,0,0,0" >> $OUT
    printf "  blk%-3s relaxed=%s  qlock_ops=%-9s >1ms=%-7s  dbench=%s\n" \
      "$b" "$r" "$(echo $Q|cut -d, -f1)" "$(echo $Q|cut -d, -f9)" "${T:-ERR}"
  done
done
echo 0 > $S/ivh_pv_trylock_relaxed
echo "DONE -> $OUT"
