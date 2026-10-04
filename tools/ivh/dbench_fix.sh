#!/bin/bash
# dbench_fix.sh -- find a dbench config that is LOCK-bound, not IO-bound.
# 2026-10-02: the shipped config (-F on /root/dbench_test, /dev/vda1) measures
# 44.4% iowait / 9.8% cpu and only 2.89% of vCPU time in qspinlock spin, so
# migration has nothing to repair and reads +1.11%. -F forces fsync on every
# write; tmpfs removes the device entirely.
set -u
T=/root/ivh_tools
LOCK=/var/lock/ivh_clean_check.lock
exec 9>"$LOCK"; flock -n 9 || { echo "FATAL: lock held"; exit 1; }
REPS="${1:-2}"; THRESH="${THRESH:-2500000}"
C="ivh_slowpath_wait_ns ivh_slowpath_halt_ns"
OUT=/root/ivh_logs/dbench_fix_$(date +%m%d-%H%M%S).tsv
printf "config\tarm\trep\tmbps\tspin_pct\tiowait_pct\tmigs\n" > "$OUT"
mig(){ python3 $T/migcount.py 2>/dev/null || echo 0; }

# name|dir|extra flags|timelimit
CFGS=(
 "disk_F|/root/dbench_test|-F|15"
 "disk_noF|/root/dbench_test||15"
 "tmpfs_F|/dev/shm/dbench_test|-F|10"
 "tmpfs_noF|/dev/shm/dbench_test||10"
)
for cfg in "${CFGS[@]}"; do
  IFS='|' read -r name dir flags tl <<< "$cfg"
  echo "########## $name  (dbench $flags -t $tl 16 -D $dir) ##########"
  for rep in $(seq 1 $REPS); do
    for arm in pv mig; do
      if [ "$arm" = pv ]; then bash $T/pvbase.sh >/dev/null 2>&1
      else bash $T/p7v2_arm.sh "$THRESH" >/dev/null 2>&1; fi
      sleep 1
      rm -rf "$dir"; mkdir -p "$dir"
      # tmpfs guard: dbench writes ~MB/s*t; bail if shm is already tight
      if [[ "$dir" == /dev/shm/* ]]; then
        avail=$(df -m /dev/shm | awk 'NR==2{print $4}')
        [ "$avail" -lt 4000 ] && { echo "  SKIP: /dev/shm only ${avail}M free"; continue; }
      fi
      sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
      python3 $T/read_ivh_counters.py $C > /tmp/df0.$$ 2>&1
      m0=$(mig)
      vmstat 1 $((tl+1)) > /tmp/dfv.$$ 2>&1 &
      VP=$!
      v=$(timeout 300 dbench $flags -t $tl 16 -D "$dir" 2>&1 9>&- | grep -oP 'Throughput\s+\K[0-9.]+' | head -1)
      wait $VP 2>/dev/null
      m1=$(mig)
      python3 $T/read_ivh_counters.py $C > /tmp/df1.$$ 2>&1
      sp=$(python3 - "$tl" <<'PY'
import re,sys
p=lambda f:{m.group(1):int(m.group(2)) for m in re.finditer(r'^(ivh_\w+)\s*=\s*(\d+)\s*$',open(f).read(),re.M)}
import glob
a=p(glob.glob('/tmp/df0.*')[0]); b=p(glob.glob('/tmp/df1.*')[0])
s=(b.get('ivh_slowpath_wait_ns',0)-a.get('ivh_slowpath_wait_ns',0))-(b.get('ivh_slowpath_halt_ns',0)-a.get('ivh_slowpath_halt_ns',0))
print(f"{100*s/(float(sys.argv[1])*1e9*16):.2f}")
PY
)
      io=$(awk 'NR>3{w+=$16;n++} END{if(n)printf "%.1f",w/n}' /tmp/dfv.$$)
      printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$name" "$arm" "$rep" "${v:-NA}" "$sp" "${io:-NA}" "$((m1-m0))" >> "$OUT"
      echo "  rep$rep $arm = ${v:-NA} MB/s  spin=${sp}%  iowait=${io}%  migs=$((m1-m0))"
      rm -f /tmp/df0.$$ /tmp/df1.$$ /tmp/dfv.$$
    done
  done
  rm -rf /dev/shm/dbench_test
  python3 - "$OUT" "$name" <<'PY'
import sys,statistics as st
rows=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
rows=[r for r in rows if r[0]==sys.argv[2]]
pv=[float(r[3]) for r in rows if r[1]=='pv'  and r[3] not in('NA','')]
mg=[float(r[3]) for r in rows if r[1]=='mig' and r[3] not in('NA','')]
sp=[float(r[4]) for r in rows if r[1]=='pv'  and r[4] not in('NA','')]
io=[float(r[5]) for r in rows if r[1]=='pv'  and r[5] not in('NA','')]
if pv and mg:
    p,g=st.mean(pv),st.mean(mg)
    print(f"  ==> PV {p:.1f}  MIG {g:.1f}  benefit {100*(g-p)/p:+.2f}%"
          f"   | PV-arm spin {st.mean(sp) if sp else 0:.2f}%  iowait {st.mean(io) if io else 0:.1f}%")
PY
done
echo "DONE $OUT"
