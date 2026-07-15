// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
//
// Checkpoint H observation tool (see tools/bpf/docs/ivh_build_and_evaluation_plan_2026-07-11.md).
// Kprobes the real, committed __x64_sys_ivh_cs_enter (confirmed live in the
// running kernel via /proc/kallsyms) and reads the (lock_addr, waiters,
// verdict) arguments the userspace Hotlock-replica shim in NHextend.c now
// passes -- arguments the kernel's own SYSCALL_DEFINE0(ivh_cs_enter) does not
// declare and therefore never reads or acts on. This program only OBSERVES;
// it changes no kernel behavior, gates nothing, and cannot, since the kernel
// discards these args before this kprobe even fires (the kprobe fires on
// function entry, args already in registers, kernel body hasn't run yet, and
// even once it runs, SYSCALL_DEFINE0 never touches them regardless).
//
// __x64_sys_ivh_cs_enter(const struct pt_regs *regs) is called via the
// standard x86-64 C ABI (this is the syscall's own *entry stub*, not the raw
// hardware syscall trampoline) -- so its own sole parameter (a pointer to the
// REAL syscall-entry pt_regs, which is what actually holds the userspace
// syscall(470, lock_addr, waiters, verdict) arguments in di/si/dx) arrives in
// %rdi, i.e. ctx->di at kprobe entry. One extra pointer-chase to get from
// "the probed function's own args" to "the syscall args it was handed."
#include "../../vmlinux.h"
#include "bpf_helpers.h"

char LICENSE[] SEC("license") = "Dual BSD/GPL";

struct hot_event {
	u32 tgid;
	u32 pid;
	char comm[16];
	u64 lock_addr;
	u64 waiters;
	u64 verdict; /* 0 = cold (shim would skip), 1 = hot (shim would proceed) */
};

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 256 * 1024);
} rb SEC(".maps");

struct tgid_counts {
	u64 hot;
	u64 cold;
};

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 1024);
	__type(key, u32);   /* tgid */
	__type(value, struct tgid_counts);
} tgid_stats SEC(".maps");

SEC("kprobe/__x64_sys_ivh_cs_enter")
int observe_ivh_cs_enter(struct pt_regs *ctx)
{
	struct pt_regs *real_regs = (struct pt_regs *)ctx->di;
	u64 lock_addr = 0, waiters = 0, verdict = 0;
	u64 pidtgid = bpf_get_current_pid_tgid();
	u32 tgid = (u32)(pidtgid >> 32);
	u32 pid = (u32)pidtgid;

	bpf_probe_read_kernel(&lock_addr, sizeof(lock_addr), &real_regs->di);
	bpf_probe_read_kernel(&waiters, sizeof(waiters), &real_regs->si);
	bpf_probe_read_kernel(&verdict, sizeof(verdict), &real_regs->dx);

	struct tgid_counts *c = bpf_map_lookup_elem(&tgid_stats, &tgid);
	if (c) {
		if (verdict)
			c->hot++;
		else
			c->cold++;
	} else {
		struct tgid_counts new_c = { .hot = 0, .cold = 0 };
		if (verdict)
			new_c.hot = 1;
		else
			new_c.cold = 1;
		bpf_map_update_elem(&tgid_stats, &tgid, &new_c, BPF_ANY);
	}

	struct hot_event *e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (e) {
		e->tgid = tgid;
		e->pid = pid;
		bpf_get_current_comm(e->comm, sizeof(e->comm));
		e->lock_addr = lock_addr;
		e->waiters = waiters;
		e->verdict = verdict;
		bpf_ringbuf_submit(e, 0);
	}
	return 0;
}
