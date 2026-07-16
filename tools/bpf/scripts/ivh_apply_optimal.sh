#!/bin/bash
# Apply the IVH "optimal settings" found 2026-07-15 (see
# tools/bpf/docs/rebuild_confirmation_2026-07-14.md and the Fable
# investigation results for the full writeup). None of these persist
# across a reboot -- run this fresh every time the VM comes back up.
#
# Best measured result with this stack (vcap paused, 3 rounds vs 2 no-opt):
#   iterations: parity (-0.6%, noise-level, i.e. no more regression)
#   host-preempted CS cycles: ~10.4-11.1% -> ~5.7-6.9% (~1.6x)
set -e

echo "=== sysctls ==="
echo 1010    | sudo tee /proc/sys/kernel/ivh_capacity_threshold
echo 4000000 | sudo tee /proc/sys/kernel/ivh_time_left_threshold_ns
echo 8       | sudo tee /proc/sys/kernel/ivh_max_concurrent
echo 0       | sudo tee /proc/sys/kernel/ivh_eval_cooldown_ns
echo 4000    | sudo tee /proc/sys/kernel/rseq_sched_extend_usec

# Toggles tested individually tonight -- all neutral or actively worse
# (ivh_migrate_mechanism=1 is effectively a no-op due to a race). Keep at 0.
echo 0 | sudo tee /proc/sys/kernel/ivh_time_left_source
echo 0 | sudo tee /proc/sys/kernel/ivh_migrate_mechanism
echo 0 | sudo tee /proc/sys/kernel/ivh_selection_trylock
echo 0 | sudo tee /proc/sys/kernel/ivh_gate4_strict

echo "=== scheduler debugfs (cache_nice_tries) ==="
echo Y | sudo tee /sys/kernel/debug/sched/verbose
sudo bash -c '
for cpu in /sys/kernel/debug/sched/domains/cpu*/domain0/cache_nice_tries; do
    echo 10000 > "$cpu"
done
echo "cache_nice_tries set on $(ls /sys/kernel/debug/sched/domains/ | wc -l) CPUs:"
for cpu in /sys/kernel/debug/sched/domains/cpu*/domain0/cache_nice_tries; do
    printf "%s: %s\n" "$cpu" "$(cat "$cpu")"
done
'

echo "=== vsched_module ==="
if ! lsmod | grep -q '^vsched_module'; then
    (cd /home/nick/kernels/linux-6.17-rseqport/custom_modules && sudo insmod vsched_module.ko)
    echo "module loaded"
else
    echo "module already loaded"
fi

echo "=== MY_ivh_atc (BPF hook) ==="
if pgrep -f "MY_ivh_atc$" > /dev/null; then
    echo "existing instance found, killing before restart:"
    pgrep -fa "MY_ivh_atc$"
    sudo pkill -TERM -f "MY_ivh_atc$"
    sleep 1
fi
cd /home/nick/kernels/linux-6.17-rseqport/tools/bpf
sudo ./MY_ivh_atc > /tmp/ivh_atc.log 2>&1 &
disown
sleep 2
if pgrep -f "MY_ivh_atc$" > /dev/null; then
    echo "MY_ivh_atc running:"
    pgrep -fa "MY_ivh_atc$"
else
    echo "FAILED to start MY_ivh_atc -- check /tmp/ivh_atc.log"
    tail -20 /tmp/ivh_atc.log
    exit 1
fi

echo "=== vcap daemon ==="
if pgrep -f "vcap -p" > /dev/null; then
    echo "existing instance found, killing before restart:"
    pgrep -fa "vcap -p"
    sudo pkill -TERM -f "vcap -p"
    sleep 1
fi
cd /home/nick/vsched_main/vcapacity
sudo ./vcap -p 200 -s 5000 > /tmp/vcap.log 2>&1 &
disown
sleep 3
if pgrep -f "vcap -p" > /dev/null; then
    echo "vcap running:"
    pgrep -fa "vcap -p"
else
    echo "FAILED to start vcap -- check /tmp/vcap.log"
    tail -20 /tmp/vcap.log
    exit 1
fi

echo "=== done ==="
echo "Verify gate config matches expectation with:"
echo "  grep '^#define GATE_' /home/nick/kernels/linux-6.17-rseqport/tools/bpf/MY_ivh_atc.bpf.c"
echo "  grep 'ULL' /home/nick/kernels/linux-6.17-rseqport/tools/bpf/MY_ivh_atc.bpf.c | grep -i preempt"
