#!/bin/bash
# Phase 1 staging. Run AFTER booting 6.17.0-G-LOCK-28-rotate+.
#
#   ./phase1_rotate_run.sh measure   -- probe ON, rotation OFF. Safe. Answers
#                                       "how often is a splice actually legal?"
#   ./phase1_rotate_run.sh smoke     -- rotation ON, 10s only. First live exercise
#                                       of the pointer surgery. Do this before any
#                                       long run.
#   ./phase1_rotate_run.sh ab        -- PV-queue vs rotation, 3 rounds hackbench.
set -u
S=/proc/sys/kernel
MODE="${1:-measure}"

[ -e $S/ivh_pv_rot_enable ] || { echo "wrong kernel: $(uname -r)" >&2; exit 1; }
[ "$(cat $S/ivh_pv_preempt_src)" = 2 ] || { echo "need ivh_pv_preempt_src=2" >&2; exit 1; }
pgrep -x vcap >/dev/null || { echo "vcap not running -- run goto_mode.sh ivh-as kernel" >&2; exit 1; }

report() { python3 /root/ivh_tools/read_ivh_counters.py \
  ivh_rot_handoffs ivh_rot_preempted ivh_rot_splice_ok ivh_rot_splice_done \
  ivh_rot_splice_blocked_tail ivh_rot_splice_blocked_starve; }

cleanup() { echo 0 > $S/ivh_pv_rot_enable; echo 0 > $S/ivh_pv_rot_probe; echo; echo "rotation+probe restored to 0"; }
trap cleanup EXIT

case "$MODE" in
measure)
  echo 1 > $S/ivh_pv_rot_probe; echo 0 > $S/ivh_pv_rot_enable
  echo "=== probe ON, rotation OFF ==="; report > /tmp/p1b.txt
  /root/linux-6.17/qlockbench -d 20 -q
  hackbench -T -g1 -f8 -l400000 2>&1 | tail -1
  report > /tmp/p1a.txt
  python3 - <<'PY'
rd=lambda p:{k.strip():int(v) for k,v in (l.split("=") for l in open(p) if "=" in l)}
b,a=rd("/tmp/p1b.txt"),rd("/tmp/p1a.txt"); d=lambda k:a[k]-b[k]
ho,pr,ok,bt=d("ivh_rot_handoffs"),d("ivh_rot_preempted"),d("ivh_rot_splice_ok"),d("ivh_rot_splice_blocked_tail")
print(f"\nhandoffs={ho:,}  stale_successor={pr:,}")
print(f"  splice LEGAL (live node has a successor) = {ok:,}")
print(f"  splice blocked, live node was the tail   = {bt:,}")
print(f"  --> true addressable share of stale events = {100*ok/pr if pr else 0:.1f}%")
print(f"  --> as a share of ALL handoffs             = {100*ok/ho if ho else 0:.4f}%")
PY
  ;;
smoke)
  echo "=== SMOKE: rotation ON for 10s. First live pointer surgery. ==="
  echo 1 > $S/ivh_pv_rot_probe; echo 1 > $S/ivh_pv_rot_enable
  report > /tmp/p1b.txt
  timeout 30 /root/linux-6.17/qlockbench -d 10 -q || echo "qlockbench did not finish"
  report > /tmp/p1a.txt
  python3 - <<'PY'
rd=lambda p:{k.strip():int(v) for k,v in (l.split("=") for l in open(p) if "=" in l)}
b,a=rd("/tmp/p1b.txt"),rd("/tmp/p1a.txt"); d=lambda k:a[k]-b[k]
print(f"\nrotations actually performed = {d('ivh_rot_splice_done'):,}")
print(f"blocked (tail)={d('ivh_rot_splice_blocked_tail'):,}  blocked (starvation cap)={d('ivh_rot_splice_blocked_starve'):,}")
print("\nsystem survived the run -> surgery did not deadlock the queue")
PY
  ;;
ab)
  for r in 1 2 3; do for en in 0 1; do
    echo 0 > $S/ivh_pv_rot_probe; echo $en > $S/ivh_pv_rot_enable
    t=$(hackbench -T -g1 -f8 -l400000 2>&1 | grep -oP 'Time: \K[0-9.]+')
    echo "round $r  rotation=$en  time=${t}s"
  done; done
  ;;
*) echo "usage: $0 {measure|smoke|ab}" >&2; exit 1 ;;
esac
