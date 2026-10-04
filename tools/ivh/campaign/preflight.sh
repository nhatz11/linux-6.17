#!/bin/bash
# preflight.sh -- assert the box is in a valid state for an IVH A/B campaign.
#
# Exists because a campaign that runs with one sysctl wrong produces numbers
# that look fine and mean nothing. goto_mode.sh already verifies the CALIBRATION
# knobs; this adds the four things it cannot check:
#
#   1. both arms actually take      (spin_mode / universal_eligible round-trip)
#   2. the host is actually contending   (capacity differential 0-7 vs 8-15)
#   3. migration actually FIRES          (ivh_migrations_done advances)
#   4. adaptive spinning can fire        (preempt_src is usable in mode 2)
#
# Exit 0 = safe to run a campaign. Non-zero = do not trust anything measured.
set -u
S=/proc/sys/kernel
FAIL=0
note() { printf "  %-6s %s\n" "$1" "$2"; }
bad()  { note FAIL "$1"; FAIL=1; }
ok()   { note ok   "$1"; }

echo "=== IVH campaign preflight -- $(date) ==="
echo "kernel $(uname -r)"

# ---- 1. calibration (delegate; goto_mode.sh exits 1 if a knob is wrong) ----
echo
echo "--- calibration + daemons (goto_mode.sh ivh-as kernel) ---"
if /root/linux-6.17/cvm_setup/goto_mode.sh ivh-as kernel 2>&1 | tail -20; then
    ok "goto_mode.sh reported calibration OK"
else
    bad "goto_mode.sh FAILED -- calibration incomplete, stop here"
fi

# ---- 2. arms round-trip ----
echo
echo "--- arm switching ---"
/root/spin_mode 1 >/dev/null 2>&1; echo 0 > $S/ivh_universal_eligible
el=$(cat $S/ivh_universal_eligible); am=$(cat $S/ivh_adaptive_mode)
[ "$el" = 0 ] && [ "$am" = 0 ] && ok "pv arm takes (eligible=0 adaptive_mode=0)" \
                              || bad "pv arm did NOT take (eligible=$el adaptive_mode=$am)"
/root/spin_mode 2 >/dev/null 2>&1; echo 1 > $S/ivh_universal_eligible
el=$(cat $S/ivh_universal_eligible); am=$(cat $S/ivh_adaptive_mode)
[ "$el" = 1 ] && [ "$am" = 2 ] && ok "ivh arm takes (eligible=1 adaptive_mode=2)" \
                              || bad "ivh arm did NOT take (eligible=$el adaptive_mode=$am)"

# ---- 3. adaptive spinning can actually fire in mode 2 ----
# vcpu_is_preempted() is hardwired false without KVM_FEATURE_STEAL_TIME, which
# this TDX host does not offer; tier 2 then never fires. preempt_src=2 selects
# the TSC heartbeat instead. dmesg says this explicitly at mode-2 entry.
echo
echo "--- tier-2 preemption source ---"
ps=$(cat $S/ivh_pv_preempt_src)
if [ "$ps" = 2 ]; then
    ok "ivh_pv_preempt_src=2 (TSC heartbeat)"
else
    echo 2 > $S/ivh_pv_preempt_src 2>/dev/null
    ps=$(cat $S/ivh_pv_preempt_src)
    [ "$ps" = 2 ] && ok "ivh_pv_preempt_src corrected to 2 (TSC heartbeat)" \
                  || bad "ivh_pv_preempt_src=$ps -- tier 2 CANNOT fire on this host"
fi
t1=$(cat $S/ivh_pv_tier1_enable); t2=$(cat $S/ivh_pv_tier2_enable)
[ "$t1" = 1 ] && [ "$t2" = 1 ] && ok "tier1=$t1 tier2=$t2" \
                              || bad "tier1=$t1 tier2=$t2 -- expected 1/1"

# ---- 4. host contention: capacity differential ----
echo
echo "--- host contention (capacity, per vCPU group) ---"
cap=$(python3 - <<'PY' 2>/dev/null || echo NA
import sys; sys.path.insert(0,"/root/ivh_tools")
import read_vact_rq as r, statistics as st
sym=r.load_kallsyms(); cpus=r.online_cpus()
with open(r.KCORE,"rb") as f:
    ph=r.read_phdrs(f)
    offs=[r.read_u64(f,ph,sym["__per_cpu_offset"]+8*c) for c in cpus]
    v=[r.read_u64(f,ph,sym["runqueues"]+3824+o) for o in offs]
print(f"{round(st.mean(v[:8]))} {round(st.mean(v[8:]))}")
PY
)
if [ "$cap" = NA ]; then
    bad "could not read capacity from /proc/kcore"
else
    lo=$(cut -d' ' -f1 <<<"$cap"); hi=$(cut -d' ' -f2 <<<"$cap")
    echo "         cpu0-7 = $lo   cpu8-15 = $hi   (1024 = uncontended)"
    if [ "$lo" -lt 900 ] && [ "$hi" -gt 900 ]; then
        ok "half contention present on cpu0-7, cpu8-15 quiet -- matches the 2026-09-15 shape"
    elif [ "$lo" -gt 900 ] && [ "$hi" -gt 900 ]; then
        bad "BOTH groups uncontended -- the co-tenant VM is not running. Migration has nowhere to migrate FROM; every migration verdict will read as a loss (see ivh_migration_host_contention_confound)"
    else
        note warn "unexpected contention shape -- results are not comparable to 10.4"
    fi
fi

# ---- 5. migration actually fires ----
echo
echo "--- migration liveness (10s hackbench, ivh arm) ---"
echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1
m0=$(python3 /root/ivh_tools/migcount.py 2>/dev/null)
timeout 60 hackbench -T -g1 -f8 -l60000 >/dev/null 2>&1
m1=$(python3 /root/ivh_tools/migcount.py 2>/dev/null)
if [ -z "$m0" ] || [ -z "$m1" ]; then
    bad "migcount.py could not read ivh_migrations_done"
else
    d=$((m1-m0))
    echo "         ivh_migrations_done $m0 -> $m1  (delta $d)"
    [ "$d" -gt 0 ] && ok "migration is firing" \
                   || bad "ZERO migrations under load -- the ivh arm is testing nothing"
fi

# ---- 6. control: the pv arm must NOT migrate ----
echo 0 > $S/ivh_universal_eligible; /root/spin_mode 1 >/dev/null 2>&1
m0=$(python3 /root/ivh_tools/migcount.py 2>/dev/null)
timeout 60 hackbench -T -g1 -f8 -l60000 >/dev/null 2>&1
m1=$(python3 /root/ivh_tools/migcount.py 2>/dev/null)
d=$((m1-m0))
echo "         pv-arm control: delta $d"
[ "$d" = 0 ] && ok "pv arm makes 0 migrations (clean control)" \
             || bad "pv arm made $d migrations -- the gate is leaking, arms are not separated"

# ---- 7. disk + dmesg ----
echo
echo "--- housekeeping ---"
free_gb=$(df -BG --output=avail /root | tail -1 | tr -dc '0-9')
[ "${free_gb:-0}" -ge 10 ] && ok "${free_gb}G free on /root" || bad "only ${free_gb}G free on /root"
hard=$(dmesg 2>/dev/null | grep -ciE 'soft lockup|rcu[_ ]*sched.*stall|hung task|BUG:|kernel panic|Oops')
[ "$hard" = 0 ] && ok "no hard kernel errors in dmesg" || note warn "$hard hard kernel error line(s) already in dmesg"

echo
if [ "$FAIL" = 0 ]; then
    echo "=== PREFLIGHT PASS -- safe to run a campaign ==="
else
    echo "=== PREFLIGHT FAIL -- do NOT trust anything measured until fixed ==="
fi
# leave the box in the ivh arm, as run_campaign.sh's EXIT trap does
echo 1 > $S/ivh_universal_eligible; /root/spin_mode 2 >/dev/null 2>&1
exit $FAIL
