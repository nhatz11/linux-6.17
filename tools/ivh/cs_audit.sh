#!/bin/bash
# Does the CS HOLD-DURATION predicate find the right target?
#
#   fire  = is_cs_preempted() said "the holder is stuck". Criterion 1:
#           held > that cpu's last completed hold + ivh_cs_noise_cycles.
#           held is an EXACT rdtsc delta from a stamp the OWNER wrote at
#           acquire -- no heartbeat, no publish cadence, no 1 ms tick floor.
#   truth = at unlock the holder asks its OWN rq whether its TSC jumped
#           during the hold -> ivh_cs_v_flagged[cause][verdict].
#           Self-observation is reliable; remote observation is not.
#
# DETECT-ONLY: ivh_cs_head_bail=0, so nothing here changes behaviour.
# The sampler is ARMED for verdict resolution and costs ~48% throughput,
# so NO throughput number from this run is valid.
#
# The control column is the point: base-rate is the same verdict computed
# over holds the predicate did NOT flag. precision/base-rate = lift. A
# predicate that fires at random has lift 1.0 no matter how high precision
# looks.
S=/proc/sys/kernel
set_() {
	echo "$2" > $S/$1 2>/dev/null
	[ "$(cat $S/$1 2>/dev/null)" = "$2" ] || echo "  !! $1 rejected ($2)"
}
set_ ivh_cs_track_enabled 1
set_ ivh_cs_owner_enable  1
set_ ivh_cs_owner_clear   1
set_ ivh_cs_head_probe    1
set_ ivh_cs_head_bail     0
set_ ivh_pv_rot_enable    0
set_ ivh_cs_verdict       1
set_ ivh_cs_criterion     1
set_ ivh_tks_sampler_ns   200000
set_ ivh_vact_jump_ns     300000
set_ ivh_vact_min_preempt_ns 2000
set_ ivh_universal_eligible  0

R="python3 /root/ivh_tools/read_ivh_counters.py"
N="ivh_cs_v_flagged ivh_cs_v_unflagged ivh_cs_fired ivh_cs_ep_events_by_end"

arm() {
	set_ ivh_cs_noise_cycles "$1"
	$R $N > /tmp/cs_a.txt 2>&1
	timeout 90 hackbench -T -g1 -f8 -l250000 >/dev/null 2>&1
	$R $N > /tmp/cs_b.txt 2>&1
	python3 - "$2" <<'PY'
import re, sys
def p(f):
    s = {}; arr = {}
    for ln in open(f):
        m = re.match(r'^(ivh_\w+)\s+\[(\S+)\s*\]\s+sum=(\d+)\s+nonzero_buckets=\[(.*)\]', ln)
        if m:
            arr[(m.group(1), m.group(2))] = {int(a): int(b) for a, b in
                re.findall(r'\((\d+),\s*(\d+)\)', m.group(4))}
            continue
        m = re.match(r'^(ivh_\w+)\s+\[(\S+)\s*\]\s+=\s+(\d+)', ln)
        if m:
            s[(m.group(1), m.group(2))] = int(m.group(3)); continue
        m = re.match(r'^(ivh_\w+)\s+=\s+(\d+)', ln)
        if m:
            s[(m.group(1), '')] = int(m.group(2))
    return s, arr

sa, aa = p('/tmp/cs_a.txt'); sb, ab = p('/tmp/cs_b.txt')
CAUSES = ("NONE", "TIER1", "TIER2", "EXHAUST", "TIER1_AGREED", "TIER1_DISAGREED")
f = [0]*4; u = [0]*4
for c in CAUSES:
    A = aa.get(('ivh_cs_v_flagged', c), {}); B = ab.get(('ivh_cs_v_flagged', c), {})
    for i in range(4): f[i] += B.get(i, 0) - A.get(i, 0)
    A = aa.get(('ivh_cs_v_unflagged', c), {}); B = ab.get(('ivh_cs_v_unflagged', c), {})
    for i in range(4): u[i] += B.get(i, 0) - A.get(i, 0)

n = sum(f); j = f[0] + f[1] + f[2]
nu = sum(u); ju = u[0] + u[1] + u[2]
prec = (100.0*(f[1]+f[2])/j) if j else None
base = (100.0*(u[1]+u[2])/ju) if ju else None
ps = f"{prec:5.1f}%" if prec is not None else "  n/a"
bs = f"{base:6.3f}%" if base is not None else "   n/a"
lift = f"{prec/base:5.1f}x" if (prec and base) else "  n/a"
print(f"noise={sys.argv[1]:>7}  flagged n={n:<7} judg={j:<7} "
      f"[no={f[0]} PRE={f[1]} amb={f[2]} unk={f[3]}]  PREC={ps}"
      f"  | unflagged base={bs} (n={nu})  LIFT={lift}")
PY
}

echo "CS hold-duration predicate (criterion 1), DETECT-ONLY.  noise_cycles: 2200 cyc = 1us"
echo "PREC = flagged holds whose holder self-reports a TSC jump."
echo "base = same verdict over UNflagged holds.  LIFT = PREC/base (1.0x means the predicate is random)."
for c in 4400 11000 22000 44000 110000; do
	arm $c "$((c/2200))us"
done
set_ ivh_tks_sampler_ns 0
set_ ivh_cs_verdict     0
set_ ivh_cs_criterion   0
echo CSAUDIT-DONE
