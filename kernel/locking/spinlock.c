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
#include <linux/log2.h>
#include <linux/bpf_sched.h>

/*
 * IVH critical-section stamp (Build 1, tools/bpf/docs/
 * ivh_tsc_full_redesign_build_plan_2026-07-29.md sec 3.2).
 *
 * The storage, the knobs and the counters all live in arch/x86/kernel/kvm.c
 * inside its CONFIG_PARAVIRT_SPINLOCKS block, which is the same home the TSC
 * heartbeat's counters already use and the same guard /proc/ivh_debug already
 * reads them under.  This file, by contrast, is built on every SMP
 * architecture, so the include and every use of it is guarded and the
 * non-x86/non-KVM build gets no-op stubs -- the same "arch may override"
 * shape kernel/sched/sched.h already applies to ivh_tsc_beat_publish().
 */
#if defined(CONFIG_X86) && defined(CONFIG_KVM_GUEST) && \
    defined(CONFIG_PARAVIRT_SPINLOCKS)
#include <asm/ivh_tsc_beat.h>
#define IVH_HAVE_CS_BEAT 1
#endif

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
 * ---------------------------------------------------------------------
 * Hot Threads: per-task contention/preemption classifier (see
 * linux/bpf_sched.h). Replaces the earlier per-lock "Hotlock" design.
 *
 * Two independent, event-driven EWMAs live directly on task_struct
 * (ivh_wait_decay, ivh_preempt_decay — see include/linux/sched.h) rather
 * than in a shared hashed table: each task is the sole writer of its own
 * counters, so plain READ_ONCE/WRITE_ONCE replaces the atomic CAS-retry
 * loop the per-lock table needed. A thread is classified "hot" (worth an
 * IVH migration) only if it BOTH contends real locks (ivh_wait_decay) AND
 * has actually been caught by host steal mid-critical-section
 * (ivh_preempt_decay) — an AND gate. Either signal alone is a false
 * positive: busy-but-never-stolen wastes a migration on a thread nobody
 * strands, and stolen-but-uncontended (a solo lock holder) strands nobody
 * regardless of its own preemption. Validated in userspace against a real
 * pinning experiment (hotthreads_test.c) before being ported here.
 * ---------------------------------------------------------------------
 */

/* Pre-lock gate counters, surfaced via /proc/ivh_debug (fair.c). */
DEFINE_PER_CPU(u64, ivh_prelock_coldthread_skipped);
DEFINE_PER_CPU(u64, ivh_prelock_hotthread_passed);
EXPORT_PER_CPU_SYMBOL_GPL(ivh_prelock_coldthread_skipped);
EXPORT_PER_CPU_SYMBOL_GPL(ivh_prelock_hotthread_passed);

/*
 * Observe-only stats (ivh_observe, see cs_exit() below and PR_SET_IVH_ELIGIBLE
 * in kernel/sys.c). Fed regardless of migration eligibility -- lets ivh_exec
 * -v measure a process's real host-preemption exposure whether or not IVH is
 * actually protecting it (-v -n). Surfaced via /proc/ivh_debug (fair.c).
 */
DEFINE_PER_CPU(u64, ivh_obs_total_holds);
DEFINE_PER_CPU(u64, ivh_obs_stolen_holds);
EXPORT_PER_CPU_SYMBOL_GPL(ivh_obs_total_holds);
EXPORT_PER_CPU_SYMBOL_GPL(ivh_obs_stolen_holds);

/*
 * Observe-only timing stats, same gate (ivh_observe) and same posture as the
 * hold counters above -- always compiled, never sysctl-gated, fed regardless
 * of migration eligibility. Two halves of the same story, so ivh_exec -v can
 * show what a mechanism actually changed:
 *
 *   ivh_obs_cs_time_total_ns  - time SPENT HOLDING a raw spinlock, summed
 *     over outermost holds. Denominator is ivh_obs_total_holds (every
 *     observed hold contributes, contended or not). Fed from cs_exit()
 *     below, reusing the delta cumulative_cs_time already computes -- no
 *     extra sched_clock() read.
 *   ivh_obs_wait_total_ns     - time spent WAITING to acquire, summed over
 *     contended acquisitions only. Fed from the qspinlock slowpath
 *     (kernel/locking/qspinlock.c), which is where genuine contention is
 *     first observable; its own denominator is ivh_obs_wait_events, NOT
 *     ivh_obs_total_holds -- see the comment there for why the uncontended
 *     fast path is excluded from the average rather than counted as zero.
 *
 * Defined here rather than in qspinlock.c so all five ivh_obs_* counters
 * live together and are declared as one group in linux/bpf_sched.h;
 * qspinlock.c is also conditional on CONFIG_QUEUED_SPINLOCKS, this file is
 * not, and fair.c's /proc/ivh_debug reader is unconditional.
 */
DEFINE_PER_CPU(u64, ivh_obs_cs_time_total_ns);
DEFINE_PER_CPU(u64, ivh_obs_wait_total_ns);
DEFINE_PER_CPU(u64, ivh_obs_wait_events);
EXPORT_PER_CPU_SYMBOL_GPL(ivh_obs_cs_time_total_ns);
EXPORT_PER_CPU_SYMBOL_GPL(ivh_obs_wait_total_ns);
EXPORT_PER_CPU_SYMBOL_GPL(ivh_obs_wait_events);

/*
 * Observe-only CS-hold-time DISTRIBUTION, same gate (ivh_observe), same feed
 * site (cs_exit()) and same reused cs_ns delta as ivh_obs_cs_time_total_ns
 * above -- no second sched_clock() read.
 *
 * Why a histogram at all: a running sum+count can only ever produce a mean,
 * and the mean is exactly the wrong statistic here. Real workloads
 * (hackbench/ebizzy/dbench) average 120-295ns per hold, but the holds that
 * matter to IVH are the ones caught by host steal, which run three or more
 * orders of magnitude longer (IVH_HOT_STEAL_FLOOR_NS alone is 100us). Those
 * are a fraction of a percent of holds, so they are invisible in the average
 * and only show up in the tail. p90/p95 need the shape of the distribution,
 * not its first moment.
 *
 * Why log2 bucketing: it is the cheapest possible way to bin a duration in a
 * hot path -- ilog2() on x86 is a single BSR/LZCNT, no division, no floating
 * point, no per-sample storage, and the fixed 32-entry array means the whole
 * thing is O(1) space per CPU. The cost is resolution: a percentile can only
 * be reported as the power-of-2 RANGE that contains it, never an exact
 * nanosecond value. That approximation is standard and is what every
 * histogram-based percentile carries (Prometheus histogram_quantile, HDR
 * histogram, bpftrace's hist()); ivh_exec -v states it in its output.
 *
 * Bucket layout, IVH_OBS_CS_HIST_BUCKETS entries, index i:
 *   i == 0                : 0-1 ns       (ilog2(1) == 0, and cs_ns == 0 is
 *                                         folded in here -- see cs_exit())
 *   0 < i < 31            : 2^i .. 2^(i+1)-1 ns
 *   i == 31 (top bucket)  : >= 2^31 ns (~2.1s), SATURATING -- anything
 *                           longer lands here rather than running off the
 *                           end of the array
 * So bucket 23 is 8.4ms-16.7ms: 32 buckets covers nanoseconds through
 * multi-second holds, which is well past both the ~300ns common case and the
 * 100us steal floor, with room for genuine pathological outliers (a hold that
 * spans several host scheduling quanta) instead of piling them into a
 * meaningless "overflow" count.
 *
 * Kernel side exposes the RAW per-bucket counts only (/proc/ivh_debug, see
 * fair.c); walking the cumulative distribution to find p90/p95 is done in
 * userspace by ivh_exec.c, consistent with how the existing averages are
 * computed there from raw sums rather than in the kernel.
 */
DEFINE_PER_CPU(u64, ivh_obs_cs_hist[IVH_OBS_CS_HIST_BUCKETS]);
EXPORT_PER_CPU_SYMBOL_GPL(ivh_obs_cs_hist);

/*
 * ivh_hot_note_wait_event - per-task contended-wait EWMA feed, called from
 * kernel/locking/qspinlock.c's slowpath enter points (pending-bit spin,
 * MCS queue). Not static/inline: called across a file boundary. Single
 * writer (current), so a plain compute-and-store replaces the per-lock
 * table's atomic CAS loop.
 */
void ivh_hot_note_wait_event(void)
{
	unsigned int k = (unsigned int)READ_ONCE(ivh_hot_threads_ewma_k);
	int old = READ_ONCE(current->ivh_wait_decay);
	int sample = 1 << IVH_HOTLOCK_SCALE;	/* contended wait = 1.0 */

	if (k > 15)
		k = 15;
	WRITE_ONCE(current->ivh_wait_decay, (u16)(old + ((sample - old) >> k)));
}
EXPORT_SYMBOL_GPL(ivh_hot_note_wait_event);

/*
 * ivh_hot_preempt_update - per-task preempt-during-CS EWMA feed, called
 * from cs_exit() below for every outermost eligible CS.
 *
 * Asymmetric cooldown: a real catch (stolen==true) uses a FAST shift
 * (ivh_hot_preempt_ewma_k_rise, default 3 -- one catch still spikes the
 * EWMA to SCALE*2^-3=128, comfortably above the 40 threshold, so real
 * exposure keeps registering immediately). A clean release (stolen==false)
 * uses a separate, SLOWER shift (ivh_hot_preempt_ewma_k_fall, default 8).
 * This is deliberately NOT the same constant in both directions -- a
 * symmetric EWMA (one shared k) was measured to cause a severe live
 * regression (2.1% -> 15-17% host-preempted-CS): successful IVH protection
 * suppresses the very catch evidence preempt_decay needs to stay above
 * threshold, so it decays back down, protection lapses, a real catch
 * happens, it spikes back up, and the cycle repeats. Decoupling rise from
 * fall raises the protected-state equilibrium (E_prot = SCALE / (1 +
 * ((1-p)/p) * 2^-(k_fall-k_rise)), p = the thread's true catch rate) high
 * enough to clear the threshold while still genuinely tracking p -- unlike
 * a permanent latch, a thread whose real behavior later goes cold still
 * lapses back out, just on a slower, self-adjusting timescale (~2^k_fall
 * holds) instead of every single release. Validated in userspace
 * (hotthreads_cooldown_test.c): k_rise=3/k_fall=8 restored host-preempted-CS
 * to the no-gate baseline while lapsing within ~700 holds once a thread
 * genuinely stopped being caught, versus a permanent latch which never
 * lapsed at all.
 */
static __always_inline void ivh_hot_preempt_update(bool stolen)
{
	unsigned int k = stolen ? (unsigned int)READ_ONCE(ivh_hot_preempt_ewma_k_rise)
				 : (unsigned int)READ_ONCE(ivh_hot_preempt_ewma_k_fall);
	int old = READ_ONCE(current->ivh_preempt_decay);
	int sample = stolen ? (1 << IVH_HOTLOCK_SCALE) : 0;

	if (k > 15)
		k = 15;
	WRITE_ONCE(current->ivh_preempt_decay, (u16)(old + ((sample - old) >> k)));
}

/*
 * ivh_hot_wait_decay_sample_zero - occasional (1-in-N release events)
 * 0-sample for wait_decay, so it reflects RECENT contention rather than
 * lifetime contention. wait_decay's SCALE-samples come only from actual
 * contended-wait events (qspinlock.c); without an occasional 0-feed it can
 * only ever climb, so a thread that contended briefly (e.g. a daemon's
 * startup) and has since been idle stays permanently above threshold.
 * Piggybacks on cs_exit()'s existing ivh_universal_eligible-gated block
 * (which already runs for every outermost release of an eligible task)
 * rather than adding a new touch point on the uncontended fastpath -- same
 * 1-in-N coarsening trick the old per-lock Hotlock table used for its own
 * non-contended sample path.
 */
static __always_inline void ivh_hot_wait_decay_sample_zero(void)
{
	unsigned int n = (unsigned int)READ_ONCE(ivh_hot_wait_zero_n);
	unsigned int k;
	int old;

	if (n == 0)
		n = 1;
	if (++current->ivh_wait_zero_ctr < n)
		return;
	current->ivh_wait_zero_ctr = 0;

	k = (unsigned int)READ_ONCE(ivh_hot_threads_ewma_k);
	if (k > 15)
		k = 15;
	old = READ_ONCE(current->ivh_wait_decay);
	WRITE_ONCE(current->ivh_wait_decay, (u16)(old - (old >> k)));
}

/*
 * ivh_pre_lock — IVH gate + synchronous self-migration, called BEFORE
 * __raw_spin_lock*() so that no MCS node has been allocated yet and
 * preemption is still enabled.  A no-op when IVH is not loaded (static key).
 * Must not be called from trylock paths (they don't block) or when already
 * holding a spinlock (lock_depth > 0 means preemption is already off).
 */
static __always_inline void ivh_pre_lock(raw_spinlock_t *lock)
{
	if (!bpf_sched_enabled())
		return;
	/* 2026-07-20: PF_IVH_ELIGIBLE no longer consulted here --
	 * ivh_universal_eligible is the sole gate now, not an ivh_exec-wrapper
	 * opt-in. if (!(current->flags & PF_IVH_ELIGIBLE) && ...)
	 * ivh_exclude (per-task opt-out, since the global switch alone can't
	 * exclude one process) added same day. */
	if (!READ_ONCE(ivh_universal_eligible) || current->ivh_exclude)
		return;
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
	 * Hot Threads selectivity gate, default OFF (ivh_hot_threads_enabled).
	 * A thread is worth evaluating for IVH migration if it contends real
	 * locks (ivh_wait_decay) -- "would my LHP strand anyone." That's the
	 * only condition checked by default: ivh_preempt_decay (was my thread
	 * actually caught by real host steal mid-hold) is deliberately NOT
	 * consulted unless ivh_hot_preempt_gate_enabled is set, because it's
	 * fed from real KERNEL raw-spinlock holds (cs_enter/cs_exit), which
	 * are sub-microsecond -- host steal arrives in ~5-6ms chunks, so it
	 * essentially never lands inside one of these holds by chance. Live
	 * measurement: ivh_preempt_decay read exactly 0 for 90%+ of eligible
	 * evaluations, causing a severe regression (2% -> 12%+ host-preempted)
	 * when it gated migrations, confirmed unfixable by threshold tuning.
	 * "Is my vCPU actually in danger right now" doesn't need re-answering
	 * here anyway: bpf_sched_pre_lock_migrate() below already runs its own
	 * real-time Gate 1+2 (ivh_steal_imminent(), fresh per-CPU cpu_capacity
	 * + time-left) before committing to a migration -- a contended-but-
	 * currently-safe thread costs a declined evaluation there, not a
	 * wasted migration. Validated in userspace closed-loop simulation
	 * (hotthreads_twostage_test.c): wait-only + downstream Gate 1+2
	 * restores host-preempted-CS to the no-gate baseline across a full
	 * Gate-1+2-quality sensitivity sweep, while ivh_preempt_decay's own
	 * false-positive exclusions (a single-owner, never-contended lock)
	 * are already covered by ivh_wait_decay alone -- it never generates a
	 * contended-wait event, so it's excluded regardless of preempt_decay.
	 * Checked BEFORE the per-vCPU cooldown so a cold thread never consumes
	 * this vCPU's cooldown window ahead of a genuinely hot one.
	 */
	if (READ_ONCE(ivh_hot_threads_enabled)) {
		bool wait_hot;

		/*
		 * Wait-side hot test, with an optional sticky one-way latch
		 * (ivh_hot_wait_latch_enabled, default OFF -- when off this is
		 * bit-for-bit the original `wait_decay > threshold` test). The
		 * latch fixes the live-measured failure mode where a sustained
		 * lock-holder's wait_decay sits at/below the fixed threshold
		 * (its distribution mode is BELOW 512=HALF) and the 1-in-N
		 * 0-sample keeps dragging it back down, so a static threshold
		 * rejects most of a genuinely hot thread's acquisitions. Once a
		 * task has EVER crossed the threshold it stays hot for life; a
		 * task that never contends (wait_decay == 0) never arms, so
		 * idle/cold exclusion is unchanged.
		 */
		if (READ_ONCE(ivh_hot_wait_latch_enabled) &&
		    READ_ONCE(current->ivh_wait_latched)) {
			wait_hot = true;
		} else if (READ_ONCE(current->ivh_wait_decay) > ivh_hot_wait_threshold) {
			wait_hot = true;
			if (READ_ONCE(ivh_hot_wait_latch_enabled))
				WRITE_ONCE(current->ivh_wait_latched, 1);
		} else {
			wait_hot = false;
		}

		if (!wait_hot ||
		    (READ_ONCE(ivh_hot_preempt_gate_enabled) &&
		     READ_ONCE(current->ivh_preempt_decay) <= ivh_hot_preempt_threshold)) {
			this_cpu_inc(ivh_prelock_coldthread_skipped);
			return;
		}
		this_cpu_inc(ivh_prelock_hotthread_passed);
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

/*
 * cs_enter()/cs_exit() take the lock pointer as of Build 1 (build plan sec
 * 3.2.2).  All ten wrapper call sites already have `lock` in scope, so this
 * costs nothing at any of them, and it is what makes the pointer-matched
 * stamp clear below possible at all -- see task_struct::cs_beat_lock in
 * <linux/sched.h> for the _raw_spin_unlock_bh() ordering hole it closes.
 * The argument is unused when the CS stamp is not compiled in.
 */
static __always_inline void cs_enter(raw_spinlock_t *lock)
{
	if (current->lock_depth == 1) {
#ifdef IVH_HAVE_CS_BEAT
		/*
		 * Publish this CPU's CS stamp for the whole outermost hold.
		 *
		 * Gated on ivh_cs_preempt_src so the default build path is
		 * bit-for-bit unchanged: at src == 0 this is one READ_ONCE of a
		 * read-mostly global and one predicted branch, and no rdtsc.
		 *
		 * PLACED HERE DELIBERATELY, ACCEPTING FOUR KNOWN LIMITATIONS.
		 * cs_enter() runs only at lock_depth == 1 (outermost holds
		 * only), only from !in_interrupt() call sites, and only for
		 * IVH-eligible or observed tasks; and _raw_spin_unlock_bh()
		 * clears late.  For holder IDENTITY all four are disqualifying
		 * and that is why identity lives at the qspinlock layer instead
		 * (<linux/ivh_lock_holder.h>).  For the STAMP, three of the four
		 * are merely coverage gaps: an unstamped holder reads
		 * stamp == 0, which ivh_cs_age() turns into "not in a CS" and
		 * every consumer turns into "don't act" -- the safe direction.
		 * Only the fourth is a live wrong answer, and the pointer match
		 * in cs_exit() is what neutralises it.
		 *
		 * NOTE the eligibility gate below is deliberately NOT applied
		 * to the stamp.  The CPU whose hold a remote queue head needs
		 * to reason about is very often running ineligible code, and a
		 * stamp that only exists for IVH-managed tasks would answer
		 * "not in a critical section" for most of the kernel's real
		 * holds -- a systematically false negative rather than a gap.
		 */
		if (unlikely(READ_ONCE(ivh_cs_preempt_src))) {
			ivh_cs_beat_publish();
			current->cs_beat_lock = lock;
			this_cpu_inc(ivh_cs_publishes);
		}
#endif
		current->cs_start_ts = sched_clock();
		current->cs_wall_start_ts = current->cs_start_ts;
		/* 2026-07-20: PF_IVH_ELIGIBLE no longer consulted here --
		 * ivh_universal_eligible is the sole eligibility gate now, not
		 * an ivh_exec-wrapper opt-in (ivh_observe is separate, for the
		 * -v stats tool, and stays, unaffected by ivh_exclude -- an
		 * excluded task can still be observed).
		 * if (current->flags & PF_IVH_ELIGIBLE) || */
		if ((READ_ONCE(ivh_universal_eligible) && !current->ivh_exclude) ||
		    current->ivh_observe)
			current->cs_steal_start = ivh_this_cpu_steal_ns();
	}
}

static __always_inline void cs_exit(raw_spinlock_t *lock)
{
	if (current->lock_depth == 0 && current->cs_start_ts) {
		u64 now = sched_clock();
		/* Hold length of this outermost CS, computed once and reused by
		 * both cumulative_cs_time and the observe-only sum below. */
		u64 cs_ns = now - current->cs_start_ts;

#ifdef IVH_HAVE_CS_BEAT
		if (unlikely(READ_ONCE(ivh_cs_preempt_src))) {
			unsigned int bucket;

			/*
			 * Clear ONLY IF THE RECORDED LOCK STILL MATCHES.
			 *
			 * _raw_spin_unlock_bh() runs this after the real
			 * unlock (in-tree comment at that call site), so by the
			 * time we get here this CPU may already be inside a
			 * different critical section that has published its own
			 * stamp over ours.  An unconditional clear would wipe
			 * that live stamp and make a genuinely held lock read
			 * as "nobody is in a critical section" -- a live wrong
			 * answer, not a stale one, and precisely the failure
			 * mode a remote queue head cannot detect.
			 *
			 * ivh_cs_clear_mismatch is the direct measurement of
			 * how often that ordering actually bites.  It has never
			 * been measured in this tree; if it turns out to be
			 * common, that is a Build 2 finding about where the
			 * stamp belongs, not a Build 1 failure.
			 *
			 * LIVE-FLIP CORNER, stated so it is not rediscovered as
			 * a bug: writing ivh_cs_preempt_src 1 -> 0 while a hold
			 * is in flight skips this clear, leaving one stale
			 * non-zero stamp behind on that CPU.  Nothing reads it
			 * at src == 0, and the next outermost acquire on that
			 * CPU republishes it, so it can only survive on a CPU
			 * that then takes no locks at all.  Predicate form 1 is
			 * immune regardless (it also requires the liveness
			 * heartbeat to be stale, and the tick keeps that
			 * fresh); form 0 could read one such CPU as preempted
			 * until its next acquire.  If a sweep ever needs to be
			 * airtight about this, flip src to 0, wait a tick, and
			 * only then read -- do not add a clear here, which
			 * would cost a branch on every release forever to fix a
			 * transient that only exists across a sysctl write.
			 */
			if (current->cs_beat_lock == (void *)lock) {
				ivh_cs_beat_clear();
				current->cs_beat_lock = NULL;
			} else {
				this_cpu_inc(ivh_cs_clear_mismatch);
			}

			/*
			 * The POPULATION-CORRECT CS-hold-time distribution, and
			 * the reason it is a second histogram rather than a
			 * reuse of ivh_obs_cs_hist below.
			 *
			 * ivh_obs_cs_hist is incremented only under
			 * current->ivh_observe, i.e. only for tasks launched
			 * under `ivh_exec -v`.  Calibrating the CS-stamp
			 * threshold from its p99.9 would therefore calibrate
			 * against the BENCHMARK's critical-section distribution
			 * rather than the kernel's -- which is exactly the
			 * mistake this histogram exists to prevent.  This one
			 * is fed from every outermost release that gets here,
			 * gated only on ivh_cs_preempt_src.
			 *
			 * Same log2 bucketing, same clamping at both ends, same
			 * "raw counts only, percentiles are computed in
			 * userspace off a before/after delta" contract as
			 * ivh_obs_cs_hist: a lifetime percentile computed in
			 * the kernel would be wrong for a scoped run.
			 */
			bucket = cs_ns ? ilog2(cs_ns) : 0;
			if (bucket >= IVH_CS_HOLD_HIST_BUCKETS)
				bucket = IVH_CS_HOLD_HIST_BUCKETS - 1;
			this_cpu_inc(ivh_cs_hold_hist[bucket]);
		}
#endif
		current->last_cs_ns = now - current->cs_wall_start_ts;
		current->cumulative_cs_time += cs_ns;
		current->cs_start_ts = 0;
		current->cs_wall_start_ts = 0;
		/*
		 * Hot Threads: preempt_decay feed. Between the outermost
		 * cs_enter() and this cs_exit() a raw spinlock is held, so
		 * preemption is disabled and this task cannot migrate vCPUs —
		 * cs_steal_start and this read are guaranteed same-vCPU. The
		 * only thing that can advance the counter in that window is
		 * exactly the host steal we want to detect.
		 */
		/* 2026-07-20: PF_IVH_ELIGIBLE no longer consulted in either
		 * condition below -- ivh_universal_eligible is the sole
		 * eligibility gate now, not an ivh_exec-wrapper opt-in
		 * (ivh_observe is separate, for the -v stats tool, and stays,
		 * unaffected by ivh_exclude -- an excluded task can still be
		 * observed). if ((current->flags & PF_IVH_ELIGIBLE) || ...) */
		if ((READ_ONCE(ivh_universal_eligible) && !current->ivh_exclude) ||
		    current->ivh_observe) {
			u64 steal_now = ivh_this_cpu_steal_ns();
			bool stolen = steal_now - current->cs_steal_start
					> IVH_HOT_STEAL_FLOOR_NS;

			if (READ_ONCE(ivh_universal_eligible) && !current->ivh_exclude) {
				ivh_hot_preempt_update(stolen);
				ivh_hot_wait_decay_sample_zero();
			}
			/*
			 * Observe-only stats (ivh_exec -v): independent of the
			 * EWMA feed above, and independent of eligibility --
			 * this task may not be migration-eligible at all
			 * (ivh_exec -v -n). Global per-CPU counters, one
			 * observation domain at a time by design: fine for a
			 * benchmark wrapper measuring one workload in
			 * isolation, not meant to be combined with unrelated
			 * concurrent IVH-eligible traffic.
			 */
			if (current->ivh_observe) {
				unsigned int bucket;

				this_cpu_inc(ivh_obs_total_holds);
				if (stolen)
					this_cpu_inc(ivh_obs_stolen_holds);
				/* Every observed hold contributes, contended or
				 * not -- total_holds is the matching denominator. */
				this_cpu_add(ivh_obs_cs_time_total_ns, cs_ns);
				/*
				 * Same hold, same cs_ns, binned by magnitude so
				 * ivh_exec -v can report p90/p95 and not just the
				 * mean (see IVH_OBS_CS_HIST_BUCKETS above). Both
				 * ends are clamped explicitly rather than trusted:
				 *   cs_ns == 0 -- ilog2(0) is undefined (fls64(0)
				 *     - 1 == -1 here), which would index the array
				 *     at [-1]. A zero-length hold is real (two
				 *     sched_clock() reads inside one coarse tick)
				 *     and belongs at the bottom, so fold it into
				 *     bucket 0 with the 1ns holds.
				 *   ilog2(cs_ns) >= BUCKETS -- saturate into the
				 *     top bucket. Every hold is counted exactly
				 *     once, so the bucket sum stays equal to
				 *     total_holds and userspace's percentile walk
				 *     has a trustworthy denominator.
				 * Preempted and non-preempted holds go into the
				 * same histogram on purpose: the whole point is
				 * where the combined tail sits, so the stolen
				 * holds are not split out here.
				 */
				bucket = cs_ns ? ilog2(cs_ns) : 0;
				if (bucket >= IVH_OBS_CS_HIST_BUCKETS)
					bucket = IVH_OBS_CS_HIST_BUCKETS - 1;
				this_cpu_inc(ivh_obs_cs_hist[bucket]);
			}
		}
	}
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
		cs_enter(lock);
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
		cs_enter(lock);
	}
	return ret;
}
EXPORT_SYMBOL(_raw_spin_trylock_bh);
#endif

#ifndef CONFIG_INLINE_SPIN_LOCK
noinline void __lockfunc _raw_spin_lock(raw_spinlock_t *lock)
{
	ivh_pre_lock(lock);
	__raw_spin_lock(lock);
	if (!in_interrupt()) {
		current->lock_depth++;
		cs_enter(lock);
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
		cs_enter(lock);
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
		cs_enter(lock);
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
		cs_enter(lock);
	}
}
EXPORT_SYMBOL(_raw_spin_lock_bh);
#endif

#ifdef CONFIG_UNINLINE_SPIN_UNLOCK
noinline void __lockfunc _raw_spin_unlock(raw_spinlock_t *lock)
{
	if (!in_interrupt()) {
		current->lock_depth--;
		cs_exit(lock);
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
		cs_exit(lock);
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
		cs_exit(lock);
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
		cs_exit(lock);
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
