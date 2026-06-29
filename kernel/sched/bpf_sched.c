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
 * Capacity gate: trigger IVH only when the source vCPU is at or below this
 * fraction of full capacity (scale 0–1024).  Default 512 = 50%: migrate only
 * from severely stolen vCPUs where the gain is unambiguous and a clearly
 * better target is more likely to exist.  Raising toward 900 makes IVH more
 * aggressive (fires at ~12% steal) but risks pool exhaustion under heavy load.
 *   echo 512 > /proc/sys/kernel/ivh_capacity_threshold
 */
unsigned long ivh_capacity_threshold = 512UL;

/*
 * Time-left gate: skip migration when this many nanoseconds remain in the
 * estimated active burst.  Tunable at runtime via sysctl without a rebuild:
 *   echo 250000 > /proc/sys/kernel/ivh_time_left_threshold_ns
 * Default 500 μs — wide enough to absorb EWMA noise and the ~10 ms tick
 * lag in last_preemption, yet short enough to catch most end-of-burst locks.
 */
unsigned long ivh_time_left_threshold_ns = 500000UL;

/*
 * Migration watchdog timeout (ns): if schedule() does not return within this
 * window after set_cpus_allowed_ptr({target}), the target vCPU is assumed
 * stolen and the original affinity is restored so the thread can run on any
 * healthy CPU instead of waiting indefinitely.
 * Default 500 µs (~8× the typical 65 µs migration RTT).
 * Set to 0 to disable the watchdog (not recommended under steal).
 *   echo 500000 > /proc/sys/kernel/ivh_migration_timeout_ns
 */
unsigned long ivh_migration_timeout_ns = 500000UL;

/*
 * Concurrency cap: maximum number of threads allowed inside schedule()
 * simultaneously during IVH migration.  Prevents pool exhaustion under
 * heavy steal where many threads pass the gates at once.
 * Default 3.  Set to 0 to disable the cap.
 *   echo 3 > /proc/sys/kernel/ivh_max_concurrent
 */
unsigned long ivh_max_concurrent = 3UL;

#ifdef CONFIG_SYSCTL
static const struct ctl_table ivh_sysctls[] = {
	{
		.procname	= "ivh_capacity_threshold",
		.data		= &ivh_capacity_threshold,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_time_left_threshold_ns",
		.data		= &ivh_time_left_threshold_ns,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_migration_timeout_ns",
		.data		= &ivh_migration_timeout_ns,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_max_concurrent",
		.data		= &ivh_max_concurrent,
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
 * sys_ivh_cs_enter - userspace pre-lock migration trigger.
 *
 * Called from pthread_spin_lock() (glibc nptl patch) and NHextend grab_lock()
 * BEFORE acquiring the userspace spinlock — mirrors ivh_pre_lock() in the
 * kernel spinlock path.  bpf_sched_pre_lock_migrate() applies the same gates
 * (capacity, time-left, movability, no pending migration) and performs a
 * synchronous self-migration so the lock is acquired on a good vCPU.
 */
SYSCALL_DEFINE0(ivh_cs_enter)
{
	if (!bpf_sched_enabled())
		return 0;
	if (!(current->flags & PF_IVH_ELIGIBLE))
		return 0;

	bpf_sched_pre_lock_migrate();
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
