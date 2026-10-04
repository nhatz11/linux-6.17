#!/bin/bash
# rwsem wait in the PV+t1 arm (no migration), so ebizzy's wait row has a baseline.
# migcost_light's migrate probes simply never fire here; the rwsem probes do.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
W="${1:?w}"; SEC="${2:-25}"; ARM="${3:-pv_t1}"
case $ARM in
  pv_t1)  bash $T/pvbase.sh >/dev/null 2>&1 ;;
  mig_t1) bash $T/p7v2_arm.sh 2500000 >/dev/null 2>&1
          echo 0 > $S/ivh_pv_tier2_enable; echo 1 > $S/ivh_cs_gate2_reference ;;
esac
echo 1 > $S/ivh_pv_tier1_enable; sleep 1
case $W in
  ebizzy) CMD="timeout 120 /home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304"; D=/root ;;
  fsmark) rm -rf /dev/shm/fsmark; mkdir -p /dev/shm/fsmark
          CMD="timeout 300 fs_mark -d /dev/shm/fsmark -D 16 -n 30000 -s 4096 -t 16 -L 1"; D=/root ;;
  dedup)  sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
          CMD="timeout 600 ./bin/parsecmgmt -a run -p dedup -c gcc -i native -n 16"; D=/root/parsec-benchmark ;;
esac
BT=/root/ivh_logs/rwsem_${W}_${ARM}_$(date +%m%d-%H%M%S).txt
bpftrace $T/migcost_light.bt "$SEC" > "$BT" 2>&1 & BTPID=$!
sleep 2; ( cd "$D" && eval "$CMD" >/dev/null 2>&1 ); wait $BTPID 2>/dev/null
python3 - "$BT" "$W" "$ARM" <<'PY'
import re,sys
t=open(sys.argv[1]).read()
def top(k):
    v=re.findall(r'@'+k+r'\[([^\]]+)\]:\s*(\d+)',t)
    return sorted(((int(x),c) for c,x in v),reverse=True)
def sc(k):
    m=re.search(r'@'+k+r':\s*(\d+)',t); return int(m.group(1)) if m else 0
wl=sys.argv[2]
rw=top('rww_ns'); rr=top('rwr_ns'); nw=top('rww_n'); nr=top('rwr_n')
pick=lambda L:next((x for x,c in L if wl[:8] in c), L[0][0] if L else 0)
W=pick(rw); R=pick(rr)
print(f"  {sys.argv[2]:8s} {sys.argv[3]:7s}  rwsem WRITE {W/1e6:9.1f} ms  READ {R/1e6:9.1f} ms  TOTAL {(W+R)/1e6:9.1f} ms")
print(f"           migrations tracked: {sc('n')}")
PY
