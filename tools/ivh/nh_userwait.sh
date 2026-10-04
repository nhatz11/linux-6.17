#!/bin/bash
# nhextend's OWN lock wait, not the kernel qspinlock counter.
# NHextend's threads contend on its userspace AFL lock; ivh_slowpath_wait_ns
# counts kernel qspinlocks and does not describe those threads at all.
# "Total wait time" (NHextend-csmin.c:1668) is microseconds, summed over threads.
set -u
T=/root/ivh_tools; S=/proc/sys/kernel
BLOCKS="${1:-2}"; PER="${2:-2}"
NH="env NHEXTEND_DURATION=8 NHEXTEND_LOOP_SPIN=600000 IVH_AFL_DISABLE=1 NHEXTEND_CS_MIN=1 /root/linux-6.17/NHextend-csmin -l -n 16"
OUT=/root/ivh_logs/nh_userwait_$(date +%m%d-%H%M%S).tsv
printf "arm\tblock\trep\titers\ttotal_wait_us\tavg_wait_us\tmax_wait\tmigs\n" > "$OUT"
mig(){ python3 $T/migcount.py 2>/dev/null | tail -1 | grep -oE '[0-9]+$'; }
arm(){ case $1 in
    pv_t1)  bash $T/pvbase.sh >/dev/null 2>&1 ;;
    mig_t1) bash $T/p7v2_arm.sh 2500000 >/dev/null 2>&1
            echo 0 > $S/ivh_pv_tier2_enable; echo 1 > $S/ivh_cs_gate2_reference ;;
  esac; echo 1 > $S/ivh_pv_tier1_enable; sleep 1; }
for b in $(seq 1 "$BLOCKS"); do
  if [ $((b % 2)) -eq 1 ]; then ORDER="pv_t1 mig_t1"; else ORDER="mig_t1 pv_t1"; fi
  for a in $ORDER; do
    arm $a
    ( cd /root && eval "timeout 90 $NH" >/dev/null 2>&1 )          # warmup
    for r in $(seq 1 "$PER"); do
      m0=$(mig)
      O=$( cd /root && eval "timeout 90 $NH" 2>&1 )
      m1=$(mig)
      it=$(echo "$O"  | grep -oP 'Ran for \K[0-9]+')
      # "Total wait time: SECS.USEC  (avg: SECS.USEC)" -> us
      tw=$(echo "$O" | grep -oP '^Total wait time: \K[0-9]+\.[0-9]+' | head -1)
      aw=$(echo "$O" | grep -oP 'avg: \K[0-9]+\.[0-9]+' | head -1)
      mx=$(echo "$O" | grep -oP '^ *max wait: \K[0-9]+' | head -1)
      twus=$(python3 -c "s='${tw:-0}'.split('.');print(int(s[0])*1000000+int(s[1]))" 2>/dev/null || echo 0)
      awus=$(python3 -c "s='${aw:-0}'.split('.');print(int(s[0])*1000000+int(s[1]))" 2>/dev/null || echo 0)
      printf "%s\t%d\t%d\t%s\t%s\t%s\t%s\t%d\n" "$a" "$b" "$r" "${it:-NA}" "$twus" "$awus" "${mx:-0}" "$((m1-m0))" >> "$OUT"
      printf "  b%d %-7s r%d iters=%-7s userlock_wait=%.1f ms avg=%s us migs=%d\n" \
        "$b" "$a" "$r" "${it:-NA}" "$(python3 -c "print($twus/1000)")" "${awus:-0}" "$((m1-m0))"
    done
  done
done
bash $T/pvbase.sh >/dev/null 2>&1; echo 0 > $S/ivh_cs_gate2_reference
python3 - "$OUT" <<'PY'
import sys, statistics as st
r=[l.split('\t') for l in open(sys.argv[1]).read().splitlines()[1:] if l.strip()]
d={}
for x in r:
    if x[3]=='NA': continue
    d.setdefault(x[0],{'i':[],'w':[],'m':[]})
    d[x[0]]['i'].append(float(x[3])); d[x[0]]['w'].append(float(x[4])); d[x[0]]['m'].append(int(x[7]))
A,B=d['pv_t1'],d['mig_t1']
ia,ib=st.mean(A['i']),st.mean(B['i']); wa,wb=st.mean(A['w']),st.mean(B['w'])
print(f"\n  ===== NHEXTEND, USERSPACE AFL LOCK WAIT (its own counter) =====")
print(f"    iters      pv {ia:10.0f}   mig {ib:10.0f}   {100*(ib-ia)/ia:+7.2f}%")
print(f"    user wait  pv {wa/1000:10.1f} ms  mig {wb/1000:10.1f} ms  {100*(wa-wb)/wa:+7.2f}%  (saved {(wa-wb)/1000:+.1f} ms)")
print(f"    migs/run   {int(st.mean(B['m']))}  x 5.98us MECH = {st.mean(B['m'])*5.98/1000:.1f} ms migration cost")
sv=(wa-wb)*1000; c=st.mean(B['m'])*5980
print(f"    saving/cost {sv/c:6.2f}x" if c else "")
PY
echo "NH_USERWAIT_DONE $OUT"
