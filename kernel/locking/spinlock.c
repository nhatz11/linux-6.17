// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (2004) Linus Torvalds
 *
 * Author: Zwane Mwaikambo <zwane@fsmlabs.com>
 *
 * Copyright (2004, 2005) Ingo Molnar
 *
 * This file contains the spinlock/rwlock implementations for the
 * SMP and the DEBUG_SPINLOCK cases. (UP-nondebug inlines them)
 *
 * Note that some architectures have special knowledge about the
 * stack frames of these functions in their profile_pc. If you
 * change anything significant here that could change the stack
 * frame contact the architecture maintainers.
 */

#include <linux/linkage.h>
#include <linux/preempt.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/debug_locks.h>
#include <linux/export.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/bpf_sched.h>
#include <linux/hash.h>
#include <linux/cache.h>

/*
 * cs_enter / cs_exit — helpers called around lock_depth transitions.
 * cs_enter: called after lock_depth reaches 1 (outermost acquire); records
 *   the start timestamp so cumulative_cs_time can be updated on release.
 * cs_exit: called after lock_depth reaches 0 (outermost release); accumulates
 *   elapsed time into cumulative_cs_time and clears cs_start_ts.
 * Both are no-ops for nested locks (lock_depth > 1 after enter, > 0 after exit).
 *
 * sched_clock() is used instead of ktime_get_ns() because spinlocks fire
 * before timekeeping is initialized; sched_clock() is safe in any context.
 */
/*
 * ivh_pre_lock — IVH gate + synchronous self-migration, called BEFORE
 * __raw_spin_lock*() so that no MCS node has been allocated yet and
 * preemption is still enabled.  A no-op when IVH is not loaded (static key).
 * Must not be called from trylock paths (they don't block) or when already
 * holding a spinlock (lock_depth > 0 means preemption is already off).
 */
/*
 * Forward declarations: the Hotlock helpers ivh_pre_lock() (below) needs are
 * defined later in this file (they were built for locus (c) and live next to
 * the rest of that infrastructure) -- these prototypes let this locus (a)
 * gate reuse them without reordering the file.
 */
static __always_inline int ivh_hotlock_waiters(const void *lock);
static __always_inline void ivh_hotlock_update(const void *lock, bool contended);
static __always_inline u16 ivh_hotlock_read(const void *lock);

static __always_inline void ivh_pre_lock(raw_spinlock_t *lock)
{
	if (!bpf_sched_enabled())
		return;
	if (!(current->flags & PF_IVH_ELIGIBLE))
		return;
	/*
	 * 2026-07-07: the CS-length floor that used to live here (skip if
	 * current->last_cs_ns was shorter than a fixed threshold) is gone.
	 * That check made sense for locus (c) (which migrates WHILE holding
	 * the lock, so waiters pay the migration cost) but never made sense
	 * here: this locus migrates BEFORE any lock is attempted, holds
	 * nothing during its own migration, and has no waiter population to
	 * protect from a duration-based cost trade -- there was never an
	 * economic reason for it to exist on this path, and it was silently
	 * vetoing hackbench-shaped short-CS workloads where this locus's own
	 * measured win lives. See ivh_min_cs_ns's removal (was bpf_sched.c).
	 */
	if (!in_task() || !preemptible() || current->lock_depth > 0)
		return;
	/*
	 * Only migrate a genuinely runnable task. A caller may already have
	 * done set_current_state(TASK_INTERRUPTIBLE|...) as part of an outer
	 * prepare-to-wait sequence (freezer, futex, sigtimedwait, hrtimer,
	 * wait_event_freezable, ...) and merely be taking this spinlock as
	 * bookkeeping before its own schedule(). Injecting
	 * set_cpus_allowed_ptr() + schedule() here would consume that sleep
	 * state and dequeue the task on a wakeup nobody will ever send IVH's
	 * way — a lost wakeup (on_rq=0, __state!=TASK_RUNNING, stuck
	 * indefinitely). Skipping is always safe: IVH is best-effort and
	 * retries on the task's next lock acquisition while it's running.
	 */
	if (READ_ONCE(current->__state) != TASK_RUNNING)
		return;
	/*
	 * 2026-07-07: optional selectivity gate, default OFF -- reuses locus
	 * (c)'s Hotlock table to ask "would my LHP strand real waiters on
	 * this lock" instead of gating on duration (which locus (a) never
	 * had an economic reason to do -- see the removed CS-floor comment
	 * above). Checked BEFORE the cooldown below for the same reason
	 * locus (c)'s per-lock cooldown is checked last among its cheap
	 * gates: a cold-lock attempt that was always going to be skipped
	 * shouldn't consume this vCPU's cooldown window ahead of a hot-lock
	 * attempt that arrives moments later. When locus (c) is disabled,
	 * nothing else drives the EWMA, so this samples it directly rather
	 * than reading a permanently-stale history of 0.
	 */
	if (ivh_pre_lock_hotlock_enabled) {
		int waiters = ivh_hotlock_waiters(lock);
		bool contended = waiters > 0;

		if (!ivh_post_lock_enabled)
			ivh_hotlock_update(lock, contended);
		if (!contended && ivh_hotlock_read(lock) <= IVH_HOTLOCK_HALF) {
			this_cpu_inc(ivh_prelock_hotlock_cold_skipped);
			return;
		}
		this_cpu_inc(ivh_prelock_hotlock_hot_passed);
	}
	/*
	 * Per-vCPU evaluation cooldown — see ivh_eval_cooldown_ok() in fair.c.
	 * Cuts redundant full evaluations under high lock-call-volume
	 * workloads (hackbench) without discriminating by which lock or how
	 * long it's held: the thing being gated (vCPU health) is a property
	 * of this CPU, not of this specific lock acquisition.
	 */
	if (!ivh_eval_cooldown_ok())
		return;
	bpf_sched_pre_lock_migrate();
}

static __always_inline void cs_enter(void)
{
	if (current->lock_depth == 1) {
		current->cs_start_ts = sched_clock();
		current->cs_wall_start_ts = current->cs_start_ts;
	}
}

static __always_inline void cs_exit(void)
{
	if (current->lock_depth == 0 && current->cs_start_ts) {
		u64 now = sched_clock();
		current->last_cs_ns = now - current->cs_wall_start_ts;
		current->cumulative_cs_time += now - current->cs_start_ts;
		current->cs_start_ts = 0;
		current->cs_wall_start_ts = 0;
	}
}

/*
 * ---------------------------------------------------------------------
 * Locus (c): post-acquisition, confirmed-holder migration ("Hotlock").
 *
 * Unlike ivh_pre_lock() above (which migrates a task that MIGHT become a
 * holder, before any lock is attempted -- fires blind, can't tell fast-path
 * from slow-path), this fires once the calling task is a CONFIRMED holder
 * of `lock` -- after __raw_spin_lock() has returned -- but before the
 * caller's critical section body runs. This is the only insertion point
 * that actually intervenes on real lock-holder preemption (LHP): the thing
 * IVH is fundamentally trying to prevent is the HOLDER being unavailable,
 * not a waiter being delayed. See
 * tools/bpf/docs/post_acquisition_reactive_migration_2026-07-02.md for the
 * full derivation (in particular: why this is only worth enabling once the
 * steal-prediction signal's conditional precision clears roughly 1/2000 --
 * an empirical question this code cannot answer for itself, which is why
 * ivh_post_lock_enabled defaults OFF).
 *
 * SCOPE: wired only into _raw_spin_lock()'s plain (non-irqsave/irq/bh) path,
 * below. The _irqsave/_irq/_bh wrappers keep IRQs or softirqs disabled for
 * the entire held duration; a bare preempt_enable() does not touch that
 * state (preemptible() requires BOTH preempt_count()==0 AND
 * !irqs_disabled()), so schedule() would be unsafe there. Do not wire this
 * into those wrappers without building the IRQ/BH-restore dance explicitly
 * first -- that is out of scope for this feature as built.
 * ---------------------------------------------------------------------
 */
#if defined(CONFIG_MMIOWB)
#error "Locus (c) post-acquisition IVH migration (ivh_post_lock(), this file) is not verified safe with CONFIG_MMIOWB set: mmiowb_spin_lock()/_unlock() (asm-generic/mmiowb.h) is a per-CPU nesting counter opened at acquire and closed at release via current-CPU accessors, spanning the WHOLE acquire-to-release window -- exactly the window this locus deliberately keeps open across a migration. Confirmed inert on the config this was built against (CONFIG_MMIOWB unset); do not enable on a config where it IS set without re-auditing first."
#endif

/* IVH_HOTLOCK_{BITS,SIZE,SCALE,HALF} are declared in linux/bpf_sched.h so
 * bpf_sched_post_lock_migrate() (kernel/sched/fair.c) can share the same
 * fixed-point scale for its Tier-1 threshold computation. */

struct ivh_hotlock_entry {
	const void	*lock;     /* direct-mapped key; NULL == empty bucket */
	atomic_t	history;   /* EWMA, IVH_HOTLOCK_SCALE fixed point.
				    * atomic_t (not u16) as of 2026-07-03:
				    * the original plain read-modify-write was
				    * a genuine lost-update race under
				    * concurrent updates to the same bucket --
				    * confirmed real, fixed via CAS-retry below.
				    * No struct growth: this field was already
				    * padded out to 4 bytes to align the
				    * following atomic_t waiters. */
	atomic_t	waiters;   /* live count of threads currently in the
				    * qspinlock slowpath for this bucket --
				    * see ivh_hotlock_note_waiter_enter/exit()
				    * below. NOT reset on a lock-pointer
				    * collision (unlike history/lock above):
				    * each increment is symmetric with a later
				    * decrement for the SAME real event
				    * regardless of which lock currently owns
				    * this bucket's identity fields, so
				    * resetting it here would risk driving it
				    * negative when that earlier event's
				    * decrement eventually arrives. Worst case
				    * on a bucket collision is a lock briefly
				    * looking busier than it really is -- the
				    * same class of imprecision already
				    * accepted for history/lock. */
	atomic64_t	last_eval_ns; /* 2026-07-05: per-lock cooldown for
				    * locus (c)'s EXPENSIVE decision tail
				    * (preempt_enable + trylock + BPF
				    * destination search) -- NOT checked at
				    * the Hotlock gate (that stays cheap and
				    * unthrottled), but immediately before
				    * bpf_sched_post_lock_migrate() commits to
				    * paying that cost, so it collapses "many
				    * threads all about to search for a
				    * destination for this same lock" into one
				    * payer per window, without penalizing the
				    * cheap "not urgent, bail out early" case at
				    * all -- that case never reaches this
				    * check. Must be CAS-protected, not a plain
				    * write: this field enforces a rate LIMIT,
				    * not a soft estimate, so a lost update here
				    * would let a burst through in exactly the
				    * window the cooldown exists to prevent --
				    * same failure class already found and
				    * fixed for history above. */
	u8		__pad[8];  /* 2026-07-07: pads 24B -> 32B so two entries
				    * fit exactly in one 64B cache line instead
				    * of straddling it unevenly -- otherwise
				    * roughly 1/3 of buckets split across a
				    * line boundary, false-sharing with a
				    * neighboring (logically unrelated) bucket
				    * on every access. */
};

/*
 * Lockless, best-effort table -- deliberately no locking. `history` is a
 * soft heuristic (a decaying "was this lock usually contended" estimate),
 * not a correctness-critical value: a lost update or an occasional bucket
 * collision (two different locks hashing to the same slot, causing one
 * cold-start reset) degrades precision, never correctness -- the same
 * tolerance already accepted elsewhere in this project for the BPF
 * destination-verdict cache.
 */
static struct ivh_hotlock_entry ivh_hotlock_table[IVH_HOTLOCK_SIZE] __cacheline_aligned;

/*
 * 2026-07-07: per-CPU counter for sampling ivh_hotlock_update()'s
 * NON-contended path 1-in-8 -- see the check below for why only that path
 * is sampled, not contended updates too.
 */
static DEFINE_PER_CPU(u8, ivh_hotlock_sample_counter);

static __always_inline void ivh_hotlock_update(const void *lock, bool contended)
{
	struct ivh_hotlock_entry *e = &ivh_hotlock_table[hash_ptr(lock, IVH_HOTLOCK_BITS)];
	int sample = contended ? (1 << IVH_HOTLOCK_SCALE) : 0;
	int old, new;

	/*
	 * Sample only the !contended path 1-in-8, not contended updates --
	 * real contention is already rare (~0.8% of acquisitions, measured
	 * earlier this project) and is exactly the signal Hotlock exists to
	 * catch quickly; sampling it too would slow how fast the EWMA reacts
	 * to genuine contention. The !contended path is the bulk of the 315M
	 * calls/run and, after the new==old skip above, is what's left
	 * driving repeat CAS attempts (a lock decaying back toward 0 after a
	 * past contention spike) -- coarsening its update rate doesn't change
	 * what the EWMA converges to, only how many steps it takes to get
	 * there.
	 */
	if (!contended && (this_cpu_inc_return(ivh_hotlock_sample_counter) & 7))
		return;

	if (READ_ONCE(e->lock) != lock) {
		/* Cold-start rather than inherit a stale value from whatever
		 * lock previously owned this bucket. */
		WRITE_ONCE(e->lock, lock);
		atomic_set(&e->history, sample);
		return;
	}

	/* Bounded CAS-retry, not a lock: this runs unconditionally on every
	 * eligible acquisition (not just hot ones), so it must stay off the
	 * fast path's critical path cost -- a real lock here would route
	 * every sample back through the full _raw_spin_lock() machinery
	 * recursively (safe per the existing lock_depth guards, but far more
	 * expensive than this needs to be). */
	/*
	 * 2026-07-07: skip the CAS entirely when the EWMA update is a no-op --
	 * confirmed via live audit to be the dominant cost on this path:
	 * ~88% of calls are cold locks that decay to and stay at exactly 0
	 * (sample==0, old==0 -> new==0), yet were still taking a locked RMW
	 * on this bucket's shared cache line from every CPU, every single
	 * acquisition, forever. new==old means nothing would change -- there
	 * is nothing to protect by writing it anyway.
	 */
	old = atomic_read(&e->history);
	do {
		new = old + ((sample - old) >> ivh_hotlock_ewma_k);
		if (new == old)
			return;
	} while (!atomic_try_cmpxchg(&e->history, &old, new));
}

static __always_inline u16 ivh_hotlock_read(const void *lock)
{
	struct ivh_hotlock_entry *e = &ivh_hotlock_table[hash_ptr(lock, IVH_HOTLOCK_BITS)];

	return (READ_ONCE(e->lock) == lock) ? (u16)atomic_read(&e->history) : 0;
}

/*
 * ivh_hotlock_cooldown_ok() and its ivh_post_lock_cooldown_skipped counter
 * (per-lock rate limit on locus (c)'s real migration dispatch) were removed
 * along with bpf_sched_post_lock_migrate() (kernel/sched/fair.c) -- it had
 * no other caller. The e->last_eval_ns field in struct ivh_hotlock_entry
 * above is left in place (unused) rather than resized, to avoid re-deriving
 * that struct's cache-line packing math for a field removal with zero
 * runtime benefit.
 */

/*
 * 2026-07-07: locus (a)'s Hotlock selectivity gate counters (see
 * ivh_pre_lock() above) -- measure whether the gate is actually filtering
 * cold-lock attempts, and separately whether the win this locus was
 * originally measured to have survives gating by lock value instead of by
 * CS duration. Per-CPU for the same reason as the counter above.
 */
DEFINE_PER_CPU(u64, ivh_prelock_hotlock_cold_skipped);
DEFINE_PER_CPU(u64, ivh_prelock_hotlock_hot_passed);
EXPORT_PER_CPU_SYMBOL_GPL(ivh_prelock_hotlock_cold_skipped);
EXPORT_PER_CPU_SYMBOL_GPL(ivh_prelock_hotlock_hot_passed);

/*
 * ivh_hotlock_note_waiter_enter/exit - live per-lock waiter count, called
 * from kernel/locking/qspinlock.c at the two real contention entry points
 * (pending-bit optimistic spin, full MCS queueing) and their matching exit
 * points (bracketing wait_depth++/-- exactly). Unlike history (a decaying
 * historical average, fed by a per-acquisition sample), this is a live,
 * accurate count of how many OTHER threads are queued for this exact lock
 * right now: by the time a new holder's own acquisition completes and calls
 * ivh_post_lock(), ITS OWN enter/exit pair has already balanced out (the
 * exit call happens before queued_spin_lock_slowpath() returns), so
 * whatever remains is genuinely other waiters, not an echo of its own
 * just-finished wait. Not static/inline: called across a file boundary.
 */
void ivh_hotlock_note_waiter_enter(const void *lock)
{
	struct ivh_hotlock_entry *e = &ivh_hotlock_table[hash_ptr(lock, IVH_HOTLOCK_BITS)];

	atomic_inc(&e->waiters);
}
EXPORT_SYMBOL_GPL(ivh_hotlock_note_waiter_enter);

void ivh_hotlock_note_waiter_exit(const void *lock)
{
	struct ivh_hotlock_entry *e = &ivh_hotlock_table[hash_ptr(lock, IVH_HOTLOCK_BITS)];

	atomic_dec(&e->waiters);
}
EXPORT_SYMBOL_GPL(ivh_hotlock_note_waiter_exit);

static __always_inline int ivh_hotlock_waiters(const void *lock)
{
	struct ivh_hotlock_entry *e = &ivh_hotlock_table[hash_ptr(lock, IVH_HOTLOCK_BITS)];

	return atomic_read(&e->waiters);
}

/* /proc/ivh_debug visibility (see ivh_debug_show() in fair.c). Per-CPU for
 * the same reason as ivh_post_lock_cooldown_skipped above. */
DEFINE_PER_CPU(u64, ivh_post_lock_calls);
DEFINE_PER_CPU(u64, ivh_post_lock_hot);
EXPORT_PER_CPU_SYMBOL_GPL(ivh_post_lock_calls);
EXPORT_PER_CPU_SYMBOL_GPL(ivh_post_lock_hot);

/*
 * ivh_post_lock - gate + Hotlock sample for locus (c). Called from
 * _raw_spin_lock() AFTER lock_depth has been bumped to reflect the
 * just-acquired lock (lock_depth == 1 means "this lock, nothing nested"),
 * BEFORE cs_enter() starts CS timing -- so if a migration happens below,
 * last_cs_ns measures the CS body's actual duration on the FINAL cpu, not
 * polluted by migration latency.
 */
static __always_inline void ivh_post_lock(raw_spinlock_t *lock)
{
	bool contended;
	u16 history;

	/*
	 * Step 0: must dominate cost for the ~100% of lock acquisitions
	 * system-wide that are not under test -- both checks are already
	 * proven cheap at ivh_pre_lock() above (a static_branch + flags
	 * test), and must run BEFORE the Hotlock table lookup below, not
	 * alongside/after it, since this locus lives in the generic
	 * _raw_spin_lock() path used by every lock in the tree.
	 */
	if (!bpf_sched_enabled())
		return;
	if (!(current->flags & PF_IVH_ELIGIBLE))
		return;

	/*
	 * 2026-07-07: was only checked inside bpf_sched_post_lock_migrate(),
	 * meaning "post-lock disabled" still paid for every Hotlock sample
	 * below (hash + atomic reads + the EWMA update) on every eligible
	 * acquisition -- confirmed via live audit to be polluting any
	 * disabled-vs-enabled comparison that had the BPF prog loaded. Check
	 * it here too, before any of that work happens.
	 */
	if (!ivh_post_lock_enabled)
		return;

	/*
	 * Recursion guard. lock_depth was just bumped to reflect THIS
	 * acquisition (see _raw_spin_lock(), below) -- ==1 means "the
	 * just-acquired lock, nothing else held", the only state this
	 * function evaluates. Anything this function's own slow path takes
	 * bumps lock_depth further (bpf_sched_post_lock_migrate() mirrors
	 * the identical current->lock_depth++/-- idiom
	 * bpf_sched_pre_lock_migrate() already uses for the same purpose)
	 * and would see this check fail on re-entry -- no sentinel or
	 * explicit save/restore needed, arbitrary nesting depth is already
	 * handled correctly by the existing per-wrapper increment/decrement
	 * discipline. (In practice this guard is mostly insurance: every
	 * lock the migration path itself takes today -- my_spinlock, rq
	 * locks, the stopper's queue lock -- uses an _irqsave variant, which
	 * never reaches this function at all per the scope note above. Build
	 * it anyway -- it's one integer comparison, and it's what stands
	 * between "safe" and the exact stack-overflow-via-recursion bug
	 * ivh_pre_lock() already hit once, the moment any future change
	 * introduces a plain non-irqsave lock into this path.)
	 */
	if (current->lock_depth != 1)
		return;

	this_cpu_inc(ivh_post_lock_calls);

	/*
	 * Sample source, fixed 2026-07-03 (v2): queued_spin_is_contended() (the
	 * lock word's tail/pending bits) answers "is someone else queued RIGHT
	 * NOW" -- a forward-looking, instantaneous question that reads false
	 * for nearly all real contention, confirmed by live measurement
	 * (~0.8% of acquisitions genuinely enter the contended slowpath, yet
	 * this instantaneous check read zero across 150M+ live samples),
	 * because the queue typically drains back to empty by the time the
	 * new holder finishes claiming. An interim fix sampled a per-task
	 * "did I personally wait to get here" flag instead -- better than
	 * always-zero, but still not the actual question Hotlock needs
	 * ("are there waiters right now who'd be hurt if I'm preempted"), and
	 * per-task storage can't distinguish which lock a stale flag belongs
	 * to across nested acquisitions.
	 *
	 * ivh_hotlock_waiters(lock) (below) is a live, per-LOCK count --
	 * exactly the right question, exactly the right key. This thread's
	 * own contribution has already been removed by the time this runs
	 * (ivh_hotlock_note_waiter_exit() fires before queued_spin_lock_slowpath()
	 * returns), so a nonzero read here means other threads are genuinely
	 * queued on this exact lock right now.
	 * See tools/bpf/docs/post_acquisition_reactive_migration_2026-07-02.md.
	 */
	{
		int waiters = ivh_hotlock_waiters(lock);
		bool is_hot;

		contended = waiters > 0;
		ivh_hotlock_update(lock, contended);
		history = ivh_hotlock_read(lock);
		is_hot = contended || history > IVH_HOTLOCK_HALF;

		/*
		 * Per-acquisition Hotlock trace -- lock identity, live waiter
		 * count, decaying history, which path (fast/slow) got here,
		 * and the resulting decision, so a live run can be checked
		 * against expectations rather than only seeing aggregate
		 * counters. current->ivh_slowpath is read here, not cleared --
		 * it's reset at the START of the next outermost acquisition
		 * (see _raw_spin_lock()), not at the end of this one.
		 */
		if (unlikely(ivh_trace_enabled))
			trace_printk("ivh_hotlock: lock=%px waiters=%d history=%d "
				     "path=%s decision=%s\n",
				     lock, waiters, (int)history,
				     current->ivh_slowpath ? "slow" : "fast",
				     is_hot ? "hot" : "cold");
	}

	/* Hotlock gate: dominant no-op path even for the eligible population
	 * -- most locks even THEY take aren't hot. */
	if (!contended && history <= IVH_HOTLOCK_HALF)
		return;

	this_cpu_inc(ivh_post_lock_hot);

	/*
	 * Shadow mode only, this build: locus (c)'s real migration dispatch
	 * (bpf_sched_post_lock_migrate(), kernel/sched/fair.c) has been
	 * removed entirely -- this function's job here ends at the Hotlock
	 * classification above (ivh_post_lock_calls/hot, the trace_printk
	 * above, and the live table itself) so its "would this have fired"
	 * behavior can be validated against real kernel-side traffic without
	 * ever migrating anything.
	 */
}

#ifdef CONFIG_MMIOWB
#ifndef arch_mmiowb_state
DEFINE_PER_CPU(struct mmiowb_state, __mmiowb_state);
EXPORT_PER_CPU_SYMBOL(__mmiowb_state);
#endif
#endif

/*
 * If lockdep is enabled then we use the non-preemption spin-ops
 * even on CONFIG_PREEMPT, because lockdep assumes that interrupts are
 * not re-enabled during lock-acquire (which the preempt-spin-ops do):
 */
#if !defined(CONFIG_GENERIC_LOCKBREAK) || defined(CONFIG_DEBUG_LOCK_ALLOC)
/*
 * The __lock_function inlines are taken from
 * spinlock : include/linux/spinlock_api_smp.h
 * rwlock   : include/linux/rwlock_api_smp.h
 */
#else

/*
 * Some architectures can relax in favour of the CPU owning the lock.
 */
#ifndef arch_read_relax
# define arch_read_relax(l)	cpu_relax()
#endif
#ifndef arch_write_relax
# define arch_write_relax(l)	cpu_relax()
#endif
#ifndef arch_spin_relax
# define arch_spin_relax(l)	cpu_relax()
#endif

/*
 * We build the __lock_function inlines here. They are too large for
 * inlining all over the place, but here is only one user per function
 * which embeds them into the calling _lock_function below.
 *
 * This could be a long-held lock. We both prepare to spin for a long
 * time (making _this_ CPU preemptible if possible), and we also signal
 * towards that other CPU that it should break the lock ASAP.
 */
#define BUILD_LOCK_OPS(op, locktype)					\
static void __lockfunc __raw_##op##_lock(locktype##_t *lock)		\
{									\
	for (;;) {							\
		preempt_disable();					\
		if (likely(do_raw_##op##_trylock(lock)))		\
			break;						\
		preempt_enable();					\
									\
		arch_##op##_relax(&lock->raw_lock);			\
	}								\
}									\
									\
static unsigned long __lockfunc __raw_##op##_lock_irqsave(locktype##_t *lock) \
{									\
	unsigned long flags;						\
									\
	for (;;) {							\
		preempt_disable();					\
		local_irq_save(flags);					\
		if (likely(do_raw_##op##_trylock(lock)))		\
			break;						\
		local_irq_restore(flags);				\
		preempt_enable();					\
									\
		arch_##op##_relax(&lock->raw_lock);			\
	}								\
									\
	return flags;							\
}									\
									\
static void __lockfunc __raw_##op##_lock_irq(locktype##_t *lock)	\
{									\
	_raw_##op##_lock_irqsave(lock);					\
}									\
									\
static void __lockfunc __raw_##op##_lock_bh(locktype##_t *lock)		\
{									\
	unsigned long flags;						\
									\
	/*							*/	\
	/* Careful: we must exclude softirqs too, hence the	*/	\
	/* irq-disabling. We use the generic preemption-aware	*/	\
	/* function:						*/	\
	/**/								\
	flags = _raw_##op##_lock_irqsave(lock);				\
	local_bh_disable();						\
	local_irq_restore(flags);					\
}									\

/*
 * Build preemption-friendly versions of the following
 * lock-spinning functions:
 *
 *         __[spin|read|write]_lock()
 *         __[spin|read|write]_lock_irq()
 *         __[spin|read|write]_lock_irqsave()
 *         __[spin|read|write]_lock_bh()
 */
BUILD_LOCK_OPS(spin, raw_spinlock);

#ifndef CONFIG_PREEMPT_RT
BUILD_LOCK_OPS(read, rwlock);
BUILD_LOCK_OPS(write, rwlock);
#endif

#endif

#ifndef CONFIG_INLINE_SPIN_TRYLOCK
noinline int __lockfunc _raw_spin_trylock(raw_spinlock_t *lock)
{
	int ret = __raw_spin_trylock(lock);
	if (ret && !in_interrupt()) {
		current->lock_depth++;
		cs_enter();
	}
	return ret;
}
EXPORT_SYMBOL(_raw_spin_trylock);
#endif

#ifndef CONFIG_INLINE_SPIN_TRYLOCK_BH
noinline int __lockfunc _raw_spin_trylock_bh(raw_spinlock_t *lock)
{
	bool track = !in_interrupt();
	int ret = __raw_spin_trylock_bh(lock);
	if (ret && track) {
		current->lock_depth++;
		cs_enter();
	}
	return ret;
}
EXPORT_SYMBOL(_raw_spin_trylock_bh);
#endif

#ifndef CONFIG_INLINE_SPIN_LOCK
noinline void __lockfunc _raw_spin_lock(raw_spinlock_t *lock)
{
	/*
	 * ivh_slowpath is a sticky per-task flag, not per-lock storage -- it
	 * only means "this acquisition took the slow path" if it's guaranteed
	 * clean before the acquisition starts. Without this reset, a NESTED
	 * lock's slowpath entry (lock_depth > 1, invisible to ivh_post_lock()'s
	 * lock_depth==1 guard) could leave it set and get misattributed to a
	 * later, unrelated outer lock. Only reset at the start of a genuinely
	 * new outermost attempt (lock_depth==0) -- same fix already applied
	 * once for this class of hazard.
	 */
	if (!in_interrupt() && current->lock_depth == 0)
		current->ivh_slowpath = false;
	/*
	 * ivh_pre_lock() (locus a) re-enabled 2026-07-07 -- was disabled
	 * 2026-07-05 for A/B isolation while post-lock was evaluated on its
	 * own; that evaluation is done (post-lock confirmed safe and at
	 * parity with no-IVH under real contention, with a real but bounded
	 * safe operating range). Re-disable by re-commenting all four call
	 * sites in this file if isolated post-lock testing is needed again.
	 */
	ivh_pre_lock(lock);
	__raw_spin_lock(lock);
	if (!in_interrupt()) {
		current->lock_depth++;
		ivh_post_lock(lock);
		cs_enter();
	}
}
EXPORT_SYMBOL(_raw_spin_lock);
#endif

#ifndef CONFIG_INLINE_SPIN_LOCK_IRQSAVE
noinline unsigned long __lockfunc _raw_spin_lock_irqsave(raw_spinlock_t *lock)
{
	unsigned long flags;

	ivh_pre_lock(lock);
	flags = __raw_spin_lock_irqsave(lock);
	if (!in_interrupt()) {
		current->lock_depth++;
		cs_enter();
	}
	return flags;
}
EXPORT_SYMBOL(_raw_spin_lock_irqsave);
#endif

#ifndef CONFIG_INLINE_SPIN_LOCK_IRQ
noinline void __lockfunc _raw_spin_lock_irq(raw_spinlock_t *lock)
{
	ivh_pre_lock(lock);
	__raw_spin_lock_irq(lock);
	if (!in_interrupt()) {
		current->lock_depth++;
		cs_enter();
	}
}
EXPORT_SYMBOL(_raw_spin_lock_irq);
#endif

#ifndef CONFIG_INLINE_SPIN_LOCK_BH
noinline void __lockfunc _raw_spin_lock_bh(raw_spinlock_t *lock)
{
	bool track = !in_interrupt();
	ivh_pre_lock(lock);
	__raw_spin_lock_bh(lock);
	if (track) {
		current->lock_depth++;
		cs_enter();
	}
}
EXPORT_SYMBOL(_raw_spin_lock_bh);
#endif

#ifdef CONFIG_UNINLINE_SPIN_UNLOCK
noinline void __lockfunc _raw_spin_unlock(raw_spinlock_t *lock)
{
	if (!in_interrupt()) {
		current->lock_depth--;
		cs_exit();
	}
	__raw_spin_unlock(lock);
}
EXPORT_SYMBOL(_raw_spin_unlock);
#endif

#ifndef CONFIG_INLINE_SPIN_UNLOCK_IRQRESTORE
noinline void __lockfunc _raw_spin_unlock_irqrestore(raw_spinlock_t *lock, unsigned long flags)
{
	if (!in_interrupt()) {
		current->lock_depth--;
		cs_exit();
	}
	__raw_spin_unlock_irqrestore(lock, flags);
}
EXPORT_SYMBOL(_raw_spin_unlock_irqrestore);
#endif

#ifndef CONFIG_INLINE_SPIN_UNLOCK_IRQ
noinline void __lockfunc _raw_spin_unlock_irq(raw_spinlock_t *lock)
{
	if (!in_interrupt()) {
		current->lock_depth--;
		cs_exit();
	}
	__raw_spin_unlock_irq(lock);
}
EXPORT_SYMBOL(_raw_spin_unlock_irq);
#endif

#ifndef CONFIG_INLINE_SPIN_UNLOCK_BH
noinline void __lockfunc _raw_spin_unlock_bh(raw_spinlock_t *lock)
{
	__raw_spin_unlock_bh(lock);
	/* decrement after unlock; cs_exit measures slightly past actual release */
	if (!in_interrupt()) {
		current->lock_depth--;
		cs_exit();
	}
}
EXPORT_SYMBOL(_raw_spin_unlock_bh);
#endif

#ifndef CONFIG_PREEMPT_RT

#ifndef CONFIG_INLINE_READ_TRYLOCK
noinline int __lockfunc _raw_read_trylock(rwlock_t *lock)
{
	return __raw_read_trylock(lock);
}
EXPORT_SYMBOL(_raw_read_trylock);
#endif

#ifndef CONFIG_INLINE_READ_LOCK
noinline void __lockfunc _raw_read_lock(rwlock_t *lock)
{
	__raw_read_lock(lock);
}
EXPORT_SYMBOL(_raw_read_lock);
#endif

#ifndef CONFIG_INLINE_READ_LOCK_IRQSAVE
noinline unsigned long __lockfunc _raw_read_lock_irqsave(rwlock_t *lock)
{
	return __raw_read_lock_irqsave(lock);
}
EXPORT_SYMBOL(_raw_read_lock_irqsave);
#endif

#ifndef CONFIG_INLINE_READ_LOCK_IRQ
noinline void __lockfunc _raw_read_lock_irq(rwlock_t *lock)
{
	__raw_read_lock_irq(lock);
}
EXPORT_SYMBOL(_raw_read_lock_irq);
#endif

#ifndef CONFIG_INLINE_READ_LOCK_BH
noinline void __lockfunc _raw_read_lock_bh(rwlock_t *lock)
{
	__raw_read_lock_bh(lock);
}
EXPORT_SYMBOL(_raw_read_lock_bh);
#endif

#ifndef CONFIG_INLINE_READ_UNLOCK
noinline void __lockfunc _raw_read_unlock(rwlock_t *lock)
{
	__raw_read_unlock(lock);
}
EXPORT_SYMBOL(_raw_read_unlock);
#endif

#ifndef CONFIG_INLINE_READ_UNLOCK_IRQRESTORE
noinline void __lockfunc _raw_read_unlock_irqrestore(rwlock_t *lock, unsigned long flags)
{
	__raw_read_unlock_irqrestore(lock, flags);
}
EXPORT_SYMBOL(_raw_read_unlock_irqrestore);
#endif

#ifndef CONFIG_INLINE_READ_UNLOCK_IRQ
noinline void __lockfunc _raw_read_unlock_irq(rwlock_t *lock)
{
	__raw_read_unlock_irq(lock);
}
EXPORT_SYMBOL(_raw_read_unlock_irq);
#endif

#ifndef CONFIG_INLINE_READ_UNLOCK_BH
noinline void __lockfunc _raw_read_unlock_bh(rwlock_t *lock)
{
	__raw_read_unlock_bh(lock);
}
EXPORT_SYMBOL(_raw_read_unlock_bh);
#endif

#ifndef CONFIG_INLINE_WRITE_TRYLOCK
noinline int __lockfunc _raw_write_trylock(rwlock_t *lock)
{
	return __raw_write_trylock(lock);
}
EXPORT_SYMBOL(_raw_write_trylock);
#endif

#ifndef CONFIG_INLINE_WRITE_LOCK
noinline void __lockfunc _raw_write_lock(rwlock_t *lock)
{
	__raw_write_lock(lock);
}
EXPORT_SYMBOL(_raw_write_lock);

#ifndef CONFIG_DEBUG_LOCK_ALLOC
#define __raw_write_lock_nested(lock, subclass)	__raw_write_lock(((void)(subclass), (lock)))
#endif

void __lockfunc _raw_write_lock_nested(rwlock_t *lock, int subclass)
{
	__raw_write_lock_nested(lock, subclass);
}
EXPORT_SYMBOL(_raw_write_lock_nested);
#endif

#ifndef CONFIG_INLINE_WRITE_LOCK_IRQSAVE
noinline unsigned long __lockfunc _raw_write_lock_irqsave(rwlock_t *lock)
{
	return __raw_write_lock_irqsave(lock);
}
EXPORT_SYMBOL(_raw_write_lock_irqsave);
#endif

#ifndef CONFIG_INLINE_WRITE_LOCK_IRQ
noinline void __lockfunc _raw_write_lock_irq(rwlock_t *lock)
{
	__raw_write_lock_irq(lock);
}
EXPORT_SYMBOL(_raw_write_lock_irq);
#endif

#ifndef CONFIG_INLINE_WRITE_LOCK_BH
noinline void __lockfunc _raw_write_lock_bh(rwlock_t *lock)
{
	__raw_write_lock_bh(lock);
}
EXPORT_SYMBOL(_raw_write_lock_bh);
#endif

#ifndef CONFIG_INLINE_WRITE_UNLOCK
noinline void __lockfunc _raw_write_unlock(rwlock_t *lock)
{
	__raw_write_unlock(lock);
}
EXPORT_SYMBOL(_raw_write_unlock);
#endif

#ifndef CONFIG_INLINE_WRITE_UNLOCK_IRQRESTORE
noinline void __lockfunc _raw_write_unlock_irqrestore(rwlock_t *lock, unsigned long flags)
{
	__raw_write_unlock_irqrestore(lock, flags);
}
EXPORT_SYMBOL(_raw_write_unlock_irqrestore);
#endif

#ifndef CONFIG_INLINE_WRITE_UNLOCK_IRQ
noinline void __lockfunc _raw_write_unlock_irq(rwlock_t *lock)
{
	__raw_write_unlock_irq(lock);
}
EXPORT_SYMBOL(_raw_write_unlock_irq);
#endif

#ifndef CONFIG_INLINE_WRITE_UNLOCK_BH
noinline void __lockfunc _raw_write_unlock_bh(rwlock_t *lock)
{
	__raw_write_unlock_bh(lock);
}
EXPORT_SYMBOL(_raw_write_unlock_bh);
#endif

#endif /* !CONFIG_PREEMPT_RT */

#ifdef CONFIG_DEBUG_LOCK_ALLOC

void __lockfunc _raw_spin_lock_nested(raw_spinlock_t *lock, int subclass)
{
	preempt_disable();
	spin_acquire(&lock->dep_map, subclass, 0, _RET_IP_);
	LOCK_CONTENDED(lock, do_raw_spin_trylock, do_raw_spin_lock);
}
EXPORT_SYMBOL(_raw_spin_lock_nested);

unsigned long __lockfunc _raw_spin_lock_irqsave_nested(raw_spinlock_t *lock,
						   int subclass)
{
	unsigned long flags;

	local_irq_save(flags);
	preempt_disable();
	spin_acquire(&lock->dep_map, subclass, 0, _RET_IP_);
	LOCK_CONTENDED(lock, do_raw_spin_trylock, do_raw_spin_lock);
	return flags;
}
EXPORT_SYMBOL(_raw_spin_lock_irqsave_nested);

void __lockfunc _raw_spin_lock_nest_lock(raw_spinlock_t *lock,
				     struct lockdep_map *nest_lock)
{
	preempt_disable();
	spin_acquire_nest(&lock->dep_map, 0, 0, nest_lock, _RET_IP_);
	LOCK_CONTENDED(lock, do_raw_spin_trylock, do_raw_spin_lock);
}
EXPORT_SYMBOL(_raw_spin_lock_nest_lock);

#endif

notrace int in_lock_functions(unsigned long addr)
{
	/* Linker adds these: start and end of __lockfunc functions */
	extern char __lock_text_start[], __lock_text_end[];

	return addr >= (unsigned long)__lock_text_start
	&& addr < (unsigned long)__lock_text_end;
}
EXPORT_SYMBOL(in_lock_functions);

#if defined(CONFIG_PROVE_LOCKING) && defined(CONFIG_PREEMPT_RT)
void notrace lockdep_assert_in_softirq_func(void)
{
	lockdep_assert_in_softirq();
}
EXPORT_SYMBOL(lockdep_assert_in_softirq_func);
#endif
