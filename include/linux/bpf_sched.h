/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BPF_SCHED_H
#define _BPF_SCHED_H

#include <linux/bpf.h>

#ifdef CONFIG_BPF_SYSCALL

#include <linux/jump_label.h>

#define BPF_SCHED_HOOK(RET, DEFAULT, NAME, ...) \
	RET bpf_sched_##NAME(__VA_ARGS__);
#include <linux/sched_hook_defs.h>
#undef BPF_SCHED_HOOK

int bpf_sched_verify_prog(struct bpf_verifier_log *vlog,
			  const struct bpf_prog *prog);

/*
 * Called from _raw_spin_lock*() BEFORE __raw_spin_lock*() — i.e., before
 * preemption is disabled and before any MCS node is allocated.  If the current
 * vCPU is in the IVH danger zone and a better target CPU is available, the
 * calling task migrates itself synchronously (set_cpus_allowed_ptr + schedule())
 * so that the subsequent lock acquisition happens on a good CPU.
 * Implemented in kernel/sched/fair.c.
 */
void bpf_sched_pre_lock_migrate(void);

/* Capacity threshold shared between bpf_sched.c and fair.c */
#define IVH_CAPACITY_THRESHOLD	900u

/* Runtime-tunable time-left gate (ns); defined in bpf_sched.c */
extern unsigned long ivh_time_left_threshold_ns;

DECLARE_STATIC_KEY_FALSE(bpf_sched_enabled_key);

static inline bool bpf_sched_enabled(void)
{
	return static_branch_unlikely(&bpf_sched_enabled_key);
}

static inline void bpf_sched_inc(void)
{
	static_branch_inc(&bpf_sched_enabled_key);
}

static inline void bpf_sched_dec(void)
{
	static_branch_dec(&bpf_sched_enabled_key);
}


#else /* CONFIG_BPF_SYSCALL */

#define BPF_SCHED_HOOK(RET, DEFAULT, NAME, ...)	\
static inline RET bpf_sched_##NAME(__VA_ARGS__)	\
{						\
	return DEFAULT;				\
}
#include <linux/sched_hook_defs.h>
#undef BPF_SCHED_HOOK

static inline bool bpf_sched_enabled(void)
{
	return false;
}

#endif /* CONFIG_BPF_SYSCALL */

#endif /* _BPF_CGROUP_H */
