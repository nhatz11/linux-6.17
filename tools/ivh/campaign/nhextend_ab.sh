#!/bin/bash
# nhextend_ab.sh -- the USER-benchmark arm of evaluation point 2.
#
# The campaign harness (run_campaign.sh) runs ONE command in both arms and
# switches only kernel sysctls. NHextend cannot work that way: its "adaptive
# spinning" is a USERSPACE lock, so the arms are different binaries.
#
#   pv  : NHextend3      + migration OFF + spin_mode 1  (stock pvqspinlock)
#   ivh : NHextend-full  + migration ON  + spin_mode 1  (userspace AFL + migration)
#
# spin_mode stays 1 in BOTH arms deliberately -- goto_mode.sh's header records
# that this project holds the kernel adaptive-spinning axis fixed for NHextend
# so the userspace lock and the kernel lock are never accidentally combined.
#
# ABBA/BAAB per block, same discipline as run_campaign.sh.
set -u
S=/proc/sys/kernel
BLOCKS=${BLOCKS:-6}
DUR=${DUR:-20}
SPIN=${SPIN:-600000}
OUT=${OUT:-/root/ivh_tools/campaign/nhextend_ab_$(date +%m%d_%H%M%S)}
mkdir -p "$OUT"
CSV=$OUT/results.csv
echo "block,pos,mode,iters,waitsum,migrations" > "$CSV"
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$OUT/log"; }

run_one() {  # $1 block $2 pos $3 mode
    local blk=$1 pos=$2 mode=$3 bin out it wt m0 m1
    case "$mode" in
      pv)  echo 0 > $S/ivh_universal_eligible; bin=/root/linux-6.17/NHextend3 ;;
      ivh) echo 1 > $S/ivh_universal_eligible; bin=/root/linux-6.17/NHextend-full ;;
    esac
    /root/spin_mode 1 >/dev/null 2>&1
    local el am; el=$(cat $S/ivh_universal_eligible); am=$(cat $S/ivh_adaptive_mode)
    case "$mode" in
      pv)  [ "$el" = 0 ] && [ "$am" = 0 ] || { log "FATAL pv arm did not take"; exit 1; } ;;
      ivh) [ "$el" = 1 ] && [ "$am" = 0 ] || { log "FATAL ivh arm did not take"; exit 1; } ;;
    esac
    m0=$(python3 /root/ivh_tools/migcount.py 2>/dev/null || echo 0)
    out=$(NHEXTEND_DURATION=$DUR NHEXTEND_LOOP_SPIN=$SPIN timeout 120 "$bin" -n 16 2>&1)
    m1=$(python3 /root/ivh_tools/migcount.py 2>/dev/null || echo 0)
    it=$(sed -n 's/^Ran for \([0-9]*\) times/\1/p' <<<"$out")
    wt=$(sed -n 's/^Total wait time: \([0-9.]*\).*/\1/p' <<<"$out")
    echo "$blk,$pos,$mode,${it:-FAIL},${wt:-NA},$((m1-m0))" >> "$CSV"
    log "  blk$blk pos$pos $mode iters=${it:-FAIL} wait=${wt:-NA} migr=$((m1-m0))"
}

log "=== NHextend A/B: $BLOCKS blocks, ${DUR}s runs, loop_spin=$SPIN ==="
for b in $(seq 1 "$BLOCKS"); do
    if [ $((b % 2)) -eq 1 ]; then order="pv ivh ivh pv"; else order="ivh pv pv ivh"; fi
    QUIET=1 MIN_S=45 MAX_S=180 /root/ivh_tools/wait_capacity_settled.sh >/dev/null 2>&1
    log "block $b order=[$order]"
    pos=0
    for m in $order; do pos=$((pos+1)); run_one "$b" "$pos" "$m"; done
done

python3 - "$CSV" <<'PY'
import csv,sys,statistics as st
rows=list(csv.DictReader(open(sys.argv[1])))
blocks={}
for r in rows:
    if r['iters']=='FAIL': continue
    blocks.setdefault(int(r['block']),{}).setdefault(r['mode'],[]).append(int(r['iters']))
eff=[]
for b,d in sorted(blocks.items()):
    if 'pv' not in d or 'ivh' not in d: continue
    p,i=st.mean(d['pv']),st.mean(d['ivh'])
    e=100*(i-p)/p; eff.append(e)
    print(f"block {b}: pv={p:10.0f} ivh={i:10.0f}  {e:+7.2f}%")
if eff:
    print(f"\nmedian {st.median(eff):+.2f}%   mean {st.mean(eff):+.2f}%   "
          f"blocks positive {sum(1 for e in eff if e>0)}/{len(eff)}")
    if len(eff)>1:
        t=st.mean(eff)/(st.stdev(eff)/len(eff)**.5)
        print(f"t = {t:+.2f}")
PY
echo 1 > $S/ivh_universal_eligible
