#!/bin/bash
# dbench_fix2.sh -- stage 2: make dbench LOCK-bound at 16 clients.
#
# Stage 1 showed the shipped config is IO-bound (iowait 45.8%, qspinlock spin
# 2.96% of vCPU time, benefit -1.06%). Migration can only help a workload that
# SPINS on kernel locks while its vCPU is preempted, so the job is to move
# dbench's time out of the device and into contended locks.
#
# Levers, strongest first:
#   -S  sync-dir : every create/unlink syncs the PARENT directory -> all clients
#                  serialise on directory inode/dentry locks
#   -x  xattr    : adds inode-lock work per op
#   small tmpfs  : a tight size forces shmem reclaim on every allocation
#   huge=always  : THP allocation pulls in compaction/zone locks
# A dedicated tmpfs is mounted at /mnt/dbt and unmounted at exit; nothing
# outside /mnt/dbt is touched.
set -u
T=/root/ivh_tools
LOCK=/var/lock/ivh_clean_check.lock
exec 9>"$LOCK"; flock 9 || exit 1          # BLOCKING: queue behind stage 1
REPS="${1:-2}"; THRESH="${THRESH:-2500000}"; TL="${TL:-10}"
MNT=/mnt/dbt
OUT=/root/ivh_logs/dbench_fix2_$(date +%m%d-%H%M%S).tsv
C="ivh_slowpath_wait_ns ivh_slowpath_halt_ns"
printf "config\tarm\trep\tmbps\tspin_pct\tiowait_pct\tmigs\n" > "$OUT"
mig(){ python3 $T/migcount.py 2>/dev/null || echo 0; }
cleanup(){ umount "$MNT" 2>/dev/null; rmdir "$MNT" 2>/dev/null; }
trap cleanup EXIT
mkdir -p "$MNT"

# ---- footprint probe: how much does dbench actually need? ----
mount -t tmpfs -o size=6G tmpfs "$MNT" 2>/dev/null || { echo "FATAL: cannot mount tmpfs"; exit 1; }
bash $T/pvbase.sh >/dev/null 2>&1; sleep 1
timeout 300 dbench -t "$TL" 16 -D "$MNT" >/dev/null 2>&1 9>&- &
DP=$!; PEAK=0
while kill -0 $DP 2>/dev/null; do
  u=$(df -m "$MNT" | awk 'NR==2{print $3}'); [ "$u" -gt "$PEAK" ] && PEAK=$u; sleep 0.5
done
wait $DP 2>/dev/null
echo "### dbench 16-client peak footprint on tmpfs: ${PEAK} MB (t=$TL)"
umount "$MNT"
TIGHT=$(( PEAK * 115 / 100 + 64 ))        # 15% headroom: tight but no ENOSPC
echo "### tight tmpfs size = ${TIGHT}M"

run_cfg(){  # $1 name  $2 mount opts (empty = no mount, use dir as-is)  $3 dbench flags
  local name="$1" mopts="$2" flags="$3"
  echo "########## $name  (dbench $flags -t $TL 16)  tmpfs opts: ${mopts:-none} ##########"
  for rep in $(seq 1 $REPS); do
    for arm in pv mig; do
      if [ "$arm" = pv ]; then bash $T/pvbase.sh >/dev/null 2>&1
      else bash $T/p7v2_arm.sh "$THRESH" >/dev/null 2>&1; fi
      sleep 1
      umount "$MNT" 2>/dev/null
      mount -t tmpfs -o "$mopts" tmpfs "$MNT" || { echo "  MOUNTFAIL"; continue; }
      sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      python3 $T/read_ivh_counters.py $C > /tmp/g0.$$ 2>&1
      m0=$(mig)
      vmstat 1 $((TL+1)) > /tmp/gv.$$ 2>&1 & VP=$!
      v=$(timeout 300 dbench $flags -t "$TL" 16 -D "$MNT" 2>&1 9>&- | grep -oP 'Throughput\s+\K[0-9.]+' | head -1)
      wait $VP 2>/dev/null
      m1=$(mig)
      python3 $T/read_ivh_counters.py $C > /tmp/g1.$$ 2>&1
      sp=$(python3 -c "
import re,sys
p=lambda f:{m.group(1):int(m.group(2)) for m in re.finditer(r'^(ivh_\w+)\s*=\s*(\d+)\s*\$',open(f).read(),re.M)}
a,b=p('/tmp/g0.$$'),p('/tmp/g1.$$')
s=(b.get('ivh_slowpath_wait_ns',0)-a.get('ivh_slowpath_wait_ns',0))-(b.get('ivh_slowpath_halt_ns',0)-a.get('ivh_slowpath_halt_ns',0))
print(f'{100*s/($TL*1e9*16):.2f}')")
      io=$(awk 'NR>3{w+=$16;n++} END{if(n)printf "%.1f",w/n}' /tmp/gv.$$)
      printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$name" "$arm" "$rep" "${v:-NA}" "$sp" "${io:-NA}" "$((m1-m0))" >> "$OUT"
      echo "  rep$rep $arm = ${v:-NA} MB/s  spin=${sp}%  iowait=${io}%  migs=$((m1-m0))"
      rm -f /tmp/g0.$$ /tmp/g1.$$ /tmp/gv.$$
    done
  done
  umount "$MNT" 2>/dev/null
  python3 - "$OUT" "$name" <<'PY'
import sys,statistics as st
rows=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
rows=[r for r in rows if r[0]==sys.argv[2]]
pv=[float(r[3]) for r in rows if r[1]=='pv'  and r[3] not in('NA','')]
mg=[float(r[3]) for r in rows if r[1]=='mig' and r[3] not in('NA','')]
sp=[float(r[4]) for r in rows if r[1]=='pv'  and r[4] not in('NA','')]
mi=[int(r[6])   for r in rows if r[1]=='mig']
if pv and mg:
    p,g=st.mean(pv),st.mean(mg)
    b=100*(g-p)/p
    tag="  *** WIN ***" if b>=10 else ("  (positive)" if b>0 else "")
    print(f"  ==> PV {p:.1f}  MIG {g:.1f}  benefit {b:+.2f}%"
          f"   | PV spin {st.mean(sp) if sp else 0:.2f}%  migs {st.mean(mi):.0f}{tag}")
PY
}

run_cfg tmpfs_plain  "size=6G"               ""
run_cfg tmpfs_S      "size=6G"               "-S"
run_cfg tmpfs_SF     "size=6G"               "-S -F"
run_cfg tmpfs_x      "size=6G"               "-x"
run_cfg tmpfs_tight  "size=${TIGHT}M"        "-S"
run_cfg tmpfs_huge   "size=6G,huge=always"   "-S"
echo "DONE $OUT"
