#!/bin/bash
# OVERNIGHT: the paper decision. Adaptive-spinning arms vs stock PV across the
# documented migration-winning workloads, MIGRATION OFF throughout.
#   pv        stock PV
#   t12       tier1 + tier2 early bail
#   t12byp    t12 + head bypass                 <- paper candidate B
#   combo     fixed lock skipping (lookahead + nosteal, hop_cap=2), tier1/2 OFF
#   comboByp  combo + head bypass               <- paper candidate A
#   all       t12 + combo + head bypass
# Arms are SHUFFLED per block so drift and position cancel. Resumable.
set -u
S=/proc/sys/kernel; CAMP=/root/ivh_tools/campaign
OUT=${OUT:-$CAMP/arms_$(date +%m%d-%H%M%S)}; mkdir -p "$OUT"
LIST=${LIST:-$CAMP/benchmarks36.tsv}
BLOCKS=${BLOCKS:-8}; ARMS=${ARMS:-"pv t12 t12byp combo comboByp all"}
CSV=$OUT/results.csv; LOG=$OUT/log
[ -f "$CSV" ] || echo "workload,block,pos,arm,value,ts" > "$CSV"
log(){ echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }
zero(){ for k in ivh_head_bypass_enable ivh_head_bypass_probe ivh_pv_trylock_relaxed \
        ivh_pv_rot_probe ivh_pv_rot_enable ivh_pv_evict_node_stamp ivh_pv_evict_debug \
        ivh_pv_evict_enable ivh_pv_requeue_nosteal ivh_pv_requeue_none \
        ivh_pv_evict_lookahead ivh_pv_camp_probe; do echo 0 > $S/$k 2>/dev/null; done; }
set_arm(){
  echo 0 > $S/ivh_universal_eligible 2>/dev/null   # migration OFF in every arm
  if [ "$1" = pv ]; then /root/spin_mode 1 >/dev/null 2>&1; zero; return 0; fi
  /root/ivh_tools/evict/arm.sh nt1_only >/dev/null || return 1
  zero
  echo 2 > $S/ivh_pv_preempt_src; echo 220000 > $S/ivh_pv_beat_threshold
  echo 4095 > $S/ivh_pv_beat_publish_mask; echo 32768 > $S/ivh_pv_spin_threshold
  echo 2 > $S/ivh_pv_evict_hop_cap; echo 4 > $S/ivh_pv_requeue_max
  case "$1" in
    t12|t12byp|all|t12combo) echo 1 > $S/ivh_pv_tier1_enable; echo 1 > $S/ivh_pv_tier2_enable ;;
  esac
  case "$1" in
    combo|comboByp|all|t12combo) echo 1 > $S/ivh_pv_evict_enable; echo 1 > $S/ivh_pv_evict_lookahead
                        echo 1 > $S/ivh_pv_requeue_nosteal ;;
  esac
  case "$1" in
    t12byp|comboByp|all) echo 1 > $S/ivh_head_bypass_probe; echo 1 > $S/ivh_pv_trylock_relaxed
                         echo 1 > $S/ivh_head_bypass_enable ;;
  esac
  # verify
  local t1 t2 ev by; t1=$(cat $S/ivh_pv_tier1_enable); t2=$(cat $S/ivh_pv_tier2_enable)
  ev=$(cat $S/ivh_pv_evict_enable); by=$(cat $S/ivh_head_bypass_enable)
  case "$1" in
    t12)      [ "$t1$t2$ev$by" = 1100 ] || { log "ARM FAIL t12 $t1$t2$ev$by"; return 1; } ;;
    t12byp)   [ "$t1$t2$ev$by" = 1101 ] || { log "ARM FAIL t12byp $t1$t2$ev$by"; return 1; } ;;
    combo)    [ "$t1$t2$ev$by" = 0010 ] || { log "ARM FAIL combo $t1$t2$ev$by"; return 1; } ;;
    comboByp) [ "$t1$t2$ev$by" = 0011 ] || { log "ARM FAIL comboByp $t1$t2$ev$by"; return 1; } ;;
    all)      [ "$t1$t2$ev$by" = 1111 ] || { log "ARM FAIL all $t1$t2$ev$by"; return 1; } ;;
    t12combo) [ "$t1$t2$ev$by" = 1110 ] || { log "ARM FAIL t12combo $t1$t2$ev$by"; return 1; } ;;
  esac
  return 0
}
log "=== ARMS campaign start $(date -Is) kernel=$(uname -r) ncpu=$(nproc) ==="
log "arms: $ARMS   blocks: $BLOCKS   list: $LIST"
mkdir -p /dev/shm/fsmark /dev/shm/fiobench 2>/dev/null
while IFS=$'\t' read -r name dir to cmd ext hl; do
  [ -z "${name:-}" ] && continue; case "$name" in \#*) continue;; esac
  for b in $(seq 1 $BLOCKS); do
    grep -q "^$name,$b," "$CSV" 2>/dev/null && continue
    pos=0
    for a in $(echo $ARMS | tr ' ' '\n' | shuf); do
      pos=$((pos+1))
      set_arm "$a" || { log "$name blk$b: arm $a failed, skipping run"; continue; }
      v=$(cd "$dir" && timeout -k 10 "$to" bash -c "$cmd" 2>/dev/null | eval "$ext" 2>/dev/null | head -1)
      [ -z "$v" ] && v=FAIL
      echo "$name,$b,$pos,$a,$v,$(date +%s)" >> "$CSV"
      log "  $name blk$b pos$pos $a = $v"
    done
  done
  log "$name: done"
done < <(grep -v '^#' "$LIST")
/root/spin_mode 1 >/dev/null 2>&1; zero; echo 1 > $S/ivh_pv_evict_hop_cap
log "=== ARMS CAMPAIGN COMPLETE $(date -Is) ==="
