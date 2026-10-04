#!/bin/bash
# Cross-kernel neutrality check: identical IVH+AS hackbench on whatever kernel
# is booted. Run once on G-LOCK-28 (before reboot) and once on G-LOCK-29 with
# every ivh_cs_* switch at 0. Across a reboot, so it only catches regressions
# bigger than host drift (a few %); the interleaved A/B is the precise check.
set -u
ROUNDS=${ROUNDS:-5}
W="hackbench -T -g1 -f8 -l400000"
S=/proc/sys/kernel
MODE=${MODE:-ivhas}   # ivhas = migration ON + spin_mode 2; pv = migration OFF + spin_mode 1
OUT=/root/ivh_tools/cross_kernel_baseline_$(uname -r)_${MODE}.log

if [ "$MODE" = pv ]; then
    echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null; want_am=0
else
    echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null; want_am=2
fi
[ -e $S/ivh_pv_rot_enable ] && echo 0 > $S/ivh_pv_rot_enable
[ -e $S/ivh_pv_rot_probe ]  && echo 0 > $S/ivh_pv_rot_probe
for f in ivh_cs_owner_enable ivh_cs_owner_clear ivh_cs_head_probe ivh_cs_head_bail; do
    [ -e $S/$f ] && [ "$(cat $S/$f)" != 0 ] && { echo "FATAL: $f != 0"; exit 1; }
done
am=$(cat $S/ivh_adaptive_mode); ps=$(cat $S/ivh_pv_preempt_src)
if [ "$want_am" = 2 ]; then
    { [ "$am" = 2 ] && [ "$ps" = 2 ]; } || { echo "FATAL: adaptive_mode=$am preempt_src=$ps"; exit 1; }
else
    [ "$am" = 0 ] || { echo "FATAL: adaptive_mode=$am expected 0"; exit 1; }
fi

{
echo "=== kernel $(uname -r)  mode=$MODE  $(date -Is) ==="
for f in ivh_universal_eligible ivh_adaptive_mode ivh_pv_preempt_src ivh_pv_beat_threshold \
         ivh_pv_spin_threshold ivh_pv_tier1_enable ivh_pv_rot_enable ivh_pv_rot_probe \
         ivh_cs_owner_enable ivh_cs_owner_clear ivh_cs_head_probe ivh_cs_head_bail; do
    [ -e $S/$f ] && printf "  %-26s %s\n" "$f" "$(cat $S/$f)"
done
echo "  loadavg before: $(cat /proc/loadavg)"
for i in $(seq 1 $ROUNDS); do
    v=$($W 2>&1 | grep -oP '^Time:\s*\K[0-9.]+')
    echo "round $i time=${v}s"
done
} | tee "$OUT"
python3 - "$OUT" <<'PY'
import re,sys,statistics as st
t=[float(x) for x in re.findall(r'time=([0-9.]+)s',open(sys.argv[1]).read())]
print(f"n={len(t)} mean={st.mean(t):.2f}s median={st.median(t):.2f}s sd={st.stdev(t):.2f}s range={min(t):.2f}-{max(t):.2f}")
PY
