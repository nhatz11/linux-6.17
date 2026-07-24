// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * ivh_exec_stats.bpf.c — ground-truth host-preemption-during-critical-section
 * classifier for a wrapped process's REAL kernel raw-spinlock activity.
 *
 * Mirrors kernel/locking/spinlock.c's cs_enter()/cs_exit() exactly, but for an
 * arbitrary (non-PF_IVH_ELIGIBLE) target and with its own userspace-visible
 * aggregation:
 *
 *   - Acquire side (fexit on the _raw_spin_lock* family): on the OUTERMOST
 *     acquire for a thread, snapshot this vCPU's cumulative host steal-ns
 *     (the same per-CPU `steal_time` struct arch/x86/kernel/kvm.c's
 *     ivh_this_cpu_steal_ns() reads).
 *   - Release side (fentry on the _raw_spin_unlock* family): on the OUTERMOST
 *     release, diff steal-ns; if it advanced by more than IVH_HOT_STEAL_FLOOR_NS
 *     (100us) the vCPU was preempted by the host DURING the hold — a real
 *     lock-holder-preemption event. Count it.
 *
 * The lock is held across the whole [outermost acquire, outermost release]
 * window, so preemption is off and the acquire and release run on the SAME
 * vCPU — the only thing that can advance the per-CPU steal counter in that
 * window is genuine host preemption. This is the same invariant cs_exit()
 * documents.
 *
 * Nesting is tracked per-thread (keyed by TID) in cs_map, so we snapshot/diff
 * only on the outermost lock, mirroring cs_enter()/cs_exit()'s
 * lock_depth==1 / lock_depth==0 gating (BPF cannot read current->lock_depth
 * directly, so we keep our own counter).
 */
#include "../../vmlinux.h"
#include "bpf_helpers.h"

char LICENSE[] SEC("license") = "Dual BSD/GPL";

/*
 * Per-CPU cumulative host steal-time struct — the exact variable
 * ivh_this_cpu_steal_ns() reads via kvm_steal_clock(). Exposed to BPF as a
 * per-CPU ksym (BTF VAR 'steal_time' in the .data..percpu datasec).
 */
extern struct kvm_steal_time steal_time __ksym;

/* 100us floor — mirrors IVH_HOT_STEAL_FLOOR_NS in include/linux/bpf_sched.h */
#define IVH_HOT_STEAL_FLOOR_NS 100000ULL

/*
 * Target thread group. Set from userspace before load. We key nesting state by
 * TID (per-thread) but gate observation by TGID, so we follow every thread in
 * the wrapped process's thread group (pthreads / CLONE_THREAD share the TGID)
 * without tracking unrelated system-wide spinlock traffic.
 */
const volatile __u32 targ_tgid = 0;

/* Aggregates, read from the skeleton's .bss after the child exits. */
__u64 total_holds = 0;		/* outermost kernel lock holds observed */
__u64 stolen_holds = 0;		/* ... of which host-preempted mid-hold */

struct cs_entry {
	__u64 steal_start;	/* cumulative steal-ns at outermost acquire */
	__u32 depth;		/* raw-spinlock nesting depth for this thread */
};

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 16384);
	__type(key, __u32);	/* TID */
	__type(value, struct cs_entry);
} cs_map SEC(".maps");

static __always_inline __u64 this_cpu_steal_ns(void)
{
	struct kvm_steal_time *st = bpf_this_cpu_ptr(&steal_time);

	if (!st)
		return 0;
	return st->steal;
}

/*
 * Outermost acquire: snapshot steal. Called from fexit of the _raw_spin_lock*
 * family, i.e. after the lock is actually held (preemption off, this_cpu
 * stable) — matching cs_enter(), which runs after lock_depth++.
 */
static __always_inline int on_acquire(void)
{
	__u64 pidtgid = bpf_get_current_pid_tgid();
	__u32 tgid = (__u32)(pidtgid >> 32);
	__u32 tid = (__u32)pidtgid;
	struct cs_entry *e;
	struct cs_entry ent;

	if (targ_tgid && tgid != targ_tgid)
		return 0;

	e = bpf_map_lookup_elem(&cs_map, &tid);
	if (e) {
		e->depth++;		/* nested acquire: outermost snapshot stands */
		return 0;
	}

	ent.steal_start = this_cpu_steal_ns();
	ent.depth = 1;
	bpf_map_update_elem(&cs_map, &tid, &ent, BPF_NOEXIST);
	return 0;
}

/*
 * Outermost release: diff steal, classify. Called from fentry of the
 * _raw_spin_unlock* family, i.e. while the lock is still held (preemption off,
 * same vCPU as the acquire) — matching cs_exit()'s same-vCPU guarantee.
 */
static __always_inline int on_release(void)
{
	__u64 pidtgid = bpf_get_current_pid_tgid();
	__u32 tid = (__u32)pidtgid;
	struct cs_entry *e;
	__u64 delta;

	e = bpf_map_lookup_elem(&cs_map, &tid);
	if (!e)
		return 0;		/* acquire not observed (e.g. trylock) */

	if (e->depth > 1) {
		e->depth--;		/* nested release: wait for outermost */
		return 0;
	}

	delta = this_cpu_steal_ns() - e->steal_start;
	bpf_map_delete_elem(&cs_map, &tid);

	__sync_fetch_and_add(&total_holds, 1);
	if (delta > IVH_HOT_STEAL_FLOOR_NS)
		__sync_fetch_and_add(&stolen_holds, 1);
	return 0;
}

SEC("fexit/_raw_spin_lock")
int BPF_PROG(acq_lock) { return on_acquire(); }
SEC("fexit/_raw_spin_lock_irqsave")
int BPF_PROG(acq_lock_irqsave) { return on_acquire(); }
SEC("fexit/_raw_spin_lock_irq")
int BPF_PROG(acq_lock_irq) { return on_acquire(); }
SEC("fexit/_raw_spin_lock_bh")
int BPF_PROG(acq_lock_bh) { return on_acquire(); }

SEC("fentry/_raw_spin_unlock")
int BPF_PROG(rel_unlock) { return on_release(); }
SEC("fentry/_raw_spin_unlock_irqrestore")
int BPF_PROG(rel_unlock_irqrestore) { return on_release(); }
SEC("fentry/_raw_spin_unlock_irq")
int BPF_PROG(rel_unlock_irq) { return on_release(); }
SEC("fentry/_raw_spin_unlock_bh")
int BPF_PROG(rel_unlock_bh) { return on_release(); }
