#!/bin/bash
# Phase 0b: lock idle time. Run AFTER booting 6.17.0-G-LOCK-27-idleprobe+.
#
# Reports the ABSOLUTE idle time of the "stale + skippable" class -- the time
# handoff rotation could have recovered. Not a baseline subtraction: every
# sample is a head that had already halted (only the hashed release path is
# instrumentable on x86-64), so class 0 is not a healthy control.
set -u
S=/proc/sys/kernel
DUMP=/root/ivh_tools/phase0b_dump.py
fail() { echo "PREFLIGHT FAIL: $*" >&2; exit 1; }

echo "=== preflight ==="
[[ -e $S/ivh_pv_rot_probe ]] || fail "ivh_pv_rot_probe missing -- wrong kernel? ($(uname -r))"
src=$(cat $S/ivh_pv_preempt_src)
[[ "$src" == "2" ]] || fail "ivh_pv_preempt_src=$src, need 2 (heartbeat). At src!=2 the probe falls back to vcpu_is_preempted(), which is not trustworthy in this CVM."
pgrep -x vcap >/dev/null || fail "vcap not running"
pgrep -f MY_ivh_atc  >/dev/null || fail "MY_ivh_atc not running"
printf "  kernel=%s src=%s thr=%s cycles mode=%s\n" \
  "$(uname -r)" "$src" "$(cat $S/ivh_pv_beat_threshold)" "$(cat $S/ivh_adaptive_mode)"
echo "  OK"

run_one() {
  local name="$1"; shift
  echo; echo "=== $name ==="
  python3 "$DUMP" /tmp/p0b_before.json >/dev/null
  local t0 t1; t0=$(date +%s.%N)
  "$@" >/dev/null 2>&1
  t1=$(date +%s.%N)
  python3 "$DUMP" /tmp/p0b_after.json >/dev/null
  python3 /root/ivh_tools/phase0b_report.py /tmp/p0b_before.json /tmp/p0b_after.json \
          "$(python3 -c "print(f'{$t1-$t0:.3f}')")"
}

echo 1 > $S/ivh_pv_rot_probe
trap 'echo 0 > '"$S"'/ivh_pv_rot_probe; echo; echo "probe restored to 0"' EXIT

run_one "qlockbench (ONE lock, deep queue -- the arm that matters)" \
        /root/linux-6.17/qlockbench -d 30
run_one "qlockbench -p (CONTROL: many locks, no deep queue)" \
        /root/linux-6.17/qlockbench -d 30 -p
run_one "hackbench (many locks; idle-time %% of wall is NOT valid here)" \
        hackbench -T -g1 -f8 -l400000
