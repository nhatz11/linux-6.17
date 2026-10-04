#!/bin/bash
# G-LOCK-34b threshold sweep, redesigned.
#
# Fixes vs sweepthr.sh:
#  1. RANDOMIZED arm order per block. The first sweep ran thresholds in
#     increasing order while the capacity EMA decayed monotonically, perfectly
#     aliasing threshold with drift.
#  2. No capacity readings. ivh_uc_close() computes used/avail with
#     avail = elapsed - idle, and forces SCHED_CAPACITY_SCALE when avail==0, so
#     an idle vCPU reports FULL capacity no matter what the host is doing. Every
#     pre-run "capidle" number in this project's CSVs is that default plus EMA
#     residue from the previous arm, not a contention measurement.
#  3. Contention is instead read DIRECTLY from the gap histogram: the fraction
#     of evictions whose vCPU was still absent >=238us (bucket 19+). That is a
#     measurement of actual vCPU absence under demand -- no EMA, no idle
#     subtraction, no ceiling default.
#  4. FIGURE OF MERIT = recovered us/s = sum over buckets of
#     (count * bucket_lower_edge_us) / duration. This needs NO false/real
#     classification: a false positive contributes its own ~0.2us and is
#     self-discounting. Using the bucket LOWER edge makes it a conservative
#     bound (understates by up to 2x).
#
# NOTE the built-in trade-off: gap is the absence REMAINING at the eviction
# instant, so a higher threshold catches the vCPU later and recovers less per
# event, while firing on fewer false positives. The optimum is a knee, not a
# monotone.
set -u; S=/proc/sys/kernel; D=/root/ivh_tools/evict
SP=/tmp/claude-0/-root-linux-6-17/de075ceb-d4a7-4f0d-9a01-886ca9e713bf/scratchpad
R=/root/ivh_tools/read_ivh_counters.py
DUR=${DUR:-10}; BLOCKS=${BLOCKS:-5}
OUT=$D/sw2_$(date +%H%M%S).csv; META=${OUT%.csv}.meta
{ echo "kernel=$(uname -r)"; echo "kern_sha=$(git -C /root/kernels/linux-6.17-vanilla rev-parse --short HEAD 2>/dev/null)"
  echo "dur=$DUR blocks=$BLOCKS"; for f in $S/ivh_*; do echo "$(basename $f)=$(cat $f 2>/dev/null)"; done; } > $META
echo "blk,arm,thr_us,ops,iters,hit,p50,p99,p999,p9999,max,over1ms,evicts,recov_us_per_s,longfrac" > $OUT

ctr(){ timeout -k 5 60 python3 $R ivh_evict_marked ivh_evict_requeued 2>/dev/null|awk '{printf "%s ",$3}'; }

setarm(){   # $1 = arm name
  case $1 in
    pv)        /root/spin_mode 1 >/dev/null 2>&1; return 0 ;;
    evict_off) $D/arm.sh nt1_only >/dev/null || return 1 ;;
    *)         HOPCAP=1 REQMAX=1 $D/arm.sh evict_nt1 >/dev/null || return 1 ;;
  esac
  echo 511 > $S/ivh_pv_beat_publish_mask
  echo 1 > $S/ivh_pv_evict_gap_hist; echo 0 > $S/ivh_pv_evict_age_hist
  echo 0 > $S/ivh_pv_evict_quiet; echo 0 > $S/ivh_pv_evict_node_stamp
  case $1 in
    e100)  echo 220000  > $S/ivh_pv_beat_threshold ;;
    e238)  echo 524288  > $S/ivh_pv_beat_threshold ;;
    e477)  echo 1048576 > $S/ivh_pv_beat_threshold ;;
    e1000) echo 2200000 > $S/ivh_pv_beat_threshold ;;
    evict_off) echo 2200000 > $S/ivh_pv_beat_threshold ;;
  esac
}

ARMS=(pv evict_off e100 e238 e477 e1000)
for b in $(seq 1 $BLOCKS); do
  for a in $(printf "%s\n" "${ARMS[@]}" | shuf); do
    setarm $a || { echo "ARM FAIL $a"; exit 1; }
    THR=$(cat $S/ivh_pv_beat_threshold)
    python3 $SP/hgrab.py ivh_evict_gap_hist > $SP/w_b.json
    B=$(ctr)
    M=$(timeout -k 5 $((DUR+40)) /root/linux-6.17/qlockbench -t 16 -d $DUR -Q 2>&1|tail -1)
    A=$(ctr)
    python3 $SP/hgrab.py ivh_evict_gap_hist > $SP/w_a.json
    python3 - "$b" "$a" "$THR" "$B" "$A" "$M" "$DUR" "$OUT" "$SP" <<'PY'
import json,sys
blk,arm,thr,B,A,M,DUR,OUT,SP=sys.argv[1],sys.argv[2],int(sys.argv[3]),sys.argv[4].split(),sys.argv[5].split(),sys.argv[6],float(sys.argv[7]),sys.argv[8],sys.argv[9]
x=json.load(open(SP+"/w_b.json")).get("ivh_evict_gap_hist",{})
y=json.load(open(SP+"/w_a.json")).get("ivh_evict_gap_hist",{})
d={int(k):y.get(k,0)-x.get(k,0) for k in set(x)|set(y)}
d={k:v for k,v in d.items() if v>0}
n=sum(d.values())
recov=sum(v*(2.0**k/2200.0) for k,v in d.items())/DUR          # us recovered per second
longf=(sum(v for k,v in d.items() if k>=19)/n) if n else 0.0
ev=int(A[0])-int(B[0])
m=M.split(',')
open(OUT,'a').write(f"{blk},{arm},{thr/2200.0:.0f},{','.join(m)},{ev},{recov:.1f},{longf:.4f}\n")
print(f"  blk{blk} {arm:<10s} thr={thr/2200.0:>5.0f}us ops={m[0]:>9s} p99.9={m[5]:>8s} "
      f">1ms={m[8]:>6s} ev={ev:<6d} recov={recov:>8.0f}us/s long={100*longf:5.1f}%")
PY
  done
done
echo 220000 > $S/ivh_pv_beat_threshold; echo 4095 > $S/ivh_pv_beat_publish_mask
echo 0 > $S/ivh_pv_evict_gap_hist; /root/spin_mode 1 >/dev/null 2>&1
echo "DONE -> $OUT"
