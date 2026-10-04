#!/bin/bash
# PHASE 1a: what WORK does each eviction actually cause?
# base vs skip, measuring total spin iterations and halt work, not just throughput.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict; Q=/root/linux-6.17/qlockbench
BLOCKS=${BLOCKS:-8}; DUR=${DUR:-10}; T=${T:-$(nproc)}; HOP=${HOP:-1}; REQ=${REQ:-4}
OUT=$D/${TAG:-accept}_$(date +%m%d-%H%M%S).csv
H="spin_iters,spin_att,succ_iters,succ_att,marked,requeued,steal_ok,ok_skipped,hopexh,haltrace,hspin,harm,hhalt_from,hforeign,hdup_head,hdup_kick,xt_calls,xt_nonempty,rqn_won,rqn_fell,camp_ent,camp_trips,camp_win,camp_empty,camp_pend,la_refused,promo_unk,nhalt_ev,nhalt_cyc,hhalt_ev,hhalt_cyc"
echo "blk,arm,hop,req,iters,p50,p99,p999,p9999,max,over1ms,$H" > $OUT
snap(){ timeout -k 5 150 python3 $D/snap.py 2>/dev/null; }
setarm(){
  $D/arm.sh nt1_only >/dev/null || exit 1
  for k in ivh_head_bypass_enable ivh_head_bypass_probe ivh_pv_trylock_relaxed \
           ivh_pv_rot_probe ivh_pv_rot_enable ivh_pv_evict_node_stamp ivh_pv_evict_debug \
           ivh_pv_requeue_nosteal ivh_pv_requeue_none ivh_pv_evict_lookahead \
           ivh_pv_evict_promo_hist ivh_pv_camp_probe; do echo 0 > $S/$k 2>/dev/null; done
  echo 2 > $S/ivh_pv_preempt_src; echo 220000 > $S/ivh_pv_beat_threshold
  echo 4095 > $S/ivh_pv_beat_publish_mask; echo 32768 > $S/ivh_pv_spin_threshold
  echo $HOP > $S/ivh_pv_evict_hop_cap; echo $REQ > $S/ivh_pv_requeue_max
  case "$1" in
    base)      echo 0 > $S/ivh_pv_evict_enable ;;
    skip)      echo 1 > $S/ivh_pv_evict_enable ;;
    nosteal)   echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_requeue_nosteal ;;
    noreq)     echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_requeue_none ;;
    lookahead) echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_lookahead ;;
    combo)     echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_lookahead
               echo 1 > $S/ivh_pv_requeue_nosteal ;;
  esac
  [ "$(cat $S/ivh_pv_tier1_enable)" = 0 ] && [ "$(cat $S/ivh_pv_tier2_enable)" = 0 ] || { echo "ARM FAIL"; exit 1; }
}
for b in $(seq 1 $BLOCKS); do
  for a in $(echo ${ARMS:-"base skip"}|tr " " "\n"|shuf); do
    setarm "$a"; B=$(snap)
    QQ=$(timeout -k 5 $((DUR+50)) $Q -t $T -d $DUR -Q 2>&1|tail -1)
    A=$(snap)
    python3 - "$b" "$a" "$HOP" "$REQ" "$QQ" "$B" "$A" >> $OUT <<'PY'
import sys
b,a,hop,req,q,B,A=sys.argv[1:8]
d=[int(x)-int(y) for x,y in zip(A.split(','),B.split(','))]
f=q.split(',')
# qlockbench -Q: ops,iters,hit,p50,p99,p999,p9999,max,over1ms
print(",".join([b,a,hop,req]+f[1:2]+f[3:9]+[str(x) for x in d]))
PY
  done
  printf "  blk%-3s done\n" "$b"
done
/root/spin_mode 1 >/dev/null 2>&1; echo 0 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_hop_cap
echo "DONE -> $OUT"
