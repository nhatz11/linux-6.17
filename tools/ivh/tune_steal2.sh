#!/bin/bash
# Extension of tune_steal.sh past the previously-tested edges:
#   phase_pct was monotonic and still climbing at its documented max of 100
#   deadband was best at the LOWEST tested value (10000)
# Both are unclamped in the kernel, so the old ranges were convention, not limit.
set -u; S=/proc/sys/kernel; T=/root/ivh_tools
COARSE=${COARSE:-3}; FINE=${FINE:-15}; SECS=${SECS:-8}
OUT=$T/tune_steal2_$(date +%m%d-%H%M%S).csv
echo "stage,phase_pct,carry_ticks,deadband,cpu,host_ns,k_steal_ns,ratio" > $OUT
getst(){ python3 $T/read_vact_rq.py ivh_tks_steal_ns 2>/dev/null \
         | sed 's/.*per-cpu=\[//;s/\].*//' | cut -d, -f$(($1+1)) | tr -d ' '; }
cell(){
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
echo "### stage A: phase_pct past 100 (carry=8, deadband=10000)"
for p in 100 150 200 250 300; do cell sA $p 8 10000; printf "  pct=%-4s done\n" $p; done
BP=$(awk -F, '$1=="sA"&&$5=='"$COARSE"'{d=($8>1?$8-1:1-$8); if(b==""||d<b){b=d;k=$2}} END{print k}' $OUT)
echo "### stage B: deadband below 10000 (phase_pct=$BP carry=8)"
for db in 500 1000 2000 5000 10000; do cell sB $BP 8 $db; printf "  deadband=%-6s done\n" $db; done
echo; echo "=== RESULTS ==="; column -s, -t $OUT
echo "DONE -> $OUT"
