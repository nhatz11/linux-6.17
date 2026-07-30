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
 * fraction of full capacity (scale 0–1024).  Default 1010 (2026-07-20,
 * updated from the original 512/50% guess): every benchmark-search and
 * HP-correlation run this session used 1010 -- it fires much closer to the
 * full-capacity boundary (~1.4% steal) than the original 50% guess, and no
 * regression from the wider firing range was found across ~15 workloads.
 * 512 is kept working (still a valid, more conservative value) but is no
 * longer the empirically-validated default.
 *   echo 1010 > /proc/sys/kernel/ivh_capacity_threshold
 */
unsigned long ivh_capacity_threshold = 1010UL;

/*
 * Time-left gate: skip migration when this many nanoseconds remain in the
 * estimated active burst.  Tunable at runtime via sysctl without a rebuild:
 *   echo 250000 > /proc/sys/kernel/ivh_time_left_threshold_ns
 * Default 4 ms (2026-07-20, updated from the original 500 μs guess): this is
 * the value actually used across every reported win this session (ebizzy,
 * dbench, hackbench-g4). A sweep down to 50 μs helped short-CS NHextend3
 * further but cost hackbench some of its margin -- there is no single value
 * confirmed best for every workload; 4ms is the safer, broadly-validated
 * choice until the CS-scaled adaptive threshold (state-of-the-art doc §5.1)
 * is implemented.
 */
unsigned long ivh_time_left_threshold_ns = 4000000UL;

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
 * Default 8 (2026-07-20, updated from the original 3 guess): the value
 * actually used across every reported win/loss measurement this session.
 * Set to 0 to disable the cap.
 *   echo 8 > /proc/sys/kernel/ivh_max_concurrent
 */
unsigned long ivh_max_concurrent = 8UL;

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
 * Gate 1+2 "time left" formula toggle, kept for A/B testing against the
 * original formula without a rebuild:
 *   0           = this commit's original rq->ewma_act_ns formula, verbatim.
 *   1 (default) = the later tree's rq->last_active_time formula, updated
 *                 2026-07-20 -- this is the formula actually used across
 *                 every reported win/loss measurement this session, and the
 *                 one fed by current->last_cs_ns (the sys_ivh_cs_enter fix).
 * See ivh_steal_imminent() in kernel/sched/fair.c for both implementations.
 *   echo 0 > /proc/sys/kernel/ivh_time_left_source
 */
unsigned long ivh_time_left_source = 1UL;

/*
 * Destination-selection lock toggle:
 *   0 (default) = blocking raw_spin_lock_irqsave().
 *   1           = trylock-and-skip: if another thread is already selecting,
 *                 skip migration for this CS rather than spinning with IRQs
 *                 disabled. The caller retries on its next lock attempt.
 * See bpf_sched_pre_lock_migrate() in kernel/sched/fair.c.
 *   echo 1 > /proc/sys/kernel/ivh_selection_trylock
 */
unsigned long ivh_selection_trylock = 0UL;

/*
 * Migration dispatch mechanism toggle:
 *   0 (default) = set_cpus_allowed_ptr(current, {target}) + schedule(), then
 *                 restore the original mask. Has a documented lost-wakeup
 *                 hang class (see the TASK_RUNNING check in ivh_pre_lock(),
 *                 kernel/locking/spinlock.c) -- schedule() here can, in rare
 *                 cases, not return promptly.
 *   1           = migrate_task_to(current, target) -- calls stop_one_cpu() +
 *                 migration_cpu_stop() directly, the same primitive
 *                 set_cpus_allowed_ptr() itself eventually reaches internally
 *                 for a running task, but skips its general-purpose cpumask
 *                 save/restore and set_affinity_pending machinery. Requires
 *                 target_cpu to already be in current's cpus_mask (unlike
 *                 mechanism 0, which temporarily forces it); the migration is
 *                 skipped, not forced, when that's not the case.
 * See bpf_sched_pre_lock_migrate() in kernel/sched/fair.c.
 *   echo 1 > /proc/sys/kernel/ivh_migrate_mechanism
 */
unsigned long ivh_migrate_mechanism = 0UL;

/*
 * Hot Threads selectivity gate for ivh_pre_lock() (kernel/locking/spinlock.c),
 * default OFF.  When on, ivh_pre_lock() consults the calling task's own
 * ivh_wait_decay/ivh_preempt_decay (per-task EWMAs, include/linux/sched.h)
 * and skips the migration trigger unless BOTH are above threshold.
 * NOTE: enabling this also removes IVH's "self-rescue" migrations ahead of
 * cold-thread attempts, so validate via ivh_prelock_coldthread_skipped /
 * ivh_prelock_hotthread_passed (/proc/ivh_debug) that it doesn't cost a
 * measured win before trusting it.
 *   echo 1 > /proc/sys/kernel/ivh_hot_threads_enabled
 */
unsigned long ivh_hot_threads_enabled = 0UL;

/*
 * The sole eligibility gate for IVH migration and the Hot Threads EWMA
 * feed (default OFF). PF_IVH_ELIGIBLE is no longer consulted anywhere
 * (2026-07-20 -- see the dated comments in ivh_pre_lock(), cs_enter(),
 * cs_exit() in kernel/locking/spinlock.c, both wait-event feed sites in
 * kernel/locking/qspinlock.c, sys_ivh_cs_enter() below, and the mutex-spin
 * observability counter in kernel/locking/mutex.c): this sysctl is what
 * decides whether IVH does anything at all, for anyone, full stop. When
 * OFF, nothing gets IVH -- not even a process launched under ivh_exec's
 * default mode, which still sets PF_IVH_ELIGIBLE but that flag is now
 * vestigial (PR_GET_IVH_ELIGIBLE still reports it; no code path reads it
 * to decide whether to do IVH work). When ON, every task's contended
 * raw_spinlock acquisitions system-wide are covered, unconditionally.
 * All other safety gates in ivh_pre_lock() (bpf_sched_enabled(), in_task(),
 * preemptible(), lock_depth == 0, TASK_RUNNING) and the !in_interrupt()
 * guards in qspinlock.c are unrelated to this and still apply -- they
 * already exclude the genuinely dangerous contexts (IRQ/atomic/nested)
 * independently of eligibility, so this sysctl only ever widens which
 * tasks are considered, never which contexts are legal. Validated live
 * (2026-07-19/20): stable under sustained NHextend3 and hackbench load,
 * no crashes, real measured benefit for both, dozens of distinct
 * unrelated system processes observed engaging correctly with zero
 * opt-in of their own.
 *   echo 1 > /proc/sys/kernel/ivh_universal_eligible
 */
unsigned long ivh_universal_eligible = 0UL;

/*
 * Whether ivh_pre_lock()'s Hot Threads gate also consults ivh_preempt_decay
 * (restoring the old AND-gate) or only ivh_wait_decay (default, validated).
 * Default OFF: ivh_preempt_decay's real-KVM-steal-during-a-kernel-raw-
 * spinlock-hold signal was measured live to read exactly 0 for 90%+ of
 * eligible evaluations (host steal arrives in ~5-6ms chunks, kernel raw-
 * spinlock holds are sub-microsecond, so a steal event essentially never
 * lands inside one by chance) -- gating on it caused a severe regression
 * (2% -> 12%+ host-preempted-CS), confirmed unfixable by threshold tuning.
 * bpf_sched_pre_lock_migrate() already re-checks vCPU health in real time
 * (Gate 1+2, ivh_steal_imminent()) downstream, so a contended-but-safe
 * thread costs a declined evaluation there, not a wasted migration.
 *   echo 1 > /proc/sys/kernel/ivh_hot_preempt_gate_enabled
 */
unsigned long ivh_hot_preempt_gate_enabled = 0UL;

/*
 * Hot Threads AND-gate thresholds, IVH_HOTLOCK_SCALE fixed point (1<<10=1.0).
 * wait: 128 (~0.125) -- re-calibrated live on kernel 51+ (2026-07-18) after
 *   512 (the old per-lock Hotlock IVH_HOTLOCK_HALF cutoff, inherited without
 *   re-validation) was found to sit ABOVE the real hot-worker wait_decay
 *   distribution's mode (~384-512 under sustained contention), rejecting the
 *   majority of genuine lock-holders and driving host-preempted-CS from
 *   IVH-alone's ~0.7% up to ~7.8%. At 128, host-preempted-CS measured
 *   ~1.2% (vs IVH-alone's ~0.7%) while a CPU-busy/lock-free control thread
 *   stayed pinned at ivh_wait_decay==0 and was excluded 100% of the time
 *   (bpftrace-sampled, 20s NHEXTEND_DURATION -- vcap's ~5-6s refresh cycle
 *   makes anything shorter unreliable, see ivh_hot_preempt_gate_enabled's
 *   note on measurement pitfalls below). Re-tune if host oversubscription or
 *   workload contention characteristics differ materially from that setup.
 * preempt: 40 (~0.04) -- a genuinely exposed thread is only caught mid-hold
 *   a fraction of the time, so its EWMA converges to that fraction, not 1.0.
 *   This value scales with how oversubscribed the host currently is; tune
 *   per host/deployment.
 */
unsigned long ivh_hot_wait_threshold = 128UL;
unsigned long ivh_hot_preempt_threshold = 40UL;

/*
 * Hot Threads EWMA shift constant for wait_decay ONLY (alpha = 1/2^k;
 * smaller k reacts faster but is noisier).  Default 3 (alpha = 1/8).
 * Clamped to <=15 at use.  Used by ivh_hot_note_wait_event() in
 * kernel/locking/spinlock.c.  preempt_decay no longer shares this constant
 * -- see ivh_hot_preempt_ewma_k_rise/_fall below.
 *   echo 4 > /proc/sys/kernel/ivh_hot_threads_ewma_k
 */
unsigned long ivh_hot_threads_ewma_k = 3UL;

/*
 * Hot Threads: preempt_decay's asymmetric cooldown shift constants, used by
 * ivh_hot_preempt_update() in kernel/locking/spinlock.c -- k_rise on a real
 * catch (fast trigger), k_fall on a clean release (slow, self-adjusting
 * release). Replaces a single shared constant, which was measured to cause
 * a severe live regression (2.1% -> 15-17% host-preempted-CS) via a
 * self-undermining feedback loop: successful IVH protection suppresses the
 * catch evidence preempt_decay needs to stay above threshold, so it decays
 * back down, protection lapses, a real catch lands, it spikes back up, and
 * the cycle repeats. Defaults validated in userspace closed-loop simulation
 * (hotthreads_cooldown_test.c): k_rise=3/k_fall=8 restores host-preempted-CS
 * to the no-gate baseline while still lapsing (~700 holds, ~0.65s of real
 * activity) once a thread's true catch rate actually goes to zero -- unlike
 * a permanent latch, which was measured to never lapse at all.
 *   echo 8 > /proc/sys/kernel/ivh_hot_preempt_ewma_k_fall
 */
unsigned long ivh_hot_preempt_ewma_k_rise = 3UL;
unsigned long ivh_hot_preempt_ewma_k_fall = 8UL;

/*
 * Hot Threads: wait_decay 0-sample rate (1-in-N release events), fed from
 * cs_exit()'s existing eligible-task block (kernel/locking/spinlock.c) so it
 * doesn't add a new touch point on the uncontended fastpath. Without this,
 * wait_decay only ever receives SCALE-samples (at contended-wait entry) and
 * can never decay -- a thread that contended briefly (e.g. at startup) stays
 * permanently above threshold even after arbitrarily long idleness. Default
 * 8, matching the old per-lock Hotlock table's identical 1-in-8 coarsening
 * for its non-contended sample path.
 *   echo 8 > /proc/sys/kernel/ivh_hot_wait_zero_n
 */
unsigned long ivh_hot_wait_zero_n = 8UL;

/*
 * Hot Threads: sticky one-way wait_decay LATCH (default OFF). When enabled,
 * the FIRST time a task's ivh_wait_decay ever exceeds ivh_hot_wait_threshold
 * its per-task ivh_wait_latched bit is set and it is thereafter treated as
 * "hot" (worth an IVH evaluation) permanently, regardless of any subsequent
 * downward decay via the 1-in-N 0-sample. Rationale (root-caused live on
 * kernel 51+, 2026-07-18): under sustained contention the real hot-worker
 * wait_decay distribution is centred BELOW the 512=HALF threshold (mode
 * ~384-512), and the 1-in-N 0-sample keeps pulling it back down, so a fixed
 * threshold rejects the majority of genuine lock-holders' acquisitions --
 * driving host-preempted-CS from IVH-alone's ~0.7% up to ~7.8%. The latch
 * removes the decay-induced lapse WITHOUT lowering the arming bar, so a task
 * that never contends (ivh_wait_decay stays 0, e.g. sshd/idle) never arms and
 * stays correctly excluded. This is the permanent-latch worst-case fallback
 * validated in userspace (hotthreads_latch_test.c). NOTE: like the userspace
 * model, this permanent latch never lapses -- a once-hot, now-idle task stays
 * armed for its lifetime; a cooldown/decaying-latch variant is the follow-up.
 *   echo 1 > /proc/sys/kernel/ivh_hot_wait_latch_enabled
 */
unsigned long ivh_hot_wait_latch_enabled = 0UL;

/* Weak fallback for non-x86/non-KVM builds; arch/x86/kernel/kvm.c provides
 * the strong definition when CONFIG_KVM_GUEST is active on this arch. */
u64 __weak ivh_this_cpu_steal_ns(void)
{
	return 0;
}

/*
 * ---------------------------------------------------------------------------
 * IVH Part C source-selection knobs (Build 1, tools/bpf/docs/
 * ivh_tsc_full_redesign_build_plan_2026-07-29.md sec 3.6).
 * ---------------------------------------------------------------------------
 *
 * ivh_cap_source and ivh_preempt_event_source are DELIBERATELY TWO KNOBS, not
 * one, and that is not tidiness -- it is the direct lesson of the bimodal
 * phantom-steal bug.  They are independent replacements with independent
 * failure modes, and the discipline this whole build is organised around is
 * that no two new signals become authoritative at the same time.  That bug
 * was diagnosable ONLY because "either alone is fine, combined is broken" had
 * been established as a fact; establishing the same fact cheaply for these
 * two costs a couple of extra measurement windows, which is minutes, against
 * a rebuild+reboot, which is not.
 *
 * Both use the three-valued shape ivh_pv_preempt_src established:
 *   0 = the existing real-steal-derived value (default; bit-identical to
 *       today, and the replacement is not even consulted)
 *   1 = shadow: compute both, compare, log, keep returning the existing one
 *   2 = the Part C value is authoritative
 */
unsigned long ivh_cap_source = 0UL;

unsigned long ivh_preempt_event_source = 0UL;

/*
 * Tick-stamp staleness threshold, in RAW TSC CYCLES.  3,300,000 = 1.5 ms at
 * 2200 MHz, recalibrated from the live tsc_khz at late_initcall
 * (arch/x86/kernel/kvm.c), with the literal as the pre-calibration fallback.
 *
 * 1500 us is chosen, not guessed: it is the same number as
 * IVH_BEAT_THRESHOLD_US and the same number as the `> 1500000` ns in
 * is_cpu_preempted(), so Part C's jump detector is a CONTROLLED COMPARISON
 * against the preemption signal this tree already has rather than a new
 * signal with a new tuning surface.  A divergence between ivh_vact_jumps and
 * rq->preemptions is then a bug to be explained, not a difference to be
 * shrugged at.  Sysctl, sweepable.
 */
unsigned long ivh_vact_jump_threshold = 3300000UL;

/*
 * Tumbling-window length for the capacity ratio, in NANOSECONDS (converted to
 * cycles at the point of use, so this knob stays readable and host-portable).
 * 100 ms starts near vcap's own update period; check main.cpp's loop period
 * before committing to it.
 */
unsigned long ivh_vact_window_ns = 100000000UL;

/*
 * Residual (sub-jump-threshold) steal accounting in ivh_vact_tick().
 *
 *   0 (default) = the arithmetic Build 1 shipped, byte for byte: a gap below
 *       ivh_vact_jump_threshold is credited entirely to `used`.
 *   1           = split every gap into one nominal tick of execution plus an
 *       excess booked as steal, carrying the signed shortfall between ticks.
 *
 * The full root-cause writeup, including the 6.17.0-rseqport67 numbers that
 * motivated it (Part C agreeing with only 40% of real steal's migrate
 * verdicts and essentially never over-triggering), is at
 * ivh_vact_gap_split() in kernel/sched/core.c.  Default off, and it must stay
 * off until an A/B on /proc/ivh_debug's ivh_dec_* / ivh_cap_pass_* shows it
 * moves Part C toward real steal WITHOUT crossing into over-triggering --
 * exactly the discipline ivh_ref_carry is held to.
 */
unsigned long ivh_vact_residual = 0UL;

/*
 * Run the dual migration-decision evaluation (sec 3.8).  Default off.
 *
 * This is the knob that discharges constraint #2 -- "any boot containing a
 * TSC-vs-real comparison must ship, in that same build, whatever is needed to
 * answer *does IVH make the same migration decisions with the TSC signal as
 * with the real one*".  Counters on the raw signals do not answer that
 * question; only evaluating the actual gate twice does.
 *
 * Off by default because the destination-set half costs an O(nr_cpus) walk
 * per evaluation.  That walk sits on a path which already takes a global
 * raw_spin_lock_irqsave, crosses a BPF trampoline and does a full CPU scan,
 * so it is affordable inside a measurement window and unaffordable as a
 * permanent tax -- hence a knob rather than a compile-time choice.
 */
unsigned long ivh_decision_shadow = 0UL;

#ifdef CONFIG_SYSCTL
/*
 * Both source selectors share one validating handler shape, and it is the
 * shape ivh_pv_proc_preempt_src() (arch/x86/kernel/kvm.c) established: parse
 * into a LOCAL, validate, and publish to the global only once the value is
 * known good, so no concurrent reader ever observes a transiently invalid
 * setting.  For a three-valued knob whose values mean "off / measure / trust",
 * a value of 3 silently landing in the global would be indistinguishable at
 * every read site from 2, i.e. a typo would flip a signal to authoritative.
 *
 * Deliberately NO cross-knob rejection between these two.  Setting both to 2
 * at once is the FIRST COMBINATION of sec 3.10's flip order -- an explicitly
 * planned phase, not a mistake to be blocked.  What the plan asks for is that
 * it happen after each has been proved alone, and no sysctl handler can check
 * "has a human read the histograms yet".  The guard that belongs in code is
 * the one that rejects a configuration that could only ever be a silent
 * no-op, which is why the CS-side knobs have one and these do not.
 */
static int ivh_proc_source_common(const struct ctl_table *table, int write,
				  void *buffer, size_t *lenp, loff_t *ppos,
				  unsigned long *target, const char *name)
{
	unsigned long val = READ_ONCE(*target);
	struct ctl_table tmp = *table;
	int ret;

	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (val > 2) {
		pr_err("IVH: refusing %s=%lu: valid values are 0 (real steal-derived value, default), 1 (shadow compare) and 2 (Part C authoritative)\n",
		       name, val);
		return -EINVAL;
	}

	WRITE_ONCE(*target, val);
	return 0;
}

static int ivh_proc_cap_source(const struct ctl_table *table, int write,
			       void *buffer, size_t *lenp, loff_t *ppos)
{
	return ivh_proc_source_common(table, write, buffer, lenp, ppos,
				      &ivh_cap_source, "ivh_cap_source");
}

static int ivh_proc_preempt_event_source(const struct ctl_table *table, int write,
					 void *buffer, size_t *lenp, loff_t *ppos)
{
	return ivh_proc_source_common(table, write, buffer, lenp, ppos,
				      &ivh_preempt_event_source,
				      "ivh_preempt_event_source");
}

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
		.procname	= "ivh_time_left_source",
		.data		= &ivh_time_left_source,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_selection_trylock",
		.data		= &ivh_selection_trylock,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_migrate_mechanism",
		.data		= &ivh_migrate_mechanism,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_hot_threads_enabled",
		.data		= &ivh_hot_threads_enabled,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_universal_eligible",
		.data		= &ivh_universal_eligible,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_hot_preempt_gate_enabled",
		.data		= &ivh_hot_preempt_gate_enabled,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_hot_wait_threshold",
		.data		= &ivh_hot_wait_threshold,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_hot_preempt_threshold",
		.data		= &ivh_hot_preempt_threshold,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_hot_threads_ewma_k",
		.data		= &ivh_hot_threads_ewma_k,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_hot_wait_zero_n",
		.data		= &ivh_hot_wait_zero_n,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_hot_wait_latch_enabled",
		.data		= &ivh_hot_wait_latch_enabled,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_hot_preempt_ewma_k_rise",
		.data		= &ivh_hot_preempt_ewma_k_rise,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_hot_preempt_ewma_k_fall",
		.data		= &ivh_hot_preempt_ewma_k_fall,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	/*
	 * Build 1 (build plan sec 3.6), scheduler-side knobs.  The two source
	 * selectors default to 0, so Part C is computed on every tick and
	 * consumed by nothing; ivh_decision_shadow defaults to 0, so the dual
	 * evaluation and its O(nr_cpus) walk never run.  The remaining two are
	 * parameters of a signal nobody reads at those defaults.
	 */
	{
		.procname	= "ivh_cap_source",
		.data		= &ivh_cap_source,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= ivh_proc_cap_source,
	},
	{
		.procname	= "ivh_preempt_event_source",
		.data		= &ivh_preempt_event_source,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= ivh_proc_preempt_event_source,
	},
	{
		.procname	= "ivh_vact_jump_threshold",
		.data		= &ivh_vact_jump_threshold,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_vact_window_ns",
		.data		= &ivh_vact_window_ns,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_vact_residual",
		.data		= &ivh_vact_residual,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_decision_shadow",
		.data		= &ivh_decision_shadow,
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
 *
 * Two fixes (2026-07-20, found while investigating why short-CS NHextend3
 * runs lost throughput under IVH): this path used to skip both checks
 * ivh_pre_lock() already applies to the kernel-spinlock path --
 *   1. ivh_universal_eligible wasn't consulted, only PF_IVH_ELIGIBLE. A
 *      process that relies solely on the universal-eligible sysctl (no
 *      ivh_exec wrapper, e.g. a plain NHextend3 invocation) got NO
 *      protection via this syscall at all -- any measured benefit in that
 *      configuration was coming from elsewhere (kernel-lock piggybacking in
 *      NHextend3's other syscalls), not this mechanism.
 *   2. No per-vCPU eval cooldown (ivh_eval_cooldown_ok()). Every single
 *      lock attempt paid the full evaluation (global spinlock, IRQs off,
 *      BPF selection hook) regardless of how recently the same vCPU had
 *      already been checked -- at short critical-section lengths (high
 *      attempt rate) this needlessly re-runs the expensive path far more
 *      often than the danger signal can actually change.
 *
 * Third fix, same investigation, one level deeper (2026-07-20): even with
 * both fixes above, ivh_steal_imminent()'s time-left formula (fair.c) was
 * still approving a 13us-CS migration exactly as eagerly as a 1.6ms-CS one,
 * because its CS-length term, current->last_cs_ns, was never actually fed
 * this task's real userspace critical section. cs_exit() (spinlock.c) only
 * ever writes it for *kernel* raw-spinlock holds; userspace callers (e.g.
 * NHextend3) report their own CS duration via rseq::last_cs_overall_ns
 * (include/uapi/linux/rseq.h) at every unlock, but nothing on this syscall
 * path ever read it. So the gate's "how much runway do I actually need"
 * term was a stale, near-zero kernel-lock residue regardless of how long
 * the caller's real critical section was -- the formula had the right
 * shape but a dead input. Fixed by reading the caller's self-reported CS
 * duration off its own rseq page (best-effort, same copy_from_user_nofault
 * pattern as rseq_delay_resched() in kernel/rseq.c) right before the gate
 * evaluation, so ivh_steal_imminent() scales its danger window to this
 * task's actual CS length instead of a value that was never connected to
 * it. NOTE: ivh_time_left_threshold_ns is a fixed-size window on top of
 * this term (see fair.c) and was tuned/validated against ~1.6ms CS
 * (NHextend3's default) -- at much shorter CS this plumbing fix alone only
 * partially helps; the window itself needs live retuning (smaller, e.g. a
 * fixed ~100us margin) to see the full effect at very short CS. Left
 * untouched here since it's a single sysctl already shared with, and
 * validated for, the kernel-spinlock path -- retune it live, don't
 * hardcode a new default blind.
 */
SYSCALL_DEFINE0(ivh_cs_enter)
{
	if (!bpf_sched_enabled())
		return 0;
	/* 2026-07-20: PF_IVH_ELIGIBLE no longer consulted here --
	 * ivh_universal_eligible is the sole gate now, not an ivh_exec-wrapper
	 * opt-in. if (!(current->flags & PF_IVH_ELIGIBLE) && ...)
	 * ivh_exclude (per-task opt-out) added same day. */
	if (!READ_ONCE(ivh_universal_eligible) || current->ivh_exclude)
		return 0;
	if (!ivh_eval_cooldown_ok())
		return 0;

	/*
	 * Feed the gate's CS-length term with this task's real userspace CS
	 * duration instead of the stale kernel-lock residue cs_exit() left
	 * there. Best-effort: old/short rseq registration or a faulting
	 * read just leaves current->last_cs_ns as it was (whatever the
	 * kernel-lock path last set, which is what happened unconditionally
	 * before this fix) -- never fatal, never blocks the migration
	 * decision on this working.
	 */
	if (current->rseq &&
	    current->rseq_len > offsetof(struct rseq, last_cs_overall_ns)) {
		u64 last_cs_ns;

		if (!copy_from_user_nofault(&last_cs_ns,
					     &current->rseq->last_cs_overall_ns,
					     sizeof(last_cs_ns)))
			current->last_cs_ns = last_cs_ns;
	}

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
