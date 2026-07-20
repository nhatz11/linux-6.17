/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BPF_SCHED_H
#define _BPF_SCHED_H

#include <linux/bpf.h>

#ifdef CONFIG_BPF_SYSCALL

#include <linux/jump_label.h>
#include <linux/percpu.h>

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

/* Runtime-tunable capacity gate; defined in bpf_sched.c.
 * Gate 1 fires when rq->cpu_capacity <= this value (scale: 0–1024).
 * Default 512 = 50% capacity = trigger only on severely stolen vCPUs.
 *   echo 512 > /proc/sys/kernel/ivh_capacity_threshold
 */
extern unsigned long ivh_capacity_threshold;

/* Runtime-tunable time-left gate (ns); defined in bpf_sched.c */
extern unsigned long ivh_time_left_threshold_ns;

/* Runtime-tunable migration watchdog timeout (ns); defined in bpf_sched.c.
 * If schedule() does not return within this window the original affinity is
 * restored from hrtimer context so the thread escapes a stolen target vCPU.
 * Set to 0 to disable.  Default 500 µs.
 */
extern unsigned long ivh_migration_timeout_ns;

/* Gate 1+2 "time left" formula toggle (0 = ewma_act_ns, 1 = last_active_time);
 * defined in bpf_sched.c, read by ivh_steal_imminent() in fair.c. */
extern unsigned long ivh_time_left_source;

/* Destination-selection lock toggle (0 = blocking, 1 = trylock-and-skip);
 * defined in bpf_sched.c, read by bpf_sched_pre_lock_migrate() in fair.c. */
extern unsigned long ivh_selection_trylock;

/* Migration dispatch mechanism toggle (0 = set_cpus_allowed_ptr()+schedule(),
 * 1 = migrate_task_to()/stop_one_cpu()-based); defined in bpf_sched.c, read by
 * bpf_sched_pre_lock_migrate() in fair.c. See bpf_sched.c for the full
 * tradeoff comment. */
extern unsigned long ivh_migrate_mechanism;

/* Concurrency cap: max threads allowed in schedule() during IVH migration. */
extern unsigned long ivh_max_concurrent;

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
 * ivh_eval_cooldown_ok - per-vCPU rate limiter on full IVH pre-lock
 * evaluation.  Returns true (and stamps the cooldown) if this vCPU is due
 * for another full evaluation; false if one happened too recently.
 * Implemented in kernel/sched/fair.c.
 */
bool ivh_eval_cooldown_ok(void);

/*
 * ---------------------------------------------------------------------
 * Hot Threads: per-task contention/preemption classifier (default OFF).
 * Replaces the earlier per-lock "Hotlock" design.
 *
 * Two independent, event-driven EWMAs live directly on task_struct
 * (ivh_wait_decay, ivh_preempt_decay — include/linux/sched.h): a thread is
 * classified "hot" (worth an IVH migration) only if it BOTH contends real
 * locks AND has actually been caught by host steal mid-critical-section.
 * ivh_pre_lock() (kernel/locking/spinlock.c) consults these directly on
 * `current` — no shared table, no hashing, single writer per counter.
 * See kernel/locking/spinlock.c for the update/classification logic.
 * ---------------------------------------------------------------------
 */

/* Selectivity gate for ivh_pre_lock(); defined in bpf_sched.c. Default OFF.
 * When on, ivh_pre_lock() consults the calling task's ivh_wait_decay/
 * ivh_preempt_decay and skips the migration trigger unless both are above
 * threshold.  NOTE: enabling this removes IVH's "self-rescue" migrations
 * ahead of cold-thread attempts, so validate via the
 * ivh_prelock_coldthread_skipped/hotthread_passed counters (/proc/ivh_debug)
 * that it doesn't cost a measured win before trusting it.
 *   echo 1 > /proc/sys/kernel/ivh_hot_threads_enabled
 */
extern unsigned long ivh_hot_threads_enabled;

/* Bypass PF_IVH_ELIGIBLE entirely: every task's contended raw_spinlock
 * acquisitions feed Hot Threads / trigger ivh_pre_lock(), not just tasks
 * launched under ivh_exec. Default OFF -- ONLY the opt-in population is
 * touched otherwise. All other ivh_pre_lock() safety gates (bpf_sched_
 * enabled(), in_task(), preemptible(), lock_depth == 0, TASK_RUNNING) are
 * unaffected and still apply -- this widens WHICH tasks are considered,
 * not WHICH CONTEXTS are legal. Intended for a one-shot "does the
 * mechanism hold up against the unrestricted, full-system population"
 * test; expect much higher call volume and re-tune ivh_max_concurrent /
 * ivh_eval_cooldown_ns if needed. Defined in bpf_sched.c.
 *   echo 1 > /proc/sys/kernel/ivh_universal_eligible
 */
extern unsigned long ivh_universal_eligible;

/* Whether ivh_pre_lock()'s Hot Threads gate also consults ivh_preempt_decay
 * (the old AND-gate) or only ivh_wait_decay (the validated-good default).
 * Default OFF -- preempt_decay's real-KVM-steal-during-a-kernel-raw-
 * spinlock-hold signal is starved (reads exactly 0 for 90%+ of eligible
 * evaluations, measured live) and gating on it caused a severe regression
 * (2% -> 12%+ host-preempted-CS). Set to 1 to restore the old AND-gate for
 * A/B comparison. Defined in bpf_sched.c.
 *   echo 1 > /proc/sys/kernel/ivh_hot_preempt_gate_enabled
 */
extern unsigned long ivh_hot_preempt_gate_enabled;

/* Hot Threads AND-gate thresholds, IVH_HOTLOCK_SCALE fixed point.  Defined
 * in bpf_sched.c.
 *   echo 256 > /proc/sys/kernel/ivh_hot_wait_threshold
 *   echo 20  > /proc/sys/kernel/ivh_hot_preempt_threshold
 */
extern unsigned long ivh_hot_wait_threshold;
extern unsigned long ivh_hot_preempt_threshold;

/* Hot Threads EWMA shift constant for wait_decay ONLY (alpha = 1/2^k;
 * smaller k reacts faster but is noisier).  Default 3 (alpha = 1/8).
 * preempt_decay uses its own, separate, asymmetric pair below -- NOT this
 * constant -- see ivh_hot_preempt_ewma_k_rise/_fall.  Defined in bpf_sched.c.
 *   echo 4 > /proc/sys/kernel/ivh_hot_threads_ewma_k
 */
extern unsigned long ivh_hot_threads_ewma_k;

/* Hot Threads: preempt_decay's asymmetric cooldown shift constants (see
 * ivh_hot_preempt_update(), kernel/locking/spinlock.c). A real catch
 * (stolen==true) uses the FAST k_rise; a clean release uses the SLOWER
 * k_fall. Deliberately NOT one shared constant -- a symmetric EWMA (rise
 * and fall coupled to the same k) was measured to cause a severe
 * self-undermining feedback loop when used as a live IVH gate: successful
 * protection suppresses the catch evidence needed to stay "hot", so the
 * gate flaps shut, protection lapses, a real catch lands, the gate
 * reopens, and the cycle repeats. Decoupling rise from fall lets the
 * protected-state equilibrium sit comfortably above threshold (so genuine
 * exposure keeps the gate open) while still allowing a thread whose real
 * behavior later goes cold to eventually lapse back out, unlike a
 * permanent latch. Defaults k_rise=3 (unchanged trigger speed), k_fall=8
 * (~700-hold memory once a catch stops recurring). Defined in bpf_sched.c.
 *   echo 8 > /proc/sys/kernel/ivh_hot_preempt_ewma_k_fall
 */
extern unsigned long ivh_hot_preempt_ewma_k_rise;
extern unsigned long ivh_hot_preempt_ewma_k_fall;

/* Hot Threads: how often (1-in-N release events) wait_decay receives an
 * occasional 0-sample from cs_exit(), so it reflects RECENT contention
 * rather than lifetime contention -- without this, wait_decay only ever
 * receives SCALE-samples (at contended-wait entry) and can never decay: a
 * thread that contended briefly (e.g. a daemon's startup) and has since
 * been idle stays permanently above threshold. Same 1-in-N coarsening the
 * old per-lock Hotlock table used for its own non-contended sample path.
 * Defined in bpf_sched.c.
 *   echo 8 > /proc/sys/kernel/ivh_hot_wait_zero_n
 */
extern unsigned long ivh_hot_wait_zero_n;

/* Hot Threads: sticky one-way wait_decay latch (default OFF). When set, a task
 * that has EVER crossed ivh_hot_wait_threshold is treated as hot permanently
 * (its per-task ivh_wait_latched bit stays armed), removing the decay-induced
 * lapse that makes a fixed threshold reject the bulk of sustained lock-holders
 * whose wait_decay sits below threshold. A never-contending task never arms,
 * so idle/cold exclusion is preserved. Defined in bpf_sched.c.
 *   echo 1 > /proc/sys/kernel/ivh_hot_wait_latch_enabled
 */
extern unsigned long ivh_hot_wait_latch_enabled;

/* Fixed-point scale for the Hot Threads EWMAs, shared by
 * ivh_hot_note_wait_event()/ivh_hot_preempt_update() (kernel/locking/spinlock.c). */
#define IVH_HOTLOCK_SCALE  10                        /* fixed point: 1<<10 == "1.0" */
#define IVH_HOTLOCK_HALF   (1 << (IVH_HOTLOCK_SCALE - 1))

/* Real-steal-vs-counter-noise floor for preempt_decay's steal-delta check
 * (kernel/locking/spinlock.c cs_exit()).  100us, matches NHextend3.c's own
 * host_preempted_count methodology. */
#define IVH_HOT_STEAL_FLOOR_NS 100000ULL

/* Per-task contended-wait EWMA feed, defined in kernel/locking/spinlock.c,
 * called from kernel/locking/qspinlock.c at the real qspinlock contention
 * entry points (pending-bit optimistic spin, MCS queueing). */
void ivh_hot_note_wait_event(void);

/* Cumulative steal-ns for the current vCPU; defined in arch/x86/kernel/kvm.c
 * (weak 0 fallback in kernel/sched/bpf_sched.c for non-KVM builds). */
u64 ivh_this_cpu_steal_ns(void);

/* Hot Threads pre-lock gate counters, defined in kernel/locking/spinlock.c,
 * surfaced via /proc/ivh_debug (kernel/sched/fair.c). */
DECLARE_PER_CPU(u64, ivh_prelock_coldthread_skipped);
DECLARE_PER_CPU(u64, ivh_prelock_hotthread_passed);

/* Observe-only stats (ivh_observe / PR_SET_IVH_ELIGIBLE arg2==2), defined in
 * kernel/locking/spinlock.c, surfaced via /proc/ivh_debug (kernel/sched/fair.c). */
DECLARE_PER_CPU(u64, ivh_obs_total_holds);
DECLARE_PER_CPU(u64, ivh_obs_stolen_holds);

/*
 * Mutex optimistic-spin observability counters, defined in
 * kernel/locking/mutex.c, surfaced via /proc/ivh_debug (kernel/sched/fair.c).
 *
 * The raw-spinlock Hot Threads wait_decay feed (qspinlock.c) never observes
 * mutex contention: mutex waiters spin in mutex_spin_on_owner() (OSQ +
 * owner-watch), not in the qspinlock slowpath.  For mutex-serialised
 * workloads (e.g. hackbench's pipe->mutex) this is the *actual*
 * LHP-sensitive spin point, which is why ivh_wait_decay never rises for
 * them.  These two counters make that spin visible (PF_IVH_ELIGIBLE tasks
 * only):
 *   spin_observed        - eligible entries into the mutex owner-watch loop
 *                          (i.e. contended mutex acquisitions that spun).
 *   spin_owner_preempted - subset that abandoned the spin because the holder's
 *                          vCPU was preempted (owner_on_cpu() == false) — the
 *                          mutex analogue of the exact LHP event IVH targets.
 * Observability only: nothing migrates off this path.
 */
DECLARE_PER_CPU(u64, ivh_mutex_spin_observed);
DECLARE_PER_CPU(u64, ivh_mutex_spin_owner_preempted);

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
