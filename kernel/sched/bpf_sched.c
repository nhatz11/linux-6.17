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
 * 2026-07-14: runtime toggle between the two time-left formulas found to
 * differ between this tree and the 84f1e5fcc ("34+") commit -- see the
 * comment on the extern declaration (include/linux/bpf_sched.h) for the
 * full rationale. 0 (default) = this tree's native rq->last_active_time
 * calculation; 1 = 34+'s externally-smoothed rq->ewma_act_ns formula.
 *   echo 1 > /proc/sys/kernel/ivh_time_left_source
 */
unsigned long ivh_time_left_source = 0UL;

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

/*
 * schedule_timeout_interruptible duration (ms) for IVH migration.
 * If the target vCPU does not schedule the migrating thread within this
 * window, affinity is restored and the thread proceeds normally.
 * Default 1 ms.  Raise if healthy CPUs are not scheduling within 1ms.
 *   echo 5 > /proc/sys/kernel/ivh_sched_timeout_ms
 */
unsigned long ivh_sched_timeout_ms = 1UL;

/*
 * Per-vCPU evaluation cooldown (ns): minimum spacing between full IVH
 * pre-lock evaluations on the same vCPU, regardless of which thread or
 * lock triggers ivh_pre_lock(). Collapses redundant re-evaluation of an
 * unchanged vCPU-health answer under high spin_lock() call volume
 * (e.g. hackbench). Set 0 to disable (evaluate on every eligible call,
 * pre-cooldown behavior).
 *   echo 50000 > /proc/sys/kernel/ivh_eval_cooldown_ns
 */
unsigned long ivh_eval_cooldown_ns = 50000UL;

/*
 * 2026-07-07: ivh_min_cs_ns removed -- see the comment above
 * ivh_pre_lock_hotlock_enabled (include/linux/bpf_sched.h) for why this
 * locus never had an economic reason for a duration floor, and its own
 * removed comment (formerly here, added 2026-07-02) turned out to be
 * silently vetoing the exact short-CS workloads this locus's real,
 * measured win comes from.
 */

/*
 * Optional selectivity gate for locus (a) -- see the comment above its
 * declaration in include/linux/bpf_sched.h. Default OFF: validate via
 * ivh_prelock_hotlock_cold_skipped/hot_passed before trusting it doesn't
 * filter out the self-rescue migrations this locus's win may depend on.
 *   echo 1 > /proc/sys/kernel/ivh_pre_lock_hotlock_enabled
 */
unsigned long ivh_pre_lock_hotlock_enabled = 0UL;

/*
 * Shadow-mode Hotlock master switch -- gates ivh_post_lock()
 * (kernel/locking/spinlock.c), which samples the Hotlock table (waiters,
 * EWMA history) and its own call/hot counters on every real, eligible
 * spinlock acquisition. This build has no real post-acquisition migration
 * (bpf_sched_post_lock_migrate() removed) -- flipping this on only makes
 * the classifier's live behavior observable via /proc/ivh_debug, to
 * validate it's actually capturing real contention correctly before any
 * future build wires a real decision to it.
 *   echo 1 > /proc/sys/kernel/ivh_post_lock_enabled
 */
unsigned long ivh_post_lock_enabled = 0UL;

/*
 * Hotlock EWMA shift constant (alpha = 1/2^k; smaller k reacts faster but is
 * noisier). Default 3 (alpha = 1/8).
 *   echo 4 > /proc/sys/kernel/ivh_hotlock_ewma_k
 */
unsigned long ivh_hotlock_ewma_k = 3UL;

/*
 * ivh_hotlock_tier1_pct, ivh_hotlock_eval_cooldown_ns, and
 * ivh_post_lock_vcpu_cooldown_ns were removed along with
 * bpf_sched_post_lock_migrate() (kernel/sched/fair.c) -- all three existed
 * only to tune that function's real dispatch, which is not part of this
 * build. See the note there for what's left: the Hotlock table/EWMA itself
 * (ivh_hotlock_ewma_k above) still runs in shadow mode via ivh_post_lock().
 */

/*
 * Master switch for the trace_printk() inside ivh_steal_imminent(). Default
 * OFF: it fires on every call that clears the capacity gate, which under
 * real contention is a large fraction of all Hotlock-hot classifications --
 * real, non-negligible cost at that volume. Turn on only when actively
 * comparing ewma_act_ns vs last_active_time.
 *   echo 1 > /proc/sys/kernel/ivh_trace_enabled
 */
unsigned long ivh_trace_enabled = 0UL;

/*
 * ivh_post_lock_min_cs_ns and ivh_post_lock_dispatch were removed along with
 * bpf_sched_post_lock_migrate() -- both were Patch 3 real-dispatch-only
 * tunables (the disruption-magnitude floor and the stub/real-dispatch
 * switch respectively). last_cs_oncpu_ns (the field the floor read) was
 * removed too, having no other reader.
 */

/*
 * 2026-07-06 freeze fix, part 1/2: master switch for the yield inside
 * ivh_virt_spin_lock() (kernel/locking/qspinlock.c). Without this, a thread
 * spinning on a contended lock with preempt_count()==1 can never be
 * dislodged once a locus (c) migration lands its confirmed holder behind it
 * on the same CPU -- preempt_count>0 is a priority-blind veto on preemption
 * on every preemption model, so nothing else can force that spinner off the
 * CPU. Default OFF for a clean A/B against the pre-fix behavior; see
 * ivh_virt_spin_lock() for the exact safety gates (preempt_count()==1,
 * !irqs_disabled(), RCU-read-side awareness).
 *   echo 1 > /proc/sys/kernel/ivh_spin_yield_enabled
 */
unsigned long ivh_spin_yield_enabled = 0UL;

/*
 * 2026-07-06 freeze fix, part 2/2: master switch for the transient
 * sched_set_fifo_low() bracket in ivh_migrate_self() (kernel/sched/core.c).
 * On its own, ivh_spin_yield_enabled bounds the wedge to about one
 * scheduler tick (CFS wakeup preemption on this VM's preempt=lazy config
 * only sets TIF_NEED_RESCHED_LAZY, which the tick eventually escalates).
 * This boost makes wakeup_preempt() take the class-above path instead
 * (unconditional resched_curr(): hard TIF_NEED_RESCHED + a real IPI,
 * eligibility-blind), collapsing that bound from ~1 tick to about one IPI
 * round trip, and additionally guarantees the migrated holder -- not the
 * spinner -- is what runs next once the spinner yields. Inert by itself
 * without ivh_spin_yield_enabled=1 (there's no yield point to react to
 * the boost yet). Default ON: boosting a lock holder is the correct
 * priority-inheritance direction, the window is transient (freed
 * unconditionally once stop_one_cpu() returns, which it always does),
 * and it does nothing unless the yield above is also enabled.
 *   echo 0 > /proc/sys/kernel/ivh_migrate_boost
 */
unsigned long ivh_migrate_boost = 1UL;

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
		.procname	= "ivh_time_left_source",
		.data		= &ivh_time_left_source,
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
	{
		.procname	= "ivh_sched_timeout_ms",
		.data		= &ivh_sched_timeout_ms,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_eval_cooldown_ns",
		.data		= &ivh_eval_cooldown_ns,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_pre_lock_hotlock_enabled",
		.data		= &ivh_pre_lock_hotlock_enabled,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_post_lock_enabled",
		.data		= &ivh_post_lock_enabled,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_hotlock_ewma_k",
		.data		= &ivh_hotlock_ewma_k,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_trace_enabled",
		.data		= &ivh_trace_enabled,
		.maxlen		= sizeof(unsigned long),
		/* 0666, not the usual 0644: ivh_exec -t (see ivh_exec.c) flips
		 * this on/off as a regular, non-root user for the duration of
		 * one traced run. Purely a debug on/off switch -- no privilege
		 * implications to widening it. */
		.mode		= 0666,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_spin_yield_enabled",
		.data		= &ivh_spin_yield_enabled,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_migrate_boost",
		.data		= &ivh_migrate_boost,
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
	/*
	 * 2026-07-07: this path had no per-vCPU cooldown at all -- unlike
	 * ivh_pre_lock() (kernel/locking/spinlock.c), the kernel-side entry
	 * point, which has always checked ivh_eval_cooldown_ok() before
	 * calling bpf_sched_pre_lock_migrate(). Every userspace pthread
	 * spinlock attempt (NHextend, the glibc patch) was paying full
	 * syscall + gate-walk cost with no throttling at all under
	 * high-call-volume workloads. Bring it in line with the kernel-side
	 * path.
	 */
	if (!ivh_eval_cooldown_ok())
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
