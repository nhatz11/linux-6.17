#!/bin/bash
# Fit the tick-gap estimator against SCHED_FIFO ground truth.
# Fits on d(ivh_tks_steal_ns), which reacts per-tick, NOT on capacity (EMA,
# ~10.5s half-life) -- so no settle time is needed per cell. The capacity
# differential is verified once at the end with the winning settings.
#
# Two probe CPUs deliberately: a COARSE-quantum one (~560us events) and a
# FINE-quantum one (~117us events). A single calibration has to work on both or
# it is just overfitted to one host load shape.
set -u; S=/proc/sys/kernel; T=/root/ivh_tools
COARSE=${COARSE:-3}; FINE=${FINE:-15}; SECS=${SECS:-8}
OUT=$T/tune_steal_$(date +%m%d-%H%M%S).csv
echo "stage,phase_pct,carry_ticks,deadband,cpu,host_ns,k_steal_ns,ratio" > $OUT
getst(){ python3 $T/read_vact_rq.py ivh_tks_steal_ns 2>/dev/null \
         | sed 's/.*per-cpu=\[//;s/\].*//' | cut -d, -f$(($1+1)) | tr -d ' '; }
cell(){  # $1 stage $2 pct $3 carry $4 deadband
  echo "$2" > $S/ivh_tks_phase_pct; echo "$3" > $S/ivh_tks_carry_ticks
  echo "$4" > $S/ivh_tks_deadband_ns
  for c in $COARSE $FINE; do
    s0=$(getst $c)
    P=$(timeout -k 5 $((SECS+25)) $T/vcpu_gone $c $SECS 1 2>/dev/null)
    s1=$(getst $c)
    echo "$P" | awk -v st="$1" -v p="$2" -v ct="$3" -v db="$4" -v c=$c -v s0="$s0" -v s1="$s1" '
      /^cpu=/{split($2,a,"="); el=a[2]}
      /^hist/{split($3,g,"="); split($5,s,"=");
              if(g[2]>=10000000) next; if(g[2]>=50000) host+=s[2]}
      END{ k=s1-s0; printf "%s,%s,%s,%s,%d,%d,%d,%.4f\n", st,p,ct,db,c,host+0,k,(host>0?k/host:0) }' >> $OUT
  done
}
echo "### stage 1: phase_pct (carry=8, deadband=50000)"
for p in 0 25 50 75 100; do cell s1 $p 8 50000; printf "  pct=%-4s done\n" $p; done
BEST_P=$(awk -F, '$1=="s1"{d=($8>1?$8-1:1-$8); s[$2]+=d} END{b=""; for(k in s) if(b==""||s[k]<s[b]) b=k; print b}' $OUT)
echo "### stage 2: carry_ticks (phase_pct=$BEST_P)"
for ct in 1 2 4 8; do cell s2 $BEST_P $ct 50000; printf "  carry=%-3s done\n" $ct; done
BEST_C=$(awk -F, -v p="$BEST_P" '$1=="s2"{d=($8>1?$8-1:1-$8); s[$3]+=d} END{b=""; for(k in s) if(b==""||s[k]<s[b]) b=k; print b}' $OUT)
echo "### stage 3: deadband (phase_pct=$BEST_P carry=$BEST_C)"
for db in 10000 25000 50000; do cell s3 $BEST_P $BEST_C $db; printf "  deadband=%-7s done\n" $db; done
BEST_D=$(awk -F, '$1=="s3"{d=($8>1?$8-1:1-$8); s[$4]+=d} END{b=""; for(k in s) if(b==""||s[k]<s[b]) b=k; print b}' $OUT)
echo "BEST: phase_pct=$BEST_P carry_ticks=$BEST_C deadband_ns=$BEST_D"
echo "$BEST_P $BEST_C $BEST_D" > /tmp/best_tune.txt
echo "DONE -> $OUT"
