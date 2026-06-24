// SPDX-License-Identifier: GPL-2.0
#include <linux/bpf.h>
#include <linux/cgroup.h>
#include <linux/bpf_verifier.h>
#include <linux/bpf_sched.h>
#include <linux/btf_ids.h>
#include <linux/sched/clock.h>
#include <linux/syscalls.h>
#include <linux/sysctl.h>
#include "sched.h"

/*
 * Capacity threshold: migrate when cpu_capacity falls below this value.
 * Scale is SCHED_CAPACITY_SCALE = 1024.  900/1024 ≈ 87.9% — below this
 * the vCPU is measurably throttled by the hypervisor.  Mirrors Gate 5 in
 * MY_ivh_atc.bpf.c (rq->cpu_capacity > 900 → skip).
 */
#define IVH_CAPACITY_THRESHOLD	900u

/*
 * Time-left gate: skip migration when this many nanoseconds remain in the
 * estimated active burst.  Tunable at runtime via sysctl without a rebuild:
 *   echo 250000 > /proc/sys/kernel/ivh_time_left_threshold_ns
 * Default 500 μs — wide enough to absorb EWMA noise and the ~10 ms tick
 * lag in last_preemption, yet short enough to catch most end-of-burst locks.
 */
unsigned long ivh_time_left_threshold_ns = 500000UL;

#ifdef CONFIG_SYSCTL
static const struct ctl_table ivh_sysctls[] = {
	{
		.procname	= "ivh_time_left_threshold_ns",
		.data		= &ivh_time_left_threshold_ns,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
};

static int __init ivh_sysctl_init(void)
{
	register_sysctl_init("kernel", ivh_sysctls);
	return 0;
}
late_initcall(ivh_sysctl_init);
#endif /* CONFIG_SYSCTL */

/**
 * bpf_sched_lock_acquire - lock-acquisition-driven migration trigger.
 *
 * Called from cs_enter() when the outermost kernel spinlock is acquired in
 * process context (lock_depth == 1).  Applies two gates before firing
 * running_migration():
 *
 *   Gate 1 — capacity:  cpu_capacity <= IVH_CAPACITY_THRESHOLD
 *             The vCPU is being throttled by the hypervisor.
 *
 *   Gate 2 — time-left: (ewma_act_ns - act_sofar) < ivh_time_left_threshold_ns
 *             We are near the end of the estimated active burst, so
 *             preemption is imminent and migration is worthwhile.
 *             ewma_act_ns == 0 means vcap has not yet written a value;
 *             gate is skipped so IVH fires safely during early startup.
 *
 * IRQ safety: running_migration() protects its own lock with irqsave/restore.
 */
void bpf_sched_lock_acquire(void)
{
	struct rq *rq;
	u64 ewma, act_sofar;

	if (!bpf_sched_enabled())
		return;

	rq = this_rq();

	/* Gate 1: vCPU is not throttled — nothing to do */
	if (rq->cpu_capacity > IVH_CAPACITY_THRESHOLD)
		return;

	/* Gate 2: enough burst time remains — migration not urgent yet */
	ewma = rq->ewma_act_ns;
	if (ewma != 0) {
		/* sched_clock() - last_preemption ≈ time into current active burst */
		act_sofar = sched_clock() - rq->last_preemption;
		if (ewma > act_sofar &&
		    (ewma - act_sofar) >= ivh_time_left_threshold_ns)
			return;
	}

	running_migration(rq);
}
EXPORT_SYMBOL_GPL(bpf_sched_lock_acquire);

/**
 * sys_ivh_cs_enter - userspace lock-acquisition migration trigger.
 *
 * Called from pthread_spin_lock() (glibc nptl patch) and NHextend grab_lock()
 * immediately after acquiring a userspace spinlock — the userspace mirror of
 * the bpf_sched_lock_acquire() path that fires at kernel _raw_spin_lock().
 *
 * Same decision rule as bpf_sched_lock_acquire():
 *   cpu_capacity <= IVH_CAPACITY_THRESHOLD → trigger running_migration().
 *
 * Overhead: ~150 ns syscall overhead on the no-migrate fast path (the
 * cpu_capacity check returns immediately if the vCPU is healthy).
 * Zero overhead when IVH is not loaded (bpf_sched_enabled() static key).
 */
SYSCALL_DEFINE0(ivh_cs_enter)
{
	struct rq *rq;

	if (!bpf_sched_enabled())
		return 0;

	rq = this_rq();

	if (rq->cpu_capacity > IVH_CAPACITY_THRESHOLD)
		return 0;

	running_migration(rq);
	return 0;
}

DEFINE_STATIC_KEY_FALSE(bpf_sched_enabled_key);

/*
 * For every hook declare a nop function where a BPF program can be attached.
 */
#define BPF_SCHED_HOOK(RET, DEFAULT, NAME, ...)         \
noinline RET bpf_sched_##NAME(__VA_ARGS__)      \
{                                               \
        return DEFAULT;                         \
}

#include <linux/sched_hook_defs.h>
#undef BPF_SCHED_HOOK

#define BPF_SCHED_HOOK(RET, DEFAULT, NAME, ...) BTF_ID(func, bpf_sched_##NAME)
BTF_SET_START(bpf_sched_hooks)
#include <linux/sched_hook_defs.h>
#undef BPF_SCHED_HOOK
BTF_SET_END(bpf_sched_hooks)

int bpf_sched_verify_prog(struct bpf_verifier_log *vlog,
                          const struct bpf_prog *prog)
{
        if (!prog->gpl_compatible) {
                bpf_log(vlog,
                        "sched programs must have a GPL compatible license\n");
                return -EINVAL;
        }

        if (!btf_id_set_contains(&bpf_sched_hooks, prog->aux->attach_btf_id)) {
                bpf_log(vlog, "attach_btf_id %u points to wrong type name %s\n",
                        prog->aux->attach_btf_id, prog->aux->attach_func_name);
                return -EINVAL;
        }

        return 0;
}

BPF_CALL_1(bpf_sched_entity_to_tgidpid, struct sched_entity *, se)
{
        if (entity_is_task(se)) {
                struct task_struct *task = task_of(se);

                return (u64) task->tgid << 32 | task->pid;
        } else {
                return (u64) -1;
        }
}

BPF_CALL_1(bpf_sched_entity_to_cgrpid, struct sched_entity *, se)
{
#ifdef CONFIG_FAIR_GROUP_SCHED
        if (!entity_is_task(se))
                return cgroup_id(se->cfs_rq->tg->css.cgroup);
#endif
        return (u64) -1;
}

BPF_CALL_2(bpf_sched_entity_belongs_to_cgrp, struct sched_entity *, se,
           u64, cgrpid)
{
#ifdef CONFIG_CGROUPS
        struct cgroup *cgrp;
        int level;

        if (entity_is_task(se))
                cgrp = task_dfl_cgroup(task_of(se));
#ifdef CONFIG_FAIR_GROUP_SCHED
        else
                cgrp = se->cfs_rq->tg->css.cgroup;
#endif

        for (level = cgrp->level; level; level--)
                if (cgrp->ancestors[level]->self.id == cgrpid)
                        return 1;
#endif
        return 0;
}

BTF_ID_LIST_SINGLE(bpf_sched_entity_ids, struct, sched_entity)

static const struct bpf_func_proto bpf_sched_entity_to_tgidpid_proto = {
        .func           = bpf_sched_entity_to_tgidpid,
        .gpl_only       = false,
        .ret_type       = RET_INTEGER,
        .arg1_type      = ARG_PTR_TO_BTF_ID,
        .arg1_btf_id    = &bpf_sched_entity_ids[0],
};

static const struct bpf_func_proto bpf_sched_entity_to_cgrpid_proto = {
        .func           = bpf_sched_entity_to_cgrpid,
        .gpl_only       = false,
        .ret_type       = RET_INTEGER,
        .arg1_type      = ARG_PTR_TO_BTF_ID,
        .arg1_btf_id    = &bpf_sched_entity_ids[0],
};

static const struct bpf_func_proto bpf_sched_entity_belongs_to_cgrp_proto = {
        .func           = bpf_sched_entity_belongs_to_cgrp,
        .gpl_only       = false,
        .ret_type       = RET_INTEGER,
        .arg1_type      = ARG_PTR_TO_BTF_ID,
        .arg1_btf_id    = &bpf_sched_entity_ids[0],
        .arg2_type      = ARG_ANYTHING,
};


static const struct bpf_func_proto *
bpf_sched_func_proto(enum bpf_func_id func_id, const struct bpf_prog *prog)
{
        switch (func_id) {
        case BPF_FUNC_trace_printk:
                return bpf_get_trace_printk_proto();
        case BPF_FUNC_sched_entity_to_tgidpid:
                return &bpf_sched_entity_to_tgidpid_proto;
        case BPF_FUNC_sched_entity_to_cgrpid:
                return &bpf_sched_entity_to_cgrpid_proto;
        case BPF_FUNC_per_cpu_ptr:
                return &bpf_per_cpu_ptr_proto;
        case BPF_FUNC_this_cpu_ptr:
                return &bpf_this_cpu_ptr_proto;
        case BPF_FUNC_loop:
                return &bpf_loop_proto;
        case BPF_FUNC_spin_lock:
                return &bpf_spin_lock_proto;
        case BPF_FUNC_spin_unlock:
                return &bpf_spin_unlock_proto;
        case BPF_FUNC_jiffies64:
                return &bpf_jiffies64_proto;
        case BPF_FUNC_sched_entity_belongs_to_cgrp:
                return &bpf_sched_entity_belongs_to_cgrp_proto;
        case BPF_FUNC_ringbuf_reserve:
                return &bpf_ringbuf_reserve_proto;
        case BPF_FUNC_ringbuf_submit:
                return &bpf_ringbuf_submit_proto;
        case BPF_FUNC_ringbuf_discard:
                return &bpf_ringbuf_discard_proto;
        default:
                return bpf_base_func_proto(func_id, prog);
        }
}

const struct bpf_prog_ops bpf_sched_prog_ops = {
};

const struct bpf_verifier_ops bpf_sched_verifier_ops = {
        .get_func_proto = bpf_sched_func_proto,
        .is_valid_access = btf_ctx_access,
};
