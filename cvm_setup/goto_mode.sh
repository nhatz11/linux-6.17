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
set_ivh_sysctl ivh_tks_deadband_ns 50000
set_ivh_sysctl ivh_tks_idle_sub 0
set_ivh_sysctl ivh_tks_phase_pct 100
set_ivh_sysctl ivh_tks_carry_ticks 8

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
    cd "$REPO/tools/bpf" && make MY_ivh_atc > /dev/null 2>&1
    mkdir -p /root/ivh_logs
    setsid nohup "$REPO/tools/bpf/MY_ivh_atc" > /root/ivh_logs/atc.log 2>&1 < /dev/null &

    CFG=$(cat /proc/sys/kernel/ivh_cap_source)
    for i in $(seq 40); do
        bpftool map lookup name ivh_cfg key 0 0 0 0 >/dev/null 2>&1 && break
        sleep 0.25
    done
    bpftool map update name ivh_cfg key 0 0 0 0 value "$CFG" 0 0 0 \
      || echo "*** ERROR: ivh_cfg map update FAILED -- IVH will make no migrations ***" >&2

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
