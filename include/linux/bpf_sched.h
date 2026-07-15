/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BPF_SCHED_H
#define _BPF_SCHED_H

#include <linux/bpf.h>
#include <linux/percpu.h>

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
 * calling task migrates itself synchronously via ivh_migrate_self() (a bare
 * stop_one_cpu() dispatch, below) so the subsequent lock acquisition happens
 * on a good CPU.
 * Implemented in kernel/sched/fair.c.
 */
void bpf_sched_pre_lock_migrate(void);

/*
 * Relocate the current (running) task directly to target_cpu via a bare
 * stop_one_cpu() dispatch — the same lightweight mechanism sched_exec()
 * uses (kernel/sched/core.c) — instead of the general-purpose
 * set_cpus_allowed_ptr() API. Never touches current->cpus_mask, so no
 * restore call is needed and no window exists for a concurrent legitimate
 * affinity change to be silently clobbered.
 * Implemented in kernel/sched/core.c.
 */
void ivh_migrate_self(int target_cpu);

/*
 * Stage A1: live (not post-hoc) "is this vCPU preempted right now" bit --
 * see the definition site (kernel/sched/core.c) for why this differs from
 * get_steal_and_preemptions()'s cumulative steal_time (kernel/sched/sched.h).
 * For a module (vsched_module.c's planned shared health page) to read.
 * Implemented in kernel/sched/core.c.
 */
int ivh_get_vcpu_preempted(int cpunum);

/* Runtime-tunable capacity gate; defined in bpf_sched.c.
 * Gate 1 fires when rq->cpu_capacity <= this value (scale: 0–1024).
 * Default 512 = 50% capacity = trigger only on severely stolen vCPUs.
 *   echo 512 > /proc/sys/kernel/ivh_capacity_threshold
 */
extern unsigned long ivh_capacity_threshold;

/*
 * 2026-07-07: the CS-duration prefilter that used to live here
 * (ivh_min_cs_ns) is removed. It made sense for locus (c), which pays a
 * migration cost while holding the lock (waiters spin the whole time), but
 * locus (a) migrates before any lock is attempted and holds nothing during
 * its own migration -- there was no equivalent economic reason for a
 * duration floor here, and it silently vetoed the short-CS workloads where
 * this locus's own measured win lives. See ivh_pre_lock_hotlock_enabled
 * below for the value-based selectivity gate that replaces it.
 */

/* Optional selectivity gate for locus (a): reuses locus (c)'s Hotlock table
 * to skip migrating ahead of a lock attempt when that lock has no live
 * waiters and no recent contention history -- i.e. gate on whether anyone
 * else would be hurt, not on how long the CS runs. Default OFF: locus (a)'s
 * measured win may come partly from "self-rescue" migrations ahead of
 * COLD lock attempts too (escaping a bad vCPU regardless of which lock
 * triggered it), which this gate would filter out -- validate via
 * ivh_prelock_hotlock_cold_skipped/hot_passed before trusting it helps.
 * Defined in bpf_sched.c.
 *   echo 1 > /proc/sys/kernel/ivh_pre_lock_hotlock_enabled
 */
extern unsigned long ivh_pre_lock_hotlock_enabled;

/* Locus (a) Hotlock-gate counters, defined in kernel/locking/spinlock.c,
 * surfaced via /proc/ivh_debug (kernel/sched/fair.c). */
DECLARE_PER_CPU(u64, ivh_prelock_hotlock_cold_skipped);
DECLARE_PER_CPU(u64, ivh_prelock_hotlock_hot_passed);

/* Runtime-tunable time-left gate (ns); defined in bpf_sched.c */
extern unsigned long ivh_time_left_threshold_ns;

/*
 * 2026-07-14: runtime toggle between the two time-left formulas found to
 * differ between this tree and the 84f1e5fcc ("34+") commit: 0 (default) =
 * this tree's native rq->last_active_time calculation (kernel/sched/fair.c,
 * ivh_steal_imminent()); 1 = the older rq->ewma_act_ns-based formula 34+
 * uses (externally smoothed, written by vsched_module via set_ewma_act_ns(),
 * fed by vcap's /proc/vact_write). Both read the same ivh_time_left_threshold_ns
 * sysctl above, just apply it to a different signal. Defined in bpf_sched.c.
 *   echo 1 > /proc/sys/kernel/ivh_time_left_source
 */
extern unsigned long ivh_time_left_source;

/* Runtime-tunable migration watchdog timeout (ns); defined in bpf_sched.c.
 * If schedule() does not return within this window the original affinity is
 * restored from hrtimer context so the thread escapes a stolen target vCPU.
 * Set to 0 to disable.  Default 500 µs.
 */
extern unsigned long ivh_migration_timeout_ns;

/* Concurrency cap: max threads allowed in schedule() during IVH migration. */
extern unsigned long ivh_max_concurrent;

/* Count of threads currently inside an IVH migration's stop_one_cpu() wait
 * (both loci share this budget). Defined in kernel/sched/fair.c. Also used
 * by ivh_virt_spin_lock() (kernel/locking/qspinlock.c) as a cheap way to
 * skip the yield check entirely when no migration is in flight anywhere --
 * see the comment there for why that's safe. */
extern atomic_t ivh_in_schedule;

/* schedule_timeout_interruptible duration for IVH migration (ms).
 * How long to wait for the target vCPU before giving up and restoring affinity.
 *   echo 5 > /proc/sys/kernel/ivh_sched_timeout_ms
 */
extern unsigned long ivh_sched_timeout_ms;

/*
 * Per-vCPU evaluation cooldown (ns); defined in bpf_sched.c.
 * Minimum spacing between full IVH pre-lock evaluations on the same vCPU,
 * regardless of which thread/lock triggers ivh_pre_lock(). Set 0 to disable.
 *   echo 50000 > /proc/sys/kernel/ivh_eval_cooldown_ns
 */
extern unsigned long ivh_eval_cooldown_ns;

/*
 * Shadow-mode Hotlock master switch; defined in bpf_sched.c. Gates
 * ivh_post_lock() (kernel/locking/spinlock.c), which samples the Hotlock
 * table on every real, eligible spinlock acquisition for observation --
 * this build has no real post-acquisition migration to gate.
 *   echo 1 > /proc/sys/kernel/ivh_post_lock_enabled
 */
extern unsigned long ivh_post_lock_enabled;

/*
 * Hotlock EWMA shift constant; defined in bpf_sched.c. See
 * ivh_hotlock_update()/ivh_hotlock_read() in kernel/locking/spinlock.c.
 */
extern unsigned long ivh_hotlock_ewma_k;

/* Fixed-point scale for the Hotlock EWMA, used by
 * ivh_hotlock_update()/ivh_hotlock_read() (kernel/locking/spinlock.c). */
#define IVH_HOTLOCK_BITS   12                          /* 4096 buckets, ~64KB table */
#define IVH_HOTLOCK_SIZE   (1 << IVH_HOTLOCK_BITS)
#define IVH_HOTLOCK_SCALE  10                          /* fixed point: 1<<10 == "1.0" */
#define IVH_HOTLOCK_HALF   (1 << (IVH_HOTLOCK_SCALE - 1))

/* Shadow-mode Hotlock counters, defined in kernel/locking/spinlock.c,
 * surfaced via /proc/ivh_debug (kernel/sched/fair.c). Per-CPU: see the
 * definition site for why (cache-line bouncing at this call volume). */
DECLARE_PER_CPU(u64, ivh_post_lock_calls);
DECLARE_PER_CPU(u64, ivh_post_lock_hot);

/*
 * Live per-lock waiter count, defined in kernel/locking/spinlock.c. Called
 * from kernel/locking/qspinlock.c at the real qspinlock contention entry/exit
 * points (pending-bit optimistic spin, full MCS queueing), bracketing them
 * the same way wait_depth++/-- already does. See the comment above these
 * functions' definitions for why this replaced both
 * queued_spin_is_contended() and the earlier per-task ivh_took_slowpath flag
 * as Hotlock's contention sample.
 */
void ivh_hotlock_note_waiter_enter(const void *lock);
void ivh_hotlock_note_waiter_exit(const void *lock);

/* Master switch for the trace_printk() inside ivh_steal_imminent() --
 * defaults off since it fires on every call that clears the capacity gate,
 * real cost at high call volume; defined in bpf_sched.c. */
extern unsigned long ivh_trace_enabled;

/* 2026-07-06 freeze fix: master switch for the yield inside
 * ivh_virt_spin_lock() (kernel/locking/qspinlock.c). Defined in bpf_sched.c,
 * see the definition site for the full rationale. */
extern unsigned long ivh_spin_yield_enabled;

/* 2026-07-06 freeze fix: master switch for the transient priority boost in
 * ivh_migrate_self() (kernel/sched/core.c). Defined in bpf_sched.c. */
extern unsigned long ivh_migrate_boost;

/*
 * ivh_eval_cooldown_ok - per-vCPU rate limiter on full IVH pre-lock
 * evaluation.  Returns true (and stamps the cooldown) if this vCPU is due
 * for another full evaluation; false if one happened too recently.
 * Implemented in kernel/sched/fair.c.
 */
bool ivh_eval_cooldown_ok(void);

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
