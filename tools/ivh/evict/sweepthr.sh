#!/bin/bash
# G-LOCK-34b threshold sweep. The gap histogram under contention is bimodal
# with an EMPTY band at buckets 17-18 (30us..238us): a false mode below it
# (evicted vCPU back in <30us, i.e. it was never away -- the same population
# that is 100% of evictions on an idle host) and a real mode above it (median
# ~477us). The threshold currently sits BELOW the band, so eviction fires on
# both. This finds where to put it.
set -u; S=/proc/sys/kernel; SP=/tmp/claude-0/-root-linux-6-17/de075ceb-d4a7-4f0d-9a01-886ca9e713bf/scratchpad
D=/root/ivh_tools/evict; R=/root/ivh_tools/read_ivh_counters.py
DUR=${DUR:-10}
ctr(){ timeout -k 5 60 python3 $R ivh_evict_marked ivh_evict_requeued ivh_evict_ok_while_skipped 2>/dev/null|awk '{printf "%s ",$3}'; }
cap(){ timeout -k 5 60 python3 $D/capsnap.py|grep -o 'mean=[0-9]*'|cut -d= -f2; }

printf "%-9s %-7s %-6s %-9s %-8s %-8s %-9s %-9s %-8s %s\n" \
  thr_us cap ev/s real/s false% real% ops p99.9 ">1ms" hit
for thr in 220000 524288 1048576 2200000 4400000; do
  echo $thr > $S/ivh_pv_beat_threshold
  [ "$(cat $S/ivh_pv_beat_threshold)" = "$thr" ] || { echo "SET FAIL $thr"; exit 1; }
  sleep 2; C=$(cap)
  python3 $SP/hgrab.py ivh_evict_gap_hist > $SP/s_b.json
  B=$(ctr)
  M=$(timeout -k 5 $((DUR+40)) /root/linux-6.17/qlockbench -t 16 -d $DUR -Q 2>&1|tail -1)
  A=$(ctr)
  python3 $SP/hgrab.py ivh_evict_gap_hist > $SP/s_a.json
  python3 - "$thr" "$C" "$B" "$A" "$M" "$DUR" <<'PY'
import json,sys
thr,C,B,A,M,DUR=sys.argv[1],sys.argv[2],sys.argv[3].split(),sys.argv[4].split(),sys.argv[5].split(','),float(sys.argv[6])
SP="/tmp/claude-0/-root-linux-6-17/de075ceb-d4a7-4f0d-9a01-886ca9e713bf/scratchpad"
a=json.load(open(SP+"/s_b.json")).get("ivh_evict_gap_hist",{})
b=json.load(open(SP+"/s_a.json")).get("ivh_evict_gap_hist",{})
d={int(k):b.get(k,0)-a.get(k,0) for k in set(a)|set(b)}
d={k:v for k,v in d.items() if v>0}
n=sum(d.values())
# FALSE = came back before the empty band (<= b18); REAL = b19 and above
fa=sum(v for k,v in d.items() if k<=18); re_=sum(v for k,v in d.items() if k>=19)
ev=int(A[0])-int(B[0])
print(f"{int(thr)/2200.0:<9.0f} {C:<7} {ev/DUR:<6.0f} {re_/DUR:<9.1f} "
      f"{100*fa/n if n else 0:<8.1f} {100*re_/n if n else 0:<8.1f} "
      f"{M[0]:<9} {M[5]:<9} {M[8]:<8} {M[2]}")
PY
done
echo 220000 > $S/ivh_pv_beat_threshold
