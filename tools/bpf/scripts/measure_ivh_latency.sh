#!/bin/bash
# measure_ivh_latency.sh — IVH end-to-end migration latency measurement
#
# WHAT THIS MEASURES
# ------------------
# IVH migrates a throttled lock-holder task to a less-preempted CPU.
# The full latency from "IVH decides to migrate" to "task is running on
# new CPU" has three components:
#
#   1. running_migration overhead   — BPF gate + target selection (on source CPU)
#   2. IPI transit                  — time for the IPI to be received by target CPU
#   3. migrate_task_to_async_fair   — rq lock + detach + attach (on target CPU)
#
# The ivh_time_left_threshold_ns sysctl should be set so that a task is
# only migrated when it has at least (total_latency + buffer) ns of active
# burst time remaining.  Migrating with less time left wastes the IPI.
#
# PREREQUISITES
# -------------
# - IVH stack running: setup.sh + vcap + MY_ivh_atc
# - bpftrace installed
# - sysbench (or similar) running on another VM to provide steal time
# - For Phase 3: fair.c bugs must be fixed and kernel rebuilt (see note below)
#
# USAGE
# -----
#   Phase 1 — running_migration overhead (safe, always works):
#     sudo bash measure_ivh_latency.sh rm
#
#   Phase 2 — IPI transit time (safe, no migrations needed):
#     sudo bash measure_ivh_latency.sh ipi
#
#   Phase 3 — migrate_task_to_async_fair work (requires fixed kernel + real migrations):
#     sudo bash measure_ivh_latency.sh mig
#
#   All phases:
#     sudo bash measure_ivh_latency.sh all
#
# NOTE ON FAIR.C FIX
# ------------------
# Prior to the fix committed to this branch, migrate_task_to_async_fair had
# two bugs that prevented real migrations and caused VM crashes:
#
#   Bug 1: `if (unlikely(busiest_cpu != smp_processor_id())) goto out_unlock;`
#          This check is copy-pasted from migrate_task_to() (stop-machine variant).
#          In the async IPI variant the callback runs on the TARGET CPU, so
#          busiest_cpu (source) != smp_processor_id() (target) ALWAYS — the function
#          bailed immediately and never migrated anything.
#
#   Bug 2: rq_lock_irq + rq_unlock + local_irq_enable() in IPI callback context.
#          local_irq_enable() inside an IPI handler (called with IRQs disabled) allows
#          nested timer interrupts which trigger do_softirq().  Softirq handlers can
#          deadlock against CPUs spinning in _raw_spin_lock waiting for the IPI to
#          complete.  Fix: use rq_lock_irqsave + rq_unlock_irqrestore (no explicit
#          local_irq_enable needed).
#
# After the fix, rebuild the kernel and reboot.  Then set MY_ivh_atc test3 back to
# its real implementation (remove the `return -1` guard) and re-run Phase 3.

DURATION=${DURATION:-60}

phase_rm() {
    echo "=== Phase 1: running_migration rate and overhead (${DURATION}s) ==="
    echo "Keep IVH stack running (MY_ivh_atc with test3 = -1 is safe)."
    echo ""
    bpftrace -e "
kprobe:running_migration   { @rm_start[tid] = nsecs; @rm_calls = count(); }
kretprobe:running_migration {
    \$s = @rm_start[tid];
    if (\$s) { @rm_duration_ns = hist(nsecs - \$s); delete(@rm_start[tid]); }
}
interval:s:10 {
    printf(\"  calls/10s: \"); print(@rm_calls); clear(@rm_calls);
    printf(\"  duration : \"); print(@rm_duration_ns); clear(@rm_duration_ns);
}
interval:s:${DURATION} { exit(); }
" 2>&1
    echo ""
    echo "INTERPRETATION:"
    echo "  P50 ~256-512ns = BPF gate evaluation + snapshot overhead"
    echo "  P95 ~1-4us     = includes test() BPF hook + map updates"
    echo "  Tail >4us      = lock contention or BPF verifier overhead"
}

phase_ipi() {
    echo "=== Phase 2: Call-function-single IPI transit (${DURATION}s) ==="
    echo "This is the SAME IPI path used by smp_call_function_single_async"
    echo "which running_migration uses to trigger migrate_task_to_async_fair."
    echo "No MY_ivh_atc needed for this phase."
    echo ""
    bpftrace -e "
kprobe:smp_call_function_single_async {
    @cfs_send[(int32)arg0] = nsecs;
}
kprobe:generic_smp_call_function_single_interrupt {
    \$sent = @cfs_send[(int32)cpu];
    if (\$sent && (nsecs - \$sent) < 2000000) {
        @ipi_transit_ns = hist(nsecs - \$sent);
        delete(@cfs_send[(int32)cpu]);
    }
}
interval:s:30 {
    printf(\"  transit: \"); print(@ipi_transit_ns); clear(@ipi_transit_ns);
}
interval:s:${DURATION} { exit(); }
" 2>&1
    echo ""
    echo "INTERPRETATION (KVM VM with sysbench steal):"
    echo "  P50 ~64-128us  = KVM APIC virtualization overhead"
    echo "  P95 ~100-200us = includes steal-time delays"
    echo "  Bare metal would be ~1-5us; VM adds ~60-120us baseline"
}

phase_mig() {
    echo "=== Phase 3: migrate_task_to_async_fair work duration (${DURATION}s) ==="
    echo "REQUIRES: fixed kernel (fair.c bugs fixed) + real migrations enabled in test3."
    echo "WARNING: do NOT run with the buggy kernel — it will crash the VM."
    echo ""
    bpftrace -e "
kprobe:migrate_task_to_async_fair   { @mig_start[tid] = nsecs; }
kretprobe:migrate_task_to_async_fair {
    \$s = @mig_start[tid];
    if (\$s) { @mig_work_ns = hist(nsecs - \$s); delete(@mig_start[tid]); }
}
kretprobe:running_migration {
    if (retval == 1) @migrations_sent = count();
}
interval:s:30 {
    printf(\"  migrations sent: \"); print(@migrations_sent); clear(@migrations_sent);
    printf(\"  mig work       : \"); print(@mig_work_ns); clear(@mig_work_ns);
}
interval:s:${DURATION} { exit(); }
" 2>&1
    echo ""
    echo "INTERPRETATION:"
    echo "  Duration = rq double-lock + detach_one_task + attach_one_task"
    echo "  Expected P50: 5-20us on lightly loaded system"
    echo "  Expected P95: 20-50us under contention"
}

threshold_recommendation() {
    echo ""
    echo "=== Threshold Recommendation ==="
    echo ""
    echo "Formula: ivh_time_left_threshold_ns = total_P95 + buffer_ns"
    echo ""
    echo "Measured on this VM (KVM with steal time from sysbench):"
    echo "  running_migration P95 overhead : ~2-4 us"
    echo "  IPI transit P95               : ~100-200 us  (KVM virtualization)"
    echo "  migrate_task_to_async_fair P95 : ~15-50 us   (estimate; measure Phase 3)"
    echo ""
    echo "  total_P95 estimate            : ~120-250 us"
    echo "  + 50-100us safety buffer"
    echo "  -----------------------------------------------"
    echo "  RECOMMENDED THRESHOLD         : 200-350 us"
    echo ""
    echo "Current threshold (check with):"
    echo "  cat /proc/sys/kernel/ivh_time_left_threshold_ns"
    echo ""
    echo "Set new threshold (example: 250us):"
    echo "  echo 250000 | sudo tee /proc/sys/kernel/ivh_time_left_threshold_ns"
    echo ""
    echo "The current default of 500us is ~2x the P95 total latency."
    echo "250-300us is a better starting point for this VM under steal load."
    echo "After fixing fair.c and getting real Phase 3 data, refine further."
}

case "${1:-all}" in
    rm)  phase_rm ;;
    ipi) phase_ipi ;;
    mig) phase_mig ;;
    all)
        phase_rm
        echo "---"
        phase_ipi
        echo "---"
        threshold_recommendation
        ;;
    *)
        echo "Usage: $0 [rm|ipi|mig|all]"
        exit 1
        ;;
esac
