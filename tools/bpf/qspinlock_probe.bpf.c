// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
// fentry probe on queued_spin_lock_slowpath to confirm wait_depth bookkeeping.
// No kernel rebuild required — pure BPF loaded at runtime via bpftool.
#include "vmlinux.h"
#include "bpf_helpers.h"

char LICENSE[] SEC("license") = "Dual BSD/GPL";

/* Per-PID entry count: entries - exits = tasks currently spinning.
 * Map is hash so we only track contending PIDs, not the whole system. */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, u32);
    __type(value, s64);
} spin_depth SEC(".maps");

SEC("fentry/queued_spin_lock_slowpath")
int BPF_PROG(slowpath_enter, struct qspinlock *lock, u32 val)
{
    u32 pid = bpf_get_current_pid_tgid() & 0xFFFFFFFF;
    s64 one = 1, *v;

    v = bpf_map_lookup_elem(&spin_depth, &pid);
    if (v)
        __sync_fetch_and_add(v, 1);
    else
        bpf_map_update_elem(&spin_depth, &pid, &one, BPF_ANY);

    char fmt[] = "QSPIN_ENTER pid=%d\n";
    bpf_trace_printk(fmt, sizeof(fmt), pid);
    return 0;
}

SEC("fexit/queued_spin_lock_slowpath")
int BPF_PROG(slowpath_exit, struct qspinlock *lock, u32 val)
{
    u32 pid = bpf_get_current_pid_tgid() & 0xFFFFFFFF;
    s64 neg = -1, *v;

    v = bpf_map_lookup_elem(&spin_depth, &pid);
    if (v)
        __sync_fetch_and_add(v, -1);
    else
        bpf_map_update_elem(&spin_depth, &pid, &neg, BPF_ANY);

    char fmt[] = "QSPIN_EXIT  pid=%d\n";
    bpf_trace_printk(fmt, sizeof(fmt), pid);
    return 0;
}
