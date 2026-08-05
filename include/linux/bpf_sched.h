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

/* Advisory (non-authoritative) Gate 1+2 re-check for task @t's current CPU,
 * called from kernel/rseq.c's rseq_update_cpu_node_id() to publish
 * RSEQ_SCHED_STATE_FLAG_IVH_DANGER (include/uapi/linux/rseq.h) on every
 * return-to-userspace. Defined in kernel/sched/fair.c. See the doc comment
 * there and on RSEQ_SCHED_STATE_FLAG_IVH_DANGER for why this deliberately
 * duplicates ivh_steal_imminent()'s gates instead of calling it directly.
 */
struct task_struct;
bool ivh_task_rq_in_danger(struct task_struct *t);

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
 * kernel/locking/spinlock.c, surfaced via /proc/ivh_debug (kernel/sched/fair.c).
 *
 * cs_time_total_ns is fed from cs_exit() and pairs with total_holds as its
 * denominator; wait_total_ns is fed from the qspinlock slowpath
 * (kernel/locking/qspinlock.c) and pairs with wait_events, which counts only
 * contended acquisitions -- the two averages have deliberately different
 * denominators, see the comments at both feed sites. */
DECLARE_PER_CPU(u64, ivh_obs_total_holds);
DECLARE_PER_CPU(u64, ivh_obs_stolen_holds);
DECLARE_PER_CPU(u64, ivh_obs_cs_time_total_ns);
DECLARE_PER_CPU(u64, ivh_obs_wait_total_ns);
DECLARE_PER_CPU(u64, ivh_obs_wait_events);

/* Log2-bucketed CS-hold-time histogram, same gate and same feed site
 * (cs_exit()) as cs_time_total_ns, giving the tail of the hold-duration
 * distribution that a sum+count average structurally cannot -- see the long
 * comment above the definition in kernel/locking/spinlock.c for the bucket
 * layout and the clamping rules. Bucket i covers 2^i .. 2^(i+1)-1 ns, except
 * i == 0 (0-1 ns, absorbs zero-length holds) and the top bucket (saturating,
 * >= 2^31 ns). 32 buckets reaches multi-second holds, far past both the
 * ~300ns common case and the 100us IVH_HOT_STEAL_FLOOR_NS. The bucket counts
 * sum to ivh_obs_total_holds. */
#define IVH_OBS_CS_HIST_BUCKETS 32
DECLARE_PER_CPU(u64, ivh_obs_cs_hist[IVH_OBS_CS_HIST_BUCKETS]);

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

/*
 * ---------------------------------------------------------------------------
 * IVH Part C source selection and the decision-agreement comparators.
 * Build 1, tools/bpf/docs/ivh_tsc_full_redesign_build_plan_2026-07-29.md
 * sec 3.6 / 3.8.  All defined in kernel/sched/bpf_sched.c; the source knobs
 * are consumed in kernel/sched/fair.c's Gate 1+2, the parameters in
 * kernel/sched/core.c's ivh_vact_tick().
 * ---------------------------------------------------------------------------
 *
 *   ivh_cap_source            0 rq->cpu_capacity (vcap) | 1 shadow | 2 rq->ivh_vact_capacity
 *   ivh_preempt_event_source  0 rq->last_preemption/last_active_time (real steal)
 *                             | 1 shadow | 2 rq->ivh_vact_last_preempt_tsc/_last_active_c
 *   ivh_vact_jump_threshold   tick-stamp staleness threshold, raw TSC cycles
 *   ivh_vact_window_ns        tumbling window for the capacity ratio, ns
 *   ivh_vact_residual         0 sub-threshold gaps count wholly as executing
 *                             | 1 remove this tick's idle, then split what is
 *                               left into one nominal tick of execution plus a
 *                               stolen excess, with a carried signed shortfall
 *                               (ivh_vact_gap_split(), kernel/sched/core.c --
 *                               the fix for Part C's measured under-trigger,
 *                               and the idle term is in turn the fix for that
 *                               fix's measured 4x over-trigger)
 *   ivh_decision_shadow       run the dual migration-decision evaluation
 */
extern unsigned long ivh_cap_source;
extern unsigned long ivh_preempt_event_source;
extern unsigned long ivh_vact_jump_threshold;
extern unsigned long ivh_vact_window_ns;
extern unsigned long ivh_vact_residual;
extern unsigned long ivh_decision_shadow;

/*
 * Decision-agreement comparators (sec 3.8).  Defined in kernel/sched/fair.c
 * beside the gates they instrument, summed into /proc/ivh_debug.  Fed only
 * while ivh_decision_shadow != 0.
 *
 * (a) Gate 1+2 VERDICT agreement -- ivh_dec_*.  The gate is evaluated twice,
 *     once from (rq->cpu_capacity, rq->last_preemption, rq->last_active_time)
 *     and once from (rq->ivh_vact_capacity, ivh_vact_last_preempt_tsc,
 *     ivh_vact_last_active_c), and the pair is binned into a 2x2.  The
 *     existing ivh_steal_imminent_capacity_reject / _time_left_reject are
 *     left strictly alone so that what they mean does not change -- the same
 *     discipline the duplicated-gate comment in fair.c already enforces.
 *
 * (b) DESTINATION-SET agreement -- ivh_cap_pass_*.  The full BPF scan cannot
 *     be run twice, but the thing the capacity number actually decides can
 *     be: which CPUs pass GATE_CAPACITY_LOW and GATE_NOT_BETTER.  If
 *     pass_tsc_only and pass_real_only are both small relative to pass_both,
 *     the capacity replacement is behaviourally equivalent and
 *     ivh_cap_source=2 is safe; if either is large it is not, and the
 *     DIRECTION says which way the replacement is biased.  This is the
 *     operational form of "would IVH have picked from the same candidate
 *     set", which counters on the raw signals cannot answer.
 */
DECLARE_PER_CPU(u64, ivh_dec_agree_go);
DECLARE_PER_CPU(u64, ivh_dec_agree_nogo);
DECLARE_PER_CPU(u64, ivh_dec_tsc_only_go);
DECLARE_PER_CPU(u64, ivh_dec_real_only_go);
DECLARE_PER_CPU(u64, ivh_cap_pass_both);
DECLARE_PER_CPU(u64, ivh_cap_pass_real_only);
DECLARE_PER_CPU(u64, ivh_cap_pass_tsc_only);
DECLARE_PER_CPU(u64, ivh_cap_pass_neither);

/*
 * ---------------------------------------------------------------------------
 * IVH "uc" (used-capacity): in-kernel replica of vcap's used/(used+stolen)
 * EMA, the vcap-retirement signal.
 * tools/bpf/docs/ivh_vcap_retirement_build_plan_2026-08-03.md.
 * Sysctls defined in kernel/sched/bpf_sched.c; the tick producer is
 * ivh_uc_tick() in kernel/sched/core.c; the gate-level and destination-set
 * comparators below are defined in kernel/sched/fair.c beside the Part C
 * ones they parallel (sec 5.4); the window-close histogram/threshold
 * counters are defined in kernel/sched/core.c beside ivh_uc_close() (sec
 * 5.2/5.3), since window close -- unlike a gate evaluation -- happens on the
 * tick path, not the lock path.
 *
 *   ivh_uc_enabled          produce the signal (default ON, ungated)
 *   ivh_uc_window_ns        measurement window, ns (matches vcap's -p 200)
 *   ivh_uc_duty_ns          0 continuous | vcap's -s 5000 duty cycle for
 *                           shadow-comparison sample-count isolation only
 *   ivh_uc_ema_alpha_q16    Q16 EMA coefficient (868 = 10.4s half-life)
 *   ivh_uc_used_source      0 WALL (production, idle-excluded, spinner-
 *                           invariant) | 1 ACCT (vcap-formula replica,
 *                           validation only -- see plan sec 1.3/G3)
 *   ivh_uc_min_steal_ns     vcap's <10us-stolen -> 1.0 guard, reproduced
 *   ivh_uc_min_avail_pct    minimum non-idle % of a window before it may
 *                           close; below this the window extends instead
 *   ivh_uc_shadow           feed the window-close comparison counters
 *   ivh_uc_avgcap_enabled   run the 1Hz average_capacity_all worker (sec 6.3)
 *   ivh_cap_source           (existing, extended) 0 vcap | 1 shadow |
 *                           2 rq->ivh_vact_capacity | 3 rq->ivh_uc_capacity
 */
extern unsigned long ivh_uc_enabled;
extern unsigned long ivh_uc_window_ns;
extern unsigned long ivh_uc_duty_ns;
extern unsigned long ivh_uc_ema_alpha_q16;
extern unsigned long ivh_uc_used_source;
extern unsigned long ivh_uc_min_steal_ns;
extern unsigned long ivh_uc_min_avail_pct;
extern unsigned long ivh_uc_shadow;
extern unsigned long ivh_uc_avgcap_enabled;

/*
 * Gate-level agreement (real vs uc), parallel to ivh_dec_* above but scoped
 * to the capacity term only -- uc does not touch Gate 2, so both arms of
 * this comparison use the real (non-TSC) time-left verdict, isolating
 * exactly the difference the retirement plan needs measured (sec 5.4).
 */
DECLARE_PER_CPU(u64, ivh_dec_uc_agree_go);
DECLARE_PER_CPU(u64, ivh_dec_uc_agree_nogo);
DECLARE_PER_CPU(u64, ivh_dec_uc_only_go);
DECLARE_PER_CPU(u64, ivh_dec_uc_real_only_go);

/* Destination-set agreement (vcap vs uc), parallel to ivh_cap_pass_* above. */
DECLARE_PER_CPU(u64, ivh_uc_pass_both);
DECLARE_PER_CPU(u64, ivh_uc_pass_vcap_only);
DECLARE_PER_CPU(u64, ivh_uc_pass_uc_only);
DECLARE_PER_CPU(u64, ivh_uc_pass_neither);

/*
 * Destination-set EMPTY rate, one bool per shadow evaluation per source --
 * the counter the pre-existing comparator above does not have.  Directly
 * predicts "IVH stops migrating at all", which retirement plan sec 1.4
 * shows rq->ivh_vact_capacity would already do today if it were live.
 */
DECLARE_PER_CPU(u64, ivh_destset_empty_vcap);
DECLARE_PER_CPU(u64, ivh_destset_empty_uc);
DECLARE_PER_CPU(u64, ivh_destset_empty_tsc);

/*
 * Window-close-level validation (sec 5.2/5.3), fed only while ivh_uc_shadow
 * is on.  Signed-divergence histogram of (uc_capacity - vcap_capacity),
 * 16 buckets spanning +-1024, the direct analogue of ivh_cs_age_hist_*.
 * Threshold-crossing 2x2s at both consumer thresholds: agreement in the
 * middle of the range is worth nothing if the two disagree AT the
 * thresholds that actually gate a decision (IVH_BPF_CAP_FLOOR = 850,
 * ivh_capacity_threshold = 1010).
 */
#define IVH_UC_DIV_HIST_BUCKETS	16
DECLARE_PER_CPU(u64, ivh_uc_div_hist[IVH_UC_DIV_HIST_BUCKETS]);
DECLARE_PER_CPU(u64, ivh_uc_thr850_both);
DECLARE_PER_CPU(u64, ivh_uc_thr850_vcap_only);
DECLARE_PER_CPU(u64, ivh_uc_thr850_uc_only);
DECLARE_PER_CPU(u64, ivh_uc_thr850_neither);
DECLARE_PER_CPU(u64, ivh_uc_thr1010_both);
DECLARE_PER_CPU(u64, ivh_uc_thr1010_vcap_only);
DECLARE_PER_CPU(u64, ivh_uc_thr1010_uc_only);
DECLARE_PER_CPU(u64, ivh_uc_thr1010_neither);

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
