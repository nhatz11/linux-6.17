#!/bin/bash
# goto_mode.sh <pv|ivh|ivh-as> <user|kernel>
#
# Jump straight into one of the three canonical comparison states this
# project uses over and over, for either workload family:
#
#   pv       migration OFF, kernel adaptive spinning OFF (spin_mode 1) --
#            the stock baseline for both workload families.
#   ivh      migration ON, kernel adaptive spinning OFF (spin_mode 1) --
#            isolates the migration engine's own effect.
#   ivh-as   migration ON, PLUS "adaptive spinning" for whichever workload
#            you're testing:
#              target=user   -> run /root/linux-6.17/NHextend-full (the
#                                futex+TSC-heartbeat adaptive lock) instead
#                                of NHextend3. Kernel spin_mode stays at 1
#                                (STOCK_PV) -- this is a deliberately
#                                separate axis from the userspace lock, held
#                                fixed throughout this project's NHextend
#                                testing so the two mechanisms are never
#                                accidentally combined.
#              target=kernel -> spin_mode 2 (IVH_PV): the KERNEL's own
#                                tier-1/tier-2 adaptive spinning, since a
#                                kernel workload like hackbench never calls
#                                into the userspace lock at all.
#
# Infra (sysctls + MY_ivh_atc + vcap_probe + ivh_cfg) is brought up
# idempotently -- daemons are only (re)started if not already running, so
# repeated calls don't pay the capacity-EMA reconvergence cost
# (tools/bpf/docs/ivh_rebuild_plan.md sec 4, Step 8: destination vCPUs need
# a real idle period to reach ~1023; restarting vcap_probe/MY_ivh_atc for no
# reason throws that away).
set -u

REPO=/root/kernels/linux-6.17-vanilla
MODE="${1:-}"
TARGET="${2:-}"

usage() {
    echo "usage: $0 <pv|ivh|ivh-as> <user|kernel>" >&2
    exit 1
}
case "$MODE" in pv|ivh|ivh-as) ;; *) usage ;; esac
case "$TARGET" in user|kernel) ;; *) usage ;; esac

set_ivh_sysctl() {
    local path="/proc/sys/kernel/$1"
    [ -f "$path" ] && echo "$2" > "$path"
}

# --- infra: sysctls (cheap, idempotent, always applied) ---
set_ivh_sysctl ivh_capacity_threshold 1010
set_ivh_sysctl ivh_time_left_threshold_ns 4000000
set_ivh_sysctl ivh_max_concurrent 8
set_ivh_sysctl ivh_time_left_source 1
set_ivh_sysctl ivh_selection_trylock 1
set_ivh_sysctl ivh_migrate_mechanism 0
set_ivh_sysctl ivh_steal_source 2
set_ivh_sysctl ivh_cap_source 3
set_ivh_sysctl ivh_uc_enabled 1
set_ivh_sysctl ivh_uc_used_source 0
set_ivh_sysctl ivh_uc_min_steal_ns 500000
set_ivh_sysctl ivh_uc_window_ns 200000000
set_ivh_sysctl ivh_uc_duty_ns 0
set_ivh_sysctl ivh_uc_ema_alpha_q16 868
set_ivh_sysctl ivh_uc_min_avail_pct 10
set_ivh_sysctl ivh_tks_deadband_ns 1000
set_ivh_sysctl ivh_tks_idle_sub 0
# G-LOCK-39: the hrtimer sampler drives the estimator, so the one-period
# phase bonus must be OFF -- it corrects for undersampling and inflates a
# properly-sampled signal. ORDER MATTERS: the kernel refuses a non-zero
# ivh_tks_sampler_ns while ivh_tks_phase_pct is non-zero, so phase_pct=0 has
# to be written first. Validated against HOST schedstat ground truth
# (2026-09-23): see tools/bpf/docs/ivh_steal_host_validation_2026-09-23.md.
set_ivh_sysctl ivh_tks_phase_pct 0
set_ivh_sysctl ivh_tks_carry_ticks 8
set_ivh_sysctl ivh_tks_duty_pct 100
set_ivh_sysctl ivh_tks_sampler_ns 200000

# --- infra: daemons, idempotent -- only touched if not already correctly up ---
NEED_DAEMONS=0
pgrep -x MY_ivh_atc > /dev/null || NEED_DAEMONS=1
pgrep -x vcap_probe > /dev/null || NEED_DAEMONS=1

if [ "$NEED_DAEMONS" = "1" ]; then
    echo "daemons not both up -- (re)starting (this resets capacity-EMA convergence, ~130s half-life)"
    pkill -9 -x MY_ivh_atc 2>/dev/null
    pkill -9 -x vcap_probe 2>/dev/null
    for i in $(seq 30); do
        pgrep -x MY_ivh_atc >/dev/null || pgrep -x vcap_probe >/dev/null || break
        sleep 0.2
    done

    set_ivh_sysctl ivh_universal_eligible 0   # keep off until BPF program loaded
    mkdir -p /root/ivh_logs
    # 2026-09-22: this `make` used to be unbounded and is where the script hung
    # on a fresh VM -- the sysctls above had already been applied, but the mode
    # switch at the bottom never ran, and killing the script left the box
    # half-configured. Now: skip the build when the binary is already present,
    # and bound it when it is not. A build failure is NOT fatal.
    if [ ! -x "$REPO/tools/bpf/MY_ivh_atc" ]; then
        echo "MY_ivh_atc not built -- building (bounded, 300s)"
        ( cd "$REPO/tools/bpf" && timeout -k 10 300 make MY_ivh_atc ) > /dev/null 2>&1 \
          || echo "*** WARNING: MY_ivh_atc build failed/timed out -- migration will not run ***" >&2
    fi
    if [ -x "$REPO/tools/bpf/MY_ivh_atc" ]; then
        setsid nohup "$REPO/tools/bpf/MY_ivh_atc" > /root/ivh_logs/atc.log 2>&1 < /dev/null &
    fi

    CFG=$(cat /proc/sys/kernel/ivh_cap_source)
    if pgrep -x MY_ivh_atc >/dev/null; then
        for i in $(seq 40); do
            bpftool map lookup name ivh_cfg key 0 0 0 0 >/dev/null 2>&1 && break
            sleep 0.25
        done
        bpftool map update name ivh_cfg key 0 0 0 0 value "$CFG" 0 0 0 \
          || echo "*** ERROR: ivh_cfg map update FAILED -- IVH will make no migrations ***" >&2
    else
        echo "*** WARNING: MY_ivh_atc not running -- skipping ivh_cfg update ***" >&2
    fi

    cd /root/vcapacity && setsid nohup ./vcap_probe -p 200 -s 200 \
      > /root/ivh_logs/vcap_probe.log 2>&1 < /dev/null &
    sleep 3
else
    echo "daemons already up, left untouched (capacity EMA convergence preserved)"
fi

# --- the actual mode switch ---
case "$MODE" in
    pv)     set_ivh_sysctl ivh_universal_eligible 0 ;;
    ivh)    set_ivh_sysctl ivh_universal_eligible 1 ;;
    ivh-as) set_ivh_sysctl ivh_universal_eligible 1 ;;
esac

case "$TARGET" in
    kernel)
        case "$MODE" in
            pv|ivh) /root/spin_mode 1 > /dev/null ;;   # STOCK_PV
            ivh-as) /root/spin_mode 2 > /dev/null ;;   # IVH_PV -- kernel tier-1/tier-2
        esac
        ;;
    user)
        /root/spin_mode 1 > /dev/null   # always stock -- separate axis, held fixed
        ;;
esac

echo
echo "=== now: mode=$MODE target=$TARGET ==="
echo "ivh_universal_eligible=$(cat /proc/sys/kernel/ivh_universal_eligible)"
echo "ivh_adaptive_mode=$(cat /proc/sys/kernel/ivh_adaptive_mode 2>/dev/null)  ivh_pv_preempt_src=$(cat /proc/sys/kernel/ivh_pv_preempt_src 2>/dev/null)"
echo "atc=$(pgrep -xc MY_ivh_atc)  vcap_probe=$(pgrep -xc vcap_probe)"
echo
if [ "$TARGET" = "kernel" ]; then
    echo "Run with, e.g.:"
    echo "  hackbench -T -g1 -f8 -l400000"
else
    case "$MODE" in
        ivh-as) BIN=NHextend-full ;;
        *)      BIN=NHextend3 ;;
    esac
    echo "Run with, e.g.:"
    echo "  NHEXTEND_DURATION=20 NHEXTEND_LOOP_SPIN=600000 /root/linux-6.17/$BIN -n -v -l"
fi

# --- 2026-09-22: calibration guard -------------------------------------------
# The capacity estimator is worthless uncalibrated: at the boot defaults
# (ivh_tks_idle_sub=1, ivh_tks_phase_pct=0) it reads 0.655 of true steal on
# runnable load and 0.559 on an idle guest, so ivh_uc_capacity sits near a flat
# 1024 and nothing degrades. With the calibrated pair it reads 0.9999 / 1.006.
# See tools/bpf/docs/ivh_phase_pct_recalibration_2026-08-09.md.
#
# 2026-09-22: ivh_tks_deadband_ns lowered 50000 -> 1000. Validated against a
# SCHED_FIFO wall-clock prober (ivh_tools/vcpu_gone.c) on two vCPUs with
# different host preemption quanta. deadband, not phase_pct, is what makes
# sub-tick steal visible at all, and the response cliff sits between 1000 and
# 2000 ns -- every previous sweep floored at 10000 and so sat on the flat side
# of it. Ratios kernel/truth, median of 5:
#       deadband=50000 (old):  coarse 0.82   fine 0.00
#       deadband=1000  (new):  coarse 1.035  fine 0.995
# Capacity differential is preserved: contended mean 488 vs uncontended 879
# (was 529 vs 1012). cpu15 dropping 1012->879 is the estimator finally booking
# its real ~10.7% host steal, not a regression.
# DO NOT READ THESE AS AN ACCURACY CLAIM. Trace replay (ivh_tools/vcpu_trace +
# replay_tks.py, no kernel change needed) showed the tick-gap estimator ALIASES:
# host preemptions arrive at 457/s on cpu3 and 956/s on cpu15 against a 1000/s
# tick, so the sampler is at or below Nyquist for the process it samples.
# Replaying one fixed 10s timeline while varying only the tick PHASE moves the
# reported/true ratio by 2.16x on cpu3 and 10.67x on cpu15 (0.34 - 3.67). The
# 0.995 measured on cpu15 was a phase coincidence inside that range, not
# accuracy, and no (phase_pct, deadband) pair can fix an aliased signal.
# deadband=1000 is kept over 50000 only because 50000 sat above cpu15's whole
# excess distribution and read a constant zero; 1000 at least responds. The
# contended/idle differential (488 vs 879) was verified separately and is what
# capacity is actually consumed for -- ivh_uc_capacity is read only by the
# migration gate (fair.c:13847,13875), never by the lock path.
# These were already set at the top; this block RE-ASSERTS and VERIFIES them so
# that a failure in the daemon bring-up above can never leave the box silently
# uncalibrated again.
echo
echo "=== calibration check ==="
CAL_FAIL=0
check_cal() {   # $1 knob  $2 expected
    local cur
    cur=$(cat "/proc/sys/kernel/$1" 2>/dev/null)
    if [ "$cur" != "$2" ]; then
        echo "$2" > "/proc/sys/kernel/$1" 2>/dev/null
        cur=$(cat "/proc/sys/kernel/$1" 2>/dev/null)
    fi
    if [ "$cur" = "$2" ]; then
        printf "  ok   %-26s %s\n" "$1" "$cur"
    else
        printf "  FAIL %-26s got=%s want=%s\n" "$1" "${cur:-MISSING}" "$2"
        CAL_FAIL=1
    fi
}
check_cal ivh_tks_idle_sub      0
check_cal ivh_tks_phase_pct     0
check_cal ivh_tks_duty_pct      100
check_cal ivh_tks_sampler_ns    200000
check_cal ivh_tks_deadband_ns   1000
check_cal ivh_tks_carry_ticks   8
check_cal ivh_steal_source      2
check_cal ivh_cap_source        3
check_cal ivh_uc_enabled        1
check_cal ivh_uc_used_source    0
check_cal ivh_uc_min_steal_ns   500000
check_cal ivh_capacity_threshold 1010
if [ "$CAL_FAIL" = "1" ]; then
    echo "*** CALIBRATION INCOMPLETE -- capacity numbers are NOT trustworthy ***" >&2
    exit 1
fi
echo "calibration OK (capacity estimator armed)"
