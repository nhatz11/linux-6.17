#!/bin/bash
# 2D fit of (phase_pct, deadband). tune_steal2 showed deadband -- not phase_pct
# -- is what makes sub-tick (fine-quantum) steal visible at all, and that the
# response is very sharp between 1000 and 2000 ns. Low deadband risks booking
# ordinary tick jitter as steal, so every cell ALSO measures an in-guest-idle
# vCPU (no prober on it): that is the false-positive channel that would destroy
# the contended/idle capacity differential the user requires be preserved.
# Both probers run CONCURRENTLY so all three CPUs share one measurement window.
set -u; S=/proc/sys/kernel; T=/root/ivh_tools
COARSE=${COARSE:-3}; FINE=${FINE:-15}; IDLE=${IDLE:-8}; SECS=${SECS:-8}
OUT=$T/tune_steal3_$(date +%m%d-%H%M%S).csv
echo "phase_pct,deadband,c_host,c_kern,c_ratio,f_host,f_kern,f_ratio,idle_kern_ns" > $OUT
allst(){ python3 $T/read_vact_rq.py ivh_tks_steal_ns 2>/dev/null \
         | sed 's/.*per-cpu=\[//;s/\].*//' | tr -d ' '; }
pick(){ echo "$1" | cut -d, -f$(($2+1)); }
hostns(){ awk '/^hist/{split($3,g,"="); split($5,s,"=");
                if(g[2]>=10000000) next; if(g[2]>=50000) h+=s[2]} END{print h+0}' "$1"; }
cell(){ # $1 phase $2 deadband
  echo "$1" > $S/ivh_tks_phase_pct; echo "$2" > $S/ivh_tks_deadband_ns
  A=$(allst)
  timeout -k 5 $((SECS+25)) $T/vcpu_gone $COARSE $SECS 1 > /tmp/pc.txt 2>/dev/null &
  p1=$!
  timeout -k 5 $((SECS+25)) $T/vcpu_gone $FINE   $SECS 1 > /tmp/pf.txt 2>/dev/null &
  p2=$!
  wait $p1 $p2
  B=$(allst)
  ch=$(hostns /tmp/pc.txt); fh=$(hostns /tmp/pf.txt)
  ck=$(( $(pick "$B" $COARSE) - $(pick "$A" $COARSE) ))
  fk=$(( $(pick "$B" $FINE)   - $(pick "$A" $FINE) ))
  ik=$(( $(pick "$B" $IDLE)   - $(pick "$A" $IDLE) ))
  awk -v p=$1 -v d=$2 -v ch=$ch -v ck=$ck -v fh=$fh -v fk=$fk -v ik=$ik \
    'BEGIN{printf "%d,%d,%d,%d,%.4f,%d,%d,%.4f,%d\n",p,d,ch,ck,(ch>0?ck/ch:0),fh,fk,(fh>0?fk/fh:0),ik}' >> $OUT
}
for db in 1000 1250 1500 2000; do
  for p in 50 75 100 150; do cell $p $db; printf "  db=%-5s pct=%-4s done\n" $db $p; done
done
echo; echo "=== RESULTS (idle_kern_ns over ${SECS}s: false-positive channel) ==="
column -s, -t $OUT
echo "DONE -> $OUT"
