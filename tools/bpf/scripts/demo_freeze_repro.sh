#!/usr/bin/env bash
# demo_freeze_repro.sh -- live demo of the post-lock-migration VM freeze,
# using only /proc files (no rebuild, no ftrace).
#
# HYPOTHESIS:
#   bpf_sched_post_lock_migrate() only checks that its destination CPU is
#   IDLE (nr_running > 0) before migrating a confirmed lock-holder there.
#   It never checks whether the destination is SAFE -- i.e. whether the
#   resident thread is mid-spin on a second lock with preemption disabled
#   (this VM has no paravirt spinlocks, so contended acquisition is a bare
#   test-and-set loop, preempt_disable() held, no yielding, no backoff).
#   If the migrated holder lands there and the resident spinner wants the
#   very lock that just arrived, neither side can ever back off:
#     - the spinner can't be preempted (preempt_disable() is priority-blind)
#     - the spinner can't voluntarily yield (preempt count is 2, above the
#       yield path's threshold of 1)
#   => permanent circular wait => total VM freeze (not a slowdown, not a
#   panic -- the kernel just stops responding).
#
#   Lowering ivh_post_lock_min_cs_ns does not make any single migration
#   more dangerous. It makes migrations fire far more often, so the same
#   low-probability unlucky landing gets attempted far more times per
#   second. This is a probabilistic race, not deterministic every run.
#
# WHAT THIS SCRIPT SHOWS, using only /proc:
#   /proc/sys/kernel/ivh_post_lock_min_cs_ns  -- the dial that controls
#                                                 migration frequency
#   /proc/ivh_debug                           -- live counters: watch
#                                                 migrations_done climb,
#                                                 then stop dead at the
#                                                 freeze
#   /proc/vcap_info                           -- confirms real asymmetric
#                                                 contention is present
#                                                 (needed for the race to
#                                                 have anything to bite on)
#
# PREREQUISITE: sysbench (or equivalent) generating steal on the neighbor
# VM's CPUs 0-7, so /proc/vcap_info shows an asymmetric capacity pattern.
# Without real contention, post-lock rarely finds anything to migrate.
#
# Usage: sudo bash demo_freeze_repro.sh

set -uo pipefail

KVER="6.17.0-rseqport38-p3.5+"
MODULE="/lib/modules/${KVER}/extra/vsched_module.ko"
VCAP_DIR="/home/nick/vsched_main/vcapacity"
ATC_BIN="/home/nick/kernels/linux-6.17-rseqport/tools/bpf/MY_ivh_atc"

echo "=================================================================="
echo " HYPOTHESIS"
echo "=================================================================="
echo "Post-lock migration checks destination is IDLE, never checks it's"
echo "SAFE. A destination running a preempt-disabled nested spinner can"
echo "never yield and can never be preempted -- landing a lock-holder"
echo "there creates a permanent circular wait. Lower min_cs_ns = more"
echo "migration attempts/sec = more rolls of the same unlucky dice, not"
echo "a more dangerous individual attempt."
echo "=================================================================="
echo

echo "--- Step 1: ensure module + daemons are up ---"
if ! lsmod | grep -q vsched_module; then
    echo "loading vsched_module.ko..."
    sudo insmod "$MODULE" || { echo "FAILED to load module"; exit 1; }
fi
if ! pgrep -fa vcap | grep -qv grep; then
    echo "starting vcap..."
    (cd "$VCAP_DIR" && sudo ./vcap > /tmp/vcap.log 2>&1 &)
    sleep 1
fi
if ! pgrep -f MY_ivh_atc > /dev/null; then
    echo "starting MY_ivh_atc..."
    sudo "$ATC_BIN" > /tmp/atc.log 2>&1 &
    sleep 2
fi
echo "module + daemons: OK"
echo

echo "--- Step 2: confirm real contention is present (/proc/vcap_info) ---"
echo "Looking for CPUs 0-7 elevated vs CPUs 8-15 near zero."
echo "If this looks flat/symmetric, start sysbench on the neighbor VM first."
sudo cat /proc/vcap_info | head -16
echo
read -rp "Contention pattern looks asymmetric? Press enter to continue, Ctrl-C to abort..."
echo

echo "--- Step 3: baseline SAFE run (default min_cs_ns=30000) ---"
sudo sysctl -w kernel.ivh_post_lock_enabled=1 > /dev/null
sudo sysctl -w kernel.ivh_post_lock_dispatch=1 > /dev/null
sudo sysctl -w kernel.ivh_spin_yield_enabled=1 > /dev/null
sudo sysctl -w kernel.ivh_post_lock_min_cs_ns=30000 > /dev/null
echo "kernel.ivh_post_lock_min_cs_ns = $(cat /proc/sys/kernel/ivh_post_lock_min_cs_ns)  (safe)"
echo "Running hackbench..."
time /home/nick/ivh_exec hackbench -T -g 1 -f 8 -l 400000
echo
echo "Counters after safe run:"
sudo cat /proc/ivh_debug | grep -E "migrations_done|timeout_count|in_schedule"
echo
echo "This is expected to complete cleanly every time -- migrations are"
echo "rare enough at this threshold that the unlucky landing basically"
echo "never gets rolled."
echo

read -rp "Press enter to arm the DANGEROUS config and reproduce the freeze..."
echo

echo "--- Step 4: DANGEROUS config (min_cs_ns=1000) ---"
sudo sysctl -w kernel.ivh_post_lock_min_cs_ns=1000 > /dev/null
echo "kernel.ivh_post_lock_min_cs_ns = $(cat /proc/sys/kernel/ivh_post_lock_min_cs_ns)  (DANGEROUS)"
echo
echo "Running hackbench, watching /proc/ivh_debug live."
echo "WATCH FOR: migrations_done climbing normally, then going silent."
echo "If the terminal itself stops responding, that IS the freeze --"
echo "the VM will need a hard reset. This is the demo."
echo
echo "(open a second terminal and run: watch -n0.2 'sudo cat /proc/ivh_debug | grep -E \"migrations_done|timeout_count|in_schedule\"')"
echo

/home/nick/ivh_exec hackbench -T -g 1 -f 8 -l 400000
echo "Completed without freezing this time -- exit code $?"
echo "This is a probabilistic race; rerun step 4 a few times if it doesn't"
echo "reproduce immediately. The two reproductions used for the paper"
echo "both hit within 1-4 back-to-back trials under real contention."
