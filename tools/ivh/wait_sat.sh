#!/bin/bash
# Poll until the contended vCPUs (0-14) have clearly dropped and stayed down.
set -u
cap(){ python3 /root/ivh_tools/read_vact_rq.py ivh_uc_capacity 2>/dev/null \
       | sed 's/.*per-cpu=\[//;s/\].*//' | tr -d ' '; }
stable=0
for i in $(seq 60); do
  C=$(cap)
  M=$(echo "$C" | awk -F, '{s=0; for(i=1;i<=15;i++) s+=$i; printf "%d", s/15}')
  L=$(echo "$C" | awk -F, '{print $16}')
  printf "  t=%3ds  contended(0-14) mean=%-5s  vcpu15=%-5s  delta=%+d\n" \
     $((i*10)) "$M" "$L" $((L-M))
  if [ "$M" -lt 880 ]; then stable=$((stable+1)); else stable=0; fi
  [ "$stable" -ge 3 ] && { echo "SATURATED"; exit 0; }
  sleep 10
done
echo "TIMEOUT-NOT-SATURATED"
