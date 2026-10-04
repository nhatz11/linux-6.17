#!/bin/bash
# PHASE B: is lock skipping merely UNDER-TRIGGERED? Sweep the detector
# threshold with the BEST skipping config (lookahead + nosteal + head bypass).
# Lower threshold -> fires more often, but more false positives (27% of victims
# already return in <1us at 220000).
set -u
S=/proc/sys/kernel; CAMP=/root/ivh_tools/campaign
OUT=${OUT:-$CAMP/thresh_$(date +%m%d-%H%M%S)}; mkdir -p "$OUT"
LIST=${LIST:-$CAMP/bench5.tsv}; BLOCKS=${BLOCKS:-6}
THRS=${THRS:-"220000 110000 55000 22000"}
CSV=$OUT/results.csv; LOG=$OUT/log
[ -f "$CSV" ] || echo "workload,block,thr,arm,value,marked,la_ref,ts" > "$CSV"
log(){ echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
zero(){ for k in ivh_head_bypass_enable ivh_head_bypass_probe ivh_pv_trylock_relaxed \
        ivh_pv_rot_probe ivh_pv_rot_enable ivh_pv_evict_node_stamp ivh_pv_evict_debug \
        ivh_pv_evict_enable ivh_pv_requeue_nosteal ivh_pv_requeue_none \
        ivh_pv_evict_lookahead ivh_pv_camp_probe; do echo 0 > $S/$k 2>/dev/null; done; }
ctr(){ timeout -k 5 90 python3 /root/ivh_tools/read_ivh_counters.py \
        ivh_evict_marked ivh_evict_lookahead_refused 2>/dev/null|awk '{printf "%s ",$3}'; }
set_arm(){  # $1 arm  $2 threshold
  echo 0 > $S/ivh_universal_eligible 2>/dev/null
  if [ "$1" = pv ]; then /root/spin_mode 1 >/dev/null 2>&1; zero; return 0; fi
  /root/ivh_tools/evict/arm.sh nt1_only >/dev/null || return 1
  zero
  echo 2 > $S/ivh_pv_preempt_src; echo "$2" > $S/ivh_pv_beat_threshold
  echo 4095 > $S/ivh_pv_beat_publish_mask; echo 32768 > $S/ivh_pv_spin_threshold
  echo 2 > $S/ivh_pv_evict_hop_cap; echo 4 > $S/ivh_pv_requeue_max
  echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_lookahead
  echo 1 > $S/ivh_pv_requeue_nosteal
  echo 1 > $S/ivh_head_bypass_probe; echo 1 > $S/ivh_pv_trylock_relaxed
  echo 1 > $S/ivh_head_bypass_enable
  [ "$(cat $S/ivh_pv_beat_threshold)" = "$2" ] && [ "$(cat $S/ivh_pv_evict_enable)" = 1 ] || { log "ARM FAIL $1 $2"; return 1; }
  return 0
}
log "=== THRESH sweep start $(date -Is) thresholds: $THRS ==="
while IFS=$'\t' read -r name dir to cmd ext hl; do
  [ -z "${name:-}" ] && continue; case "$name" in \#*) continue;; esac
  for b in $(seq 1 $BLOCKS); do
    grep -q "^$name,$b," "$CSV" 2>/dev/null && continue
    for thr in $THRS; do
      for a in $(printf "pv\ncomboByp\n"|shuf); do
        set_arm "$a" "$thr" || continue
        read -r m0 l0 <<< "$(ctr)"
        v=$(cd "$dir" && timeout -k 10 "$to" bash -c "$cmd" 2>/dev/null | eval "$ext" 2>/dev/null | head -1)
        read -r m1 l1 <<< "$(ctr)"
        [ -z "$v" ] && v=FAIL
        echo "$name,$b,$thr,$a,$v,$((m1-m0)),$((l1-l0)),$(date +%s)" >> "$CSV"
        log "  $name blk$b thr=$thr $a = $v (ev=$((m1-m0)))"
      done
    done
  done
  log "$name: done"
done < <(grep -v '^#' "$LIST")
/root/spin_mode 1 >/dev/null 2>&1; zero
echo 220000 > $S/ivh_pv_beat_threshold; echo 1 > $S/ivh_pv_evict_hop_cap
log "=== THRESH COMPLETE $(date -Is) ==="
