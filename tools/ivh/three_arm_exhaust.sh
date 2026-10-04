#!/bin/bash
# PV  vs  IVH(migration+tier1+tier2)  vs  IVH(same, exhaustion effectively removed)
#
# Arm 3 raises ivh_pv_spin_threshold to its max (1<<24, 512x default) so a
# waiter effectively never halts from running out of spin budget -- it halts
# only when tier 1 or tier 2 says someone ahead of it is down. That is the
# professor's proposal, testable without a rebuild.
set -u
S=/proc/sys/kernel
ROUNDS=${ROUNDS:-5}
HB="hackbench -T -g1 -f8 -l400000"
DEF_THRESH=32768
MAX_THRESH=16777216

snap() { python3 /root/ivh_tools/phase0b_dump.py "$1" >/dev/null; }
head_ct() { python3 /root/ivh_tools/read_ivh_counters.py ivh_halt_from_head | awk -F= '{print $2}'; }

set_arm() {
  case "$1" in
    pv)    echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null
           echo $DEF_THRESH > $S/ivh_pv_spin_threshold ;;
    as)    echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null
           echo $DEF_THRESH > $S/ivh_pv_spin_threshold ;;
    noexh) echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null
           echo $MAX_THRESH > $S/ivh_pv_spin_threshold ;;
  esac
}

echo "round,arm,time_s,tier1,tier2,exhaust,head,spin_thresh,adaptive_mode,tier1_en,migration"
for r in $(seq 1 "$ROUNDS"); do
  for arm in pv as noexh; do
    set_arm "$arm"
    h0=$(head_ct); snap /tmp/ta0.json
    t=$($HB 2>&1 | grep -oP 'Time: \K[0-9.]+')
    h1=$(head_ct); snap /tmp/ta1.json
    python3 - "$r" "$arm" "$t" "$h0" "$h1" <<'PY'
import json,sys
r,arm,t,h0,h1=sys.argv[1],sys.argv[2],sys.argv[3],int(sys.argv[4]),int(sys.argv[5])
b,a=json.load(open("/tmp/ta0.json")),json.load(open("/tmp/ta1.json"))
C=["NONE","TIER1","TIER1_AGREED","TIER1_DISAGREED","TIER2","EXHAUST"];NB=32
h=[x-y for x,y in zip(a["ivh_node_halt_hist"],b["ivh_node_halt_hist"])]
c={n:sum(h[i*NB:(i+1)*NB]) for i,n in enumerate(C)}
t1=c["TIER1"]+c["TIER1_AGREED"]+c["TIER1_DISAGREED"]
rd=lambda p:open(p).read().strip()
print(f"{r},{arm},{t},{t1},{c['TIER2']},{c['EXHAUST']},{h1-h0},"
      f"{rd('/proc/sys/kernel/ivh_pv_spin_threshold')},"
      f"{rd('/proc/sys/kernel/ivh_adaptive_mode')},"
      f"{rd('/proc/sys/kernel/ivh_pv_tier1_enable')},"
      f"{rd('/proc/sys/kernel/ivh_universal_eligible')}", flush=True)
PY
  done
done
set_arm as   # leave the box in the normal IVH+AS state
