#!/bin/bash
sudo /home/nick/kernels/linux-6.17-rseqport/setup.sh

REPO=/home/nick/kernels/linux-6.17-rseqport

# IVH sysctls: confirmed-good combo (blocking lock + last_active_time formula).
# Silently skipped on kernels that don't have a given knob (e.g. Hot Threads
# sysctls don't exist before kernel 51+).
set_ivh_sysctl() {
    local path="/proc/sys/kernel/$1"
    [ -f "$path" ] && echo "$2" | sudo tee "$path" > /dev/null
}

# THE CRITICAL ONE: ivh_universal_eligible defaults to 0 and is the SOLE gate
# for IVH migration since commit 298be1454. Without this line, every sysctl
# below is cosmetic — IVH makes zero migrations, silently. Found 2026-08-08
# after this script (and ivh_mode.sh) had been shipping it unset; confirmed
# live: 14.4s -> 12.3s, 0 -> ~70,000 migrations, flipping only this one knob.
set_ivh_sysctl ivh_universal_eligible 1

set_ivh_sysctl ivh_capacity_threshold 1010
set_ivh_sysctl ivh_time_left_threshold_ns 4000000
set_ivh_sysctl ivh_max_concurrent 8
set_ivh_sysctl ivh_time_left_source 1        # last_active_time formula
set_ivh_sysctl ivh_selection_trylock 1       # non-blocking scan lock. Found 2026-08-09:
                                              # under an UNPINNED corunner (no clean/contended
                                              # split, capacity signal uniform across all 16
                                              # vCPUs), IVH with a blocking lock actively
                                              # regressed vs no-IVH (26.1s -> 31.3s, 4 runs
                                              # each) — full scan cost paid on every attempt
                                              # even though no destination is meaningfully
                                              # better. trylock=1, same scenario: 25.7s,
                                              # back in line with no-IVH. This combination
                                              # (trylock=1 + the TSC-only pipeline below) was
                                              # validated together for the first time this
                                              # session; earlier PINNED validation used
                                              # trylock=0 and never tested trylock=1 against
                                              # this specific pipeline until now.
set_ivh_sysctl ivh_migrate_mechanism 0
set_ivh_sysctl ivh_hot_threads_enabled 0     # off by default; flip on to A/B the gate

# --- TSC-only configuration, 2026-08-08: no UNHALTED.REF (unavailable in
# CVM), no real steal page (paravirt_steal_clock/kvm_steal_time) anywhere in
# the capacity or steal-time pipeline. Validated live (91+ real hackbench
# runs across two sessions, tools/bpf/docs/ivh_final_tsc_only_build_2026-08-08.md
# and ivh_wall_path_validation_2026-08-08.md) to reach ~11.5s back-to-back,
# beating the old real-steal-page reference (~11.9s), PV-free at the code
# level (traced: raw TSC, tick_sched->idle_sleeptime, ivh_tks_steal_ns — the
# kcpustat/PV-tainted path is never read at ivh_uc_used_source=0).
#
# ivh_ref_steal_enabled is deliberately NOT set here — verified live that
# ivh_pv_preempt_src=2 does not require it (the old comment about ordering
# was for the now-unused REF_TSC/vcap-calculation path). mechanism must still
# be nonzero before pure_ipi=1 is accepted, so that ordering is kept.
set_ivh_sysctl ivh_pv_wait_mechanism 2       # real halt, woken by IPI (not hypercall)
set_ivh_sysctl ivh_pv_kick_pure_ipi 1        # 100% IPI wake, no KVM_HC_KICK_CPU
set_ivh_sysctl ivh_pv_preempt_src 2          # TSC heartbeat authoritative for adaptive spinning
set_ivh_sysctl ivh_steal_source 2            # TSC tick-gap estimator, no REF_TSC, no PV steal page
set_ivh_sysctl ivh_cap_source 3              # in-kernel ivh_uc_capacity, not vcap
set_ivh_sysctl ivh_uc_enabled 1              # master gate for the ivh_uc_capacity pipeline
set_ivh_sysctl ivh_uc_used_source 0          # WALL variant — the PV-free one (ACCT reads
                                             # kcpustat, which has PV steal already
                                             # subtracted out here; see sec 8 of the
                                             # wall-path-validation report)

# THE SECOND CRITICAL ONE, found 2026-08-08 (script-reproduction audit).
# ivh_uc_min_steal_ns defaults to 10000 in the kernel (bpf_sched.c:420) but the
# validated 11.5s state runs it at 500000. This script used to leave it unset,
# so every fresh boot silently ran the WRONG value. It is not a small effect:
# it is the guard that lets a window with negligible estimated steal publish a
# clean 1024, which is what pins the *destination* (clean) population at 1023.
# At 10000 no window qualifies, the clean vCPUs publish their own phantom-
# contaminated ratio, and the contended/clean separation bleeds away run over
# run until it INVERTS. Measured live, back-to-back, everything else identical:
#
#   min_steal_ns=10000 : 11.96 12.52 14.30 15.70 16.14 s   sep 65->42->32->22->3->-28
#   min_steal_ns=500000: 11.39 11.11 11.07 11.31 11.71 11.52 11.77 11.39 s (mean 11.41)
#
# See tools/bpf/docs/ivh_script_reproduction_audit_2026-08-08.md.
set_ivh_sysctl ivh_uc_min_steal_ns 500000

# The rest of the ivh_uc / ivh_tks pipeline. These currently MATCH the kernel
# compile-time defaults, but they are pinned explicitly so this script fully
# describes the validated state and cannot be silently changed out from under
# by a default edit in bpf_sched.c / core.c.
set_ivh_sysctl ivh_uc_window_ns 200000000    # 200 ms window (matches vcap -p 200)
set_ivh_sysctl ivh_uc_duty_ns 0              # windows tile back-to-back
set_ivh_sysctl ivh_uc_ema_alpha_q16 868      # 10.4 s half-life at 200 ms window.
                                             # DO NOT raise globally — measured catastrophic
                                             # (final-tsc-only report sec 6.2)
set_ivh_sysctl ivh_uc_min_avail_pct 10       # publish threshold for a window
set_ivh_sysctl ivh_tks_deadband_ns 50000     # 50 us — measured tick-delivery jitter. Kept at 50k:
                                             # 2026-08-09 sweep at idle_sub=0/phase_pct=100 gave
                                             # hackbench ratio 1.161 (50us) / 1.146 (250us) /
                                             # 0.643 (1ms) — raising it kills real events before
                                             # it kills the residual overshoot. Not a lever.
set_ivh_sysctl ivh_tks_idle_sub 0            # 2026-08-09: STOP subtracting the vCPU's own idle
                                             # from the tick gap. ONLY valid with nohz=off (which
                                             # /etc/default/grub sets); under a tickless boot this
                                             # would reinstate the unbounded-gap phantom. Kernel
                                             # default stays 1 for exactly that reason.
set_ivh_sysctl ivh_tks_phase_pct 100         # 2026-08-09: 100, not 0 and not 50. The tick is an
                                             # ABSOLUTE periodic hrtimer, so a preemption burst
                                             # costs one WHOLE skipped tick, not the half a tick
                                             # the in-tree comment derives. Measured deficit is
                                             # 1.000 tick per ivh_tks_events, invariant over a
                                             # 5.6x range of true steal.
                                             #
                                             # idle_sub=0 + phase_pct=100 together, contended-vCPU
                                             # ratio of rq->ivh_tks_steal_ns to the kvm_steal_time
                                             # page, vs the old idle_sub=1 + phase_pct=0:
                                             #   continuously-runnable  0.655 -> 0.9999 (4 blocks)
                                             #   idle guest             0.559 -> 1.006  (7 blocks)
                                             #   hackbench              0.232 -> 1.143  (6 obs)
                                             # Clean/uncontended vCPUs read 0.0 ms at both settings
                                             # and that is CORRECT, not a miss worth fixing: their
                                             # real steal arrives in 8-14 us quanta (5-8 ms over
                                             # 60 s in ~600 steal-bearing ticks), 100x below the
                                             # 50 us deadband — structurally invisible to any
                                             # tick-gap estimator, and 0.01% of wall.
                                             #
                                             # Validated for no throughput regression, 2026-08-09:
                                             #   hackbench -T -g 1 -f 8 -l 400000, 2 batches,
                                             #     12 interleaved rounds: OFF 15.855s, old 11.370s,
                                             #     new 11.383s; paired diff +0.013s, SE 0.037,
                                             #     p~0.73. Migrations 69379 -> 69448.
                                             #   NHextend loop_spin=400000, 3 batches, 11 clean
                                             #     rounds, vs same-round IVH-off: shipped sysctls
                                             #     -3.06% -> -1.96%, NHextend4-tuned sysctls
                                             #     +7.72% -> +8.00%. Paired +1.15pp / +0.27pp,
                                             #     both n.s., neither negative.
                                             # See tools/bpf/docs/ivh_phase_pct_recalibration_2026-08-09.md
set_ivh_sysctl ivh_tks_carry_ticks 8         # sweep is flat; 64/512/4096 never improve the ratio
set_ivh_sysctl ivh_ka_enabled 0              # idle-vCPU keepalive stays off; vcap_probe below
                                             # does that job now

# BPF gate constants (tools/bpf/MY_ivh_atc.bpf.c) must be built as
# IVH_CAP_HARDFLOOR=850, IVH_CAP_TOPBAND=50, IVH_CAP_MARGIN=20 for
# ivh_capacity_threshold=1010 above to land in the validated regime. These are
# compile-time #defines, not sysctls, so they are asserted here rather than
# set — MARGIN and ivh_capacity_threshold must move together, neither works
# alone (wall-path-validation report sec 6). To change them, use the helper
# scratchpad setbpf.sh <HARDFLOOR> <TOPBAND> <MARGIN>, which edits + rebuilds
# + reloads in one step.
#
# HARDFLOOR lowered 880 -> 850 on 2026-08-19: under the -s 200 vcap_probe fix
# below, cpu8-15's own ivh_uc_capacity was still observed dipping to 853-876
# during hackbench, which the 880 floor rejected outright (REJ_CAPACITY_LOW
# 47-64.5% of scans that batch, and the run went slow, 30-50%+ of runs).
# 850 cleared that band with no new problems: two 6-run batches, 12/12 fast
# (15-18s, mean 16.0s, sd 1.04s), cpu8-15 readings never sampled below 913
# during that test, and 0 of ~2040 sampled last_migration destinations landed
# on cpu0-7 (cpu0-7 capacity stayed 328-597, nowhere near either floor value).
# Not yet proven as a root-cause fix — why cpu8-15's own capacity dips at all
# is still unexplained — this only widens the gate's tolerance for it.
BPFSRC=$REPO/tools/bpf/MY_ivh_atc.bpf.c
check_define() {
    local got
    got=$(grep -oP "^#define $1\s+\K[0-9]+" "$BPFSRC")
    if [ "$got" != "$2" ]; then
        echo "*** WARNING: $1 is $got, validated state needs $2 ***" >&2
        echo "*** the ~11.5s result will NOT reproduce with this build ***" >&2
    fi
}
check_define IVH_CAP_HARDFLOOR   850
check_define IVH_CAP_TOPBAND     50
check_define IVH_CAP_MARGIN      20
check_define IVH_CAP_MARGIN_REL  0    # relative-margin gate stays compiled OUT
                                      # (live-tested, partial fix, not shipped)

# A second MY_ivh_atc means every BPF hook fires twice and the first one goes
# D-state. Always kill before launching. Same for vcap_probe — two probes at
# -p 200 double the idle-vCPU probe load and change what ivh_uc_tick() sees.
sudo pkill -9 -x MY_ivh_atc 2>/dev/null
sudo pkill -9 -x vcap_probe 2>/dev/null
for i in $(seq 30); do
    pgrep -x MY_ivh_atc >/dev/null || pgrep -x vcap_probe >/dev/null || break
    sleep 0.2
done

cd $REPO/tools/bpf && make MY_ivh_atc > /dev/null 2>&1
mkdir -p /home/nick/ivh_logs
sudo setsid nohup $REPO/tools/bpf/MY_ivh_atc > /home/nick/ivh_logs/atc.log 2>&1 < /dev/null &

# ivh_cfg tells the BPF program which capacity source to read; it must agree
# with ivh_cap_source or the program silently reads rq->cpu_capacity (a flat
# 1024) instead of rq->ivh_uc_capacity, and no migration is ever justified.
# Poll for the map rather than a fixed sleep — the old `sleep 2` raced with
# program load and the failed update was silent.
CFG=$(cat /proc/sys/kernel/ivh_cap_source)
for i in $(seq 40); do
    sudo bpftool map lookup name ivh_cfg key 0 0 0 0 >/dev/null 2>&1 && break
    sleep 0.25
done
sudo bpftool map update name ivh_cfg key 0 0 0 0 value $CFG 0 0 0 \
  || echo "*** ERROR: ivh_cfg map update FAILED — IVH will make no migrations ***" >&2

# vcap_probe replaces vcap: keeps idle vCPUs measurable, but publishes
# NOTHING of its own (verified via `strings` — zero references to any vsched
# procfs node). All capacity/steal calculation now happens in-kernel
# (ivh_uc_capacity above), fed by this probe's job of keeping ivh_uc_tick()
# from going stale on idle vCPUs.
#
# -s 5000 -> 200 on 2026-08-19: the shipped -s 5000 (5000ms sleep between
# probes) was 26x longer than ivh_uc_window_ns's 200ms window, so ~96% of
# window-close checks saw zero probe overlap on this vCPU and the window
# just extended instead of publishing — this is what let ivh_uc_capacity
# collapse toward the HARDFLOOR under load in the first place. -s 200
# matches the probe cadence to the window period (same as -p 200). Live
# result: hackbench went from a persistent ~30-50%+ slow-run rate to 0
# slow runs across two 6-run batches post-fix.
#
# The original, fully-calculating vcap is preserved at
# /home/nick/vsched_main/vcapacity_ORIGINAL_BACKUP_2026-08-08/ as an
# emergency revert path — swap this line for that binary's `vcap -p 200 -s
# 5000` and revert ivh_cap_source to 0 / ivh_steal_source to 0 or 1 to go
# fully back to the old, real-steal-page-and-UNHALTED.REF-capable build.
cd /home/nick/vsched_main/vcapacity && sudo setsid nohup ./vcap_probe -p 200 -s 200 \
  > /home/nick/ivh_logs/vcap_probe.log 2>&1 < /dev/null &

# DO NOT BENCHMARK IMMEDIATELY. The published ivh_uc_capacity is an EMA and its
# recovery time constant is LONG (~130 s half-life at idle publish cadence,
# wall-path-validation sec 6). Until the *destination* (uncontended) vCPUs have
# converged to ~1023 the result is 1-4 s slow, with no config difference at all
# — measured 2026-08-08, same config, only the starting convergence differing:
#   clean cap 1023 -> 11.41s | 1021 -> 11.60s | ~1002 -> 12.38s | ~970 -> 15.37s
# Recovery from a fully depressed state took ~7 minutes of GENUINE idle. A
# busy-wait shell loop is itself a load and will hold it down indefinitely —
# use sleep, and keep builds/editors off the guest while waiting.
sleep 3
echo
echo "IVH up: atc=$(pgrep -xc MY_ivh_atc) vcap_probe=$(pgrep -xc vcap_probe) ivh_cfg=$CFG"
echo
echo "Capacity EMA has NOT settled yet. Before benchmarking, idle (sleep, not a"
echo "busy loop) until the uncontended vCPUs read ~1023 -- check with:"
echo "  grep '^ivh_uc_cpu:' /proc/ivh_debug | awk '{n=\$2+0; if(n<8){a+=\$5;c++}else{b+=\$5;d++}} END{printf \"cap_cont=%.0f cap_clean=%.0f\\n\", a/c, b/d}'"
