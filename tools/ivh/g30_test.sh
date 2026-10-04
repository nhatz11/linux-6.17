#!/bin/bash
# g30_test.sh -- reproduce the September campaign's ebizzy measurement on the
# G-LOCK-30 kernel, then compare against G-LOCK-48.
#
# G-LOCK-30 predates most of the sysctls the current harnesses assert
# (ivh_cs_head_bail, ivh_pv_evict_*, ivh_head_bypass_*, ivh_pv_tier1_halt_min,
# ivh_pv_trylock_relaxed, ivh_pv_skip_point ...), so pvbase.sh / p78_arm.sh /
# whyless.sh CANNOT run there. This uses the campaign's arms verbatim, from
# campaign/run_campaign.sh:36-37 -- and nothing else:
#
#     pv)  echo 0 > ivh_universal_eligible ; spin_mode 1
#     ivh) echo 1 > ivh_universal_eligible ; spin_mode 2
#
# Campaign reference (kernel 6.17.0-G-LOCK-30-csfast+, capacity 456-475,
# n=16 interleaved): pv median 978.0, ivh median 2008.5 -> +104.3%.
# G-LOCK-48 at capacity ~505 gives pv ~989, ivh ~1400 -> ~+42%.
set -u
S=/proc/sys/kernel
REPS="${REPS:-8}"
OUT="${OUT:-/root/ivh_tools/g30_$(date +%m%d-%H%M%S).csv}"
EB="/home/nick/Desktop/ebizzy -S 15 -t 16 -m -s 4194304"

echo "kernel: $(uname -r)"
[ -e $S/ivh_universal_eligible ] || { echo "FATAL: no ivh sysctls"; exit 1; }
pgrep -x MY_ivh_atc >/dev/null || echo "WARN: MY_ivh_atc not running -- migration target selection will fall back to CPU 0"
pgrep -x vcap >/dev/null || echo "WARN: vcap not running -- capacity estimate may be flat"

setarm(){
  case "$1" in
    pv)  echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1
         echo 0 > $S/ivh_universal_eligible ;;   # spin_mode may reset it
    ivh) /root/spin_mode 2 >/dev/null 2>&1; echo 1 > $S/ivh_universal_eligible
         echo 2 > $S/ivh_pv_preempt_src 2>/dev/null
         echo 2 > $S/ivh_preempt_event_source 2>/dev/null ;;
  esac
  local el am; el=$(cat $S/ivh_universal_eligible); am=$(cat $S/ivh_adaptive_mode)
  case "$1" in
    pv)  [ "$el" = 0 ] && [ "$am" = 0 ] || { echo "FATAL pv: el=$el am=$am"; return 1; } ;;
    ivh) [ "$el" = 1 ] && [ "$am" = 2 ] || { echo "FATAL ivh: el=$el am=$am"; return 1; } ;;
  esac
  return 0
}
echo "arm,rep,records_per_s,dur_s,capacity_contended" > "$OUT"
cap(){ python3 /root/ivh_tools/read_vact_rq.py ivh_uc_capacity 2>/dev/null \
       | sed 's/.*per-cpu=\[//;s/\].*//' \
       | python3 -c "import sys;v=[int(x) for x in sys.stdin.read().split(',')];print(int(sum(v[:8])/8))" 2>/dev/null || echo 0; }
echo "== g30_test: $REPS blocks of [pv ivh ivh pv] (campaign order) -> $OUT"
for r in $(seq 1 "$REPS"); do
  if [ $((r % 2)) -eq 1 ]; then ORDER="pv ivh ivh pv"; else ORDER="ivh pv pv ivh"; fi
  for a in $ORDER; do
    setarm "$a" || continue
    sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; sleep 1
    c=$(cap); t0=$(date +%s.%N)
    v=$(cd /root && eval "$EB" 2>&1 | grep -oP '^\K[0-9]+(?= records/s)' | tail -1)
    t1=$(date +%s.%N)
    python3 -c "print(f'$a,$r,${v:-0},{$t1-$t0:.3f},$c')" >> "$OUT"
    printf "  %-4s r%-2s %8s records/s  cap=%s\n" "$a" "$r" "${v:-0}" "$c"
  done
done
python3 - "$OUT" <<'PY'
import sys,csv,statistics as st
rows=list(csv.DictReader(open(sys.argv[1])))
g=lambda a:[float(r['records_per_s']) for r in rows if r['arm']==a and float(r['records_per_s'])>0]
pv,iv=g('pv'),g('ivh')
c=[float(r['capacity_contended']) for r in rows if float(r['capacity_contended'])>0]
print(f"\n  kernel {__import__('platform').release()}")
if c: print(f"  capacity (contended half) median={st.median(c):.0f}")
for n,v in [('pv',pv),('ivh',iv)]:
    if v: print(f"  {n:4} n={len(v)} median={st.median(v):7.1f} CV={100*st.pstdev(v)/st.mean(v):4.1f}%  range={min(v):.0f}-{max(v):.0f}")
if pv and iv: print(f"  IVH vs PV: {100*(st.median(iv)-st.median(pv))/st.median(pv):+.2f}%")
print(f"  CAMPAIGN reference: pv 978.0  ivh 2008.5  -> +104.3%  (capacity 456-475)")
print(f"  G-LOCK-48 today   : pv  989    ivh ~1400   -> ~+42%    (capacity ~505)")
PY
echo G30-TEST-DONE
