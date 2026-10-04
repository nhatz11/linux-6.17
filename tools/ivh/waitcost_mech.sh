#!/bin/bash
# Measure MECH (= t_onrq - t_commit) per migration on THIS kernel, plus the
# rwsem wait for ebizzy, using migcost_light.bt (NOT migcost.bt, which probes
# bpf_sched_pre_lock_migrate at ~48k/s and changes the result it measures).
# Instrumented: these numbers are mechanism terms only, never the headline.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
W="${1:?workload}"; SEC="${2:-20}"
case $W in
  hackbench) CMD="timeout 300 hackbench -T -g1 -f8 -l150000"; D=/root ;;
  memtier)   systemctl stop memcached >/dev/null 2>&1; pkill -9 -x memcached 2>/dev/null; sleep 1
             memcached -u root -d -m 1024 -t 16 -p 11211 -o hashpower=15 2>/dev/null 9>&-; sleep 2
             CMD="timeout 120 /root/memtier_benchmark/memtier_benchmark -P memcache_binary -s 127.0.0.1 -p 11211 -t 16 -c 50 --key-maximum=100 --ratio=1:0 -d 32 --test-time=10 --hide-histogram"; D=/root ;;
  ebizzy)    CMD="timeout 120 /home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304"; D=/root ;;
  dedup)     sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
             CMD="timeout 600 ./bin/parsecmgmt -a run -p dedup -c gcc -i native -n 16"; D=/root/parsec-benchmark ;;
  fsmark)    rm -rf /dev/shm/fsmark; mkdir -p /dev/shm/fsmark
             CMD="timeout 300 fs_mark -d /dev/shm/fsmark -D 16 -n 30000 -s 4096 -t 16 -L 1"; D=/root ;;
  nhextend)  CMD="timeout 90 env NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=600000 IVH_AFL_DISABLE=1 NHEXTEND_CS_MIN=1 /root/linux-6.17/NHextend-csmin -l -n 16"; D=/root ;;
  *) echo "unknown $W"; exit 1 ;;
esac
bash $T/p7v2_arm.sh 2500000 >/dev/null 2>&1 || exit 1
echo 0 > $S/ivh_pv_tier2_enable; echo 1 > $S/ivh_pv_tier1_enable; echo 1 > $S/ivh_cs_gate2_reference
[ "$(cat $S/ivh_cs_gate2_reference)" = 1 ] || { echo "FATAL csmin"; exit 1; }
BT=/root/ivh_logs/mech_${W}_$(date +%m%d-%H%M%S).txt
bpftrace $T/migcost_light.bt "$SEC" > "$BT" 2>&1 &
BTPID=$!
sleep 2
( cd "$D" && eval "$CMD" >/dev/null 2>&1 )
wait $BTPID 2>/dev/null
python3 - "$BT" "$W" <<'PY'
import re, sys
t = open(sys.argv[1]).read(); w = sys.argv[2]
def tot(key):
    # @c_mech[comm]: sum ; take the largest comm bucket (the workload's threads)
    vals = re.findall(r'@'+key+r'\[([^\]]+)\]:\s*(\d+)', t)
    return sorted(((int(v), c) for c, v in vals), reverse=True)
def scalar(key):
    m = re.search(r'@'+key+r':\s*(\d+)', t); return int(m.group(1)) if m else 0
n = scalar('n')
mech = tot('c_mech'); delay = tot('c_delay')
print(f"  === {w}: migcost_light, {n} completed migrations tracked ===")
if n and mech:
    tm, tc = mech[0]
    td = delay[0][0] if delay else 0
    print(f"    top comm           {tc}")
    print(f"    MECH  total {tm/1e6:9.1f} ms   per migration {tm/n/1000:7.2f} us   <- THE migration cost")
    print(f"    DELAY total {td/1e6:9.1f} ms   per migration {td/n/1000:7.2f} us   (target-rq wait, NOT summed)")
else:
    print("    no migrations tracked -- check the arm")
for k, lbl in (('rww_ns','rwsem WRITE wait'), ('rwr_ns','rwsem READ wait')):
    v = tot(k)
    if v: print(f"    {lbl:18s} {v[0][0]/1e6:9.1f} ms  (comm {v[0][1]})")
PY
