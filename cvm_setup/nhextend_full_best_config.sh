#!/bin/bash
# The validated combination that produced the IVH+adaptivespin results in
# tools/bpf/docs/ivh_nhextend_adaptive_futex_lock_2026-09-12.md:
#   loop_spin=5000    (~13us CS):  +17.5% vs IVH alone, +42.5% vs stock PV
#   loop_spin=600000  (~1.6ms CS): +19.7% vs IVH alone, +129.3% vs stock PV
#
# Two independent mechanisms, both required:
#   1. The migration engine (kernel sysctls below + two daemons + one BPF
#      map write) -- "IVH" on its own.
#   2. The userspace adaptive-spin lock in NHextend-full (ivh_adaptive_futex_lock.h)
#      -- env vars only, no rebuild needed for wake/stale/spins tuning.
#
# Order matters: the BPF program (MY_ivh_atc) must be loaded and ivh_cfg
# written BEFORE ivh_universal_eligible is set to 1 -- otherwise every
# eligible task's migration target defaults to CPU 0 (thundering herd), per
# kernel/sched/fair.c's own safety-note comment.
set -u

REPO=/root/kernels/linux-6.17-vanilla

set_ivh_sysctl() {
    local path="/proc/sys/kernel/$1"
    [ -f "$path" ] && echo "$2" | tee "$path" > /dev/null
}

# --- keep migration OFF until the BPF program is loaded ---
set_ivh_sysctl ivh_universal_eligible 0

set_ivh_sysctl ivh_capacity_threshold 1010
set_ivh_sysctl ivh_time_left_threshold_ns 4000000
set_ivh_sysctl ivh_max_concurrent 8
set_ivh_sysctl ivh_time_left_source 1
set_ivh_sysctl ivh_selection_trylock 1
set_ivh_sysctl ivh_migrate_mechanism 0
set_ivh_sysctl ivh_steal_source 2        # TSC-only estimator, no real steal page needed
set_ivh_sysctl ivh_cap_source 3          # in-kernel ivh_uc_capacity
set_ivh_sysctl ivh_uc_enabled 1
set_ivh_sysctl ivh_uc_used_source 0
set_ivh_sysctl ivh_uc_min_steal_ns 500000
set_ivh_sysctl ivh_uc_window_ns 200000000
set_ivh_sysctl ivh_uc_duty_ns 0
set_ivh_sysctl ivh_uc_ema_alpha_q16 868
set_ivh_sysctl ivh_uc_min_avail_pct 10
set_ivh_sysctl ivh_tks_deadband_ns 50000
set_ivh_sysctl ivh_tks_idle_sub 0
set_ivh_sysctl ivh_tks_phase_pct 100
set_ivh_sysctl ivh_tks_carry_ticks 8

# --- daemons: kill any stale copies first (a second copy double-fires every
# BPF hook and puts the first copy in D-state) ---
pkill -9 -x MY_ivh_atc 2>/dev/null
pkill -9 -x vcap_probe 2>/dev/null
for i in $(seq 30); do
    pgrep -x MY_ivh_atc >/dev/null || pgrep -x vcap_probe >/dev/null || break
    sleep 0.2
done

cd "$REPO/tools/bpf" && make MY_ivh_atc > /dev/null 2>&1
setsid nohup "$REPO/tools/bpf/MY_ivh_atc" > /root/ivh_logs/atc.log 2>&1 < /dev/null &

CFG=$(cat /proc/sys/kernel/ivh_cap_source)
for i in $(seq 40); do
    bpftool map lookup name ivh_cfg key 0 0 0 0 >/dev/null 2>&1 && break
    sleep 0.25
done
bpftool map update name ivh_cfg key 0 0 0 0 value "$CFG" 0 0 0 \
  || echo "*** ERROR: ivh_cfg map update FAILED -- IVH will make no migrations ***" >&2

# REMOVED 2026-10-01 (vcap_probe deleted -- obsolete since vcap measures its own demand, and it inflated every PV-relative number): cd /root/vcapacity && setsid nohup ./vcap_probe -p 200 -s 200 \
# --- vcap: the measurement daemon (TSC-gap steal -> capacity + active time).
# Replaces vcap_probe, deleted 2026-10-01: it computed nothing and inflated
# every PV-relative number by damaging the baseline (IVH is insensitive to it,
# PV is not).  vcap needs /proc/ivh_cpu_stats (G-LOCK-51+) for tsc_khz and
# exits immediately without it, so it is guarded.
# ivh_ucw_max_age_ns MUST exceed vcap's loop period (~5.2s at -p 200 -s 5000)
# or the staleness watchdog expires capacity to 1024 between publishes and the
# arm silently becomes "IVH off".  A mismatch here is SILENT.
if [ -e /proc/ivh_cpu_stats ]; then
    [ -e /proc/sys/kernel/ivh_ucw_max_age_ns ] && echo 16000000000 > /proc/sys/kernel/ivh_ucw_max_age_ns
    cd /root/vcapacity && (pgrep -x vcap >/dev/null || (nohup ./vcap -p 200 -s 5000 >/root/ivh_logs/vcap.log 2>&1 & sleep 2))
fi
  > /root/ivh_logs/vcap_probe.log 2>&1 < /dev/null &

sleep 3
echo "atc=$(pgrep -xc MY_ivh_atc) vcap_probe=$(pgrep -xc vcap_probe) ivh_cfg=$CFG"

# --- now safe to turn migration on ---
set_ivh_sysctl ivh_universal_eligible 1

# --- kernel adaptive spinning (tier-1/tier-2): OFF/stock, unrelated axis ---
/root/spin_mode 1 > /dev/null

echo
echo "IVH migration engine: up."
echo "Userspace adaptive-spin lock defaults (env-overridable, no rebuild):"
echo "  IVH_AFL_WAKE=1  IVH_AFL_STALE_NS=50000  IVH_AFL_SPINS=256  IVH_AFL_DISABLE=0"
echo
echo "Run with, e.g.:"
echo "  NHEXTEND_DURATION=20 NHEXTEND_LOOP_SPIN=5000 /root/linux-6.17/NHextend-full -n -v -l"
