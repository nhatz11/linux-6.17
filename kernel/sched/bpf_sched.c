// SPDX-License-Identifier: GPL-2.0
#include <linux/bpf.h>
#include <linux/cgroup.h>
#include <linux/bpf_verifier.h>
#include <linux/bpf_sched.h>
#include <linux/btf_ids.h>
#include <linux/sched/clock.h>
#include <linux/syscalls.h>
#include <linux/sysctl.h>
#include <linux/uaccess.h>
#include "sched.h"

/*
 * Step 1 (ivh-rebuild-main): core BPF_PROG_TYPE_SCHED infra only, ported
 * verbatim from the production tree's kernel/sched/bpf_sched.c lines
 * 913-1061 (the hook-registration/BTF-verification/kfunc section). None of
 * the IVH-specific tunables (capacity gates, Hot Threads, PV, uc/tks
 * pipeline) are ported here -- those arrive in Step 6. This step should add
 * zero scheduler behavior: no hook has a real call site yet.
 */

DEFINE_STATIC_KEY_FALSE(bpf_sched_enabled_key);

/*
 * Step 3 (ivh-rebuild-main): CS-hold timing storage + the ivh_cs_track_enabled
 * diagnostic sysctl -- see include/linux/bpf_sched.h for the mode
 * documentation and kernel/locking/spinlock.c's cs_enter()/cs_exit() for the
 * consumers. Default 1 (real sched_clock() TSC reads, bit-for-bit the
 * original always-on behavior this step's mechanism replaces).
 */
unsigned long ivh_cs_track_enabled = 1UL;
DEFINE_PER_CPU(u64, ivh_cs_fake_clock_ctr);

/*
 * Step 6 (ivh-rebuild-main): the capacity + migration decision engine,
 * plumbing only -- ivh_universal_eligible stays at its compiled default of
 * 0 (declared in include/linux/bpf_sched.h so kernel/locking/spinlock.c can
 * reach it too). Ported verbatim from production except where noted; see
 * this step's commit message for the full exclusion list (Hot Threads,
 * Part C/ivh_vact_* (G-LOCK-23 note: a MINIMAL subset of this -- just the
 * jump-detector fields ivh_gate_time_left_reject()'s tsc_pe branch reads,
 * see kernel/sched/core.c's ivh_vact_tick() -- was ported after all; the
 * capacity/EMA estimator, residual split, window accumulation, and
 * ivh_vact_capacity itself remain unported, deliberately), ivh_uc_shadow/
 * ivh_decision_shadow, ivh_uc_avgcap_enabled, the ivh_ref_* Plan-2 steal
 * estimator, ivh_ka_* idle keepalive (G-LOCK-23 note: ivh_preempt_event_source
 * and the tsc_pe Gate-2 branch ARE now ported, see below -- this list entry
 * is stale, kept only so the exclusion list's history stays legible), and
 * the broadcast preempt-migrate mechanism / cfs_spin_len hook).
 */
unsigned long ivh_universal_eligible = 0UL;

unsigned long ivh_capacity_threshold = 1010UL;
unsigned long ivh_time_left_threshold_ns = 4000000UL;
unsigned long ivh_migration_timeout_ns = 500000UL;
unsigned long ivh_max_concurrent = 8UL;
unsigned long ivh_sched_timeout_ms = 1UL;
unsigned long ivh_eval_cooldown_ns = 50000UL;
unsigned long ivh_rcu_guard = 1UL;   /* G-LOCK-49: see bpf_sched.h */
unsigned long ivh_time_left_source = 1UL;
unsigned long ivh_selection_trylock = 0UL;
unsigned long ivh_migrate_mechanism = 0UL;

/*
 * ivh_cap_source -- which capacity number Gate 1 reads (kernel/sched/
 * fair.c's ivh_gate_capacity()).
 *   0 (default) = rq->cpu_capacity, the vcap-shaped fallback (reads a flat
 *                 1024 on every vCPU now that the vcap daemon is retired).
 *   3           = rq->ivh_uc_capacity, the one production actually runs.
 * Value 2 (Part C / rq->ivh_vact_capacity) is a documented dead end (sec
 * 1.7, "attempted, measured regression, root-caused, not shipped") and is
 * not ported; ivh_gate_capacity() folds it into the default case rather
 * than referencing a field that does not exist in this rebuild. Value 1
 * (shadow) has no meaning without ivh_decision_shadow, also not ported.
 */
unsigned long ivh_cap_source = 0UL;

/*
 * G-LOCK-39 sampler knobs. ivh_tks_sampler_ns needs a validating handler for
 * two reasons: the period sets a per-CPU hrtimer rate, so an accidental
 * sub-microsecond value would be a self-inflicted interrupt storm; and the
 * per-CPU timers have to be re-armed when it changes, which a plain
 * proc_doulongvec_minmax() would never do.
 */
static int ivh_proc_tks_sampler(const struct ctl_table *table, int write,
				void *buffer, size_t *lenp, loff_t *ppos)
{
	unsigned long val = READ_ONCE(ivh_tks_sampler_ns);
	struct ctl_table tmp = *table;
	int ret;

	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (val && (val < 20000 || val > 10000000)) {
		pr_err("IVH: refusing ivh_tks_sampler_ns=%lu: accepts 0 "
		       "(tick-driven, the default) or 20000..10000000 ns. "
		       "Below 20us the per-CPU hrtimer rate stops being "
		       "affordable. Validated operating point: 20000 with "
		       "ivh_tks_duty_pct=5 and ivh_tks_phase_pct=0\n", val);
		return -EINVAL;
	}

	/* The one-period bonus (ivh_tks_phase_pct) is an UNDERSAMPLING
	 * correction: it credits a whole period per detected event, which is
	 * unbiased only while events are far rarer than samples. With the
	 * sampler driving it is pure inflation -- 1.49 against 0.94 at 20 kHz
	 * -- and worse than that ratio suggests, because at a 20 us period
	 * ordinary interrupt-entry jitter clears the 1 us deadband routinely
	 * and each spurious hit books a whole period. goto_mode.sh sets
	 * phase_pct=100 after every reboot, so without this check the machine's
	 * default state is exactly the inflating one. Refuse rather than
	 * silently rewrite the operator's other knob. */
	if (val && READ_ONCE(ivh_tks_phase_pct)) {
		pr_err("IVH: refusing ivh_tks_sampler_ns=%lu while "
		       "ivh_tks_phase_pct=%lu: the phase bonus corrects for "
		       "undersampling and inflates a properly-sampled signal. "
		       "Write ivh_tks_phase_pct=0 first\n",
		       val, READ_ONCE(ivh_tks_phase_pct));
		return -EINVAL;
	}

	WRITE_ONCE(ivh_tks_sampler_ns, val);
	ivh_tks_sampler_reconfigure();
	return 0;
}

/*
 * F7: ivh_tks_on_ns feeds both a multiply and a timer interval. Unbounded,
 * on_ns * (100 - duty) overflows u64 above ~1.9e17 and wraps to an off_ns of
 * years -- the sampler would stop on that CPU while account_process_tick()
 * stays gated off, freezing steal accounting silently.
 */
static int ivh_proc_tks_on_ns(const struct ctl_table *table, int write,
			      void *buffer, size_t *lenp, loff_t *ppos)
{
	unsigned long val = READ_ONCE(ivh_tks_on_ns);
	struct ctl_table tmp = *table;
	int ret;

	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (val && (val < 100000 || val > 1000000000)) {
		pr_err("IVH: refusing ivh_tks_on_ns=%lu: 0 (no duty cycling) "
		       "or 100000..1000000000 ns\n", val);
		return -EINVAL;
	}

	WRITE_ONCE(ivh_tks_on_ns, val);
	return 0;
}

static int ivh_proc_tks_duty(const struct ctl_table *table, int write,
			     void *buffer, size_t *lenp, loff_t *ppos)
{
	unsigned long val = READ_ONCE(ivh_tks_duty_pct);
	struct ctl_table tmp = *table;
	int ret;

	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	/* 0 would divide by zero when computing the off-interval. */
	if (val < 1 || val > 100) {
		pr_err("IVH: refusing ivh_tks_duty_pct=%lu: 1..100\n", val);
		return -EINVAL;
	}

	WRITE_ONCE(ivh_tks_duty_pct, val);
	return 0;
}

static int ivh_proc_cap_source(const struct ctl_table *table, int write,
			       void *buffer, size_t *lenp, loff_t *ppos)
{
	unsigned long val = READ_ONCE(ivh_cap_source);
	struct ctl_table tmp = *table;
	int ret;

	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (val != 0 && val != 3) {
		pr_err("IVH: refusing ivh_cap_source=%lu: this rebuild only "
		       "implements 0 (vcap fallback) and 3 (ivh_uc_capacity) "
		       "-- 1 (shadow) and 2 (Part C) are not ported, see "
		       "tools/bpf/docs/ivh_rebuild_plan.md sec 1.7\n", val);
		return -EINVAL;
	}

	WRITE_ONCE(ivh_cap_source, val);
	return 0;
}

/*
 * G-LOCK-23: which preemption-event clock ivh_gate_time_left_reject()'s
 * (kernel/sched/fair.c) two formulas read their "time since last preemption/
 * idle-exit" terms from.
 *   0 (default) = the existing paravirt_steal_clock()-derived
 *                 rq->last_preemption/last_active_time path -- real code,
 *                 but requires KVM_FEATURE_STEAL_TIME, confirmed absent on
 *                 this host, so this is a documented no-op here (matches
 *                 ewma_act_ns's existing "safe no-op, not a missing
 *                 feature" posture, same file's comment on that field in
 *                 kernel/sched/sched.h).
 *   2            = ivh_vact_tick()'s TSC-native jump detector (kernel/sched/
 *                 core.c), steal-time-independent, the minimal Part C port
 *                 this value exists to select.
 * Value 1 (production's ewma_act_ns/vsched_module path) is not ported --
 * that field has no in-tree writer in this rebuild either (it is written
 * exclusively by an out-of-tree module via /proc/vact_write) -- same
 * posture as ivh_cap_source's refusal of its own unported values above.
 */
unsigned long ivh_preempt_event_source = 0UL;

static int ivh_proc_preempt_event_source(const struct ctl_table *table, int write,
					 void *buffer, size_t *lenp, loff_t *ppos)
{
	unsigned long val = READ_ONCE(ivh_preempt_event_source);
	struct ctl_table tmp = *table;
	int ret;

	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (val != 0 && val != 2) {
		pr_err("IVH: refusing ivh_preempt_event_source=%lu: this rebuild "
		       "only implements 0 (steal-time path, dead without "
		       "KVM_FEATURE_STEAL_TIME) and 2 (ivh_vact_tick(), TSC-native) "
		       "-- 1 (ewma_act_ns/vsched_module) is not ported, see "
		       "kernel/sched/core.c's ivh_vact_tick()\n", val);
		return -EINVAL;
	}

	WRITE_ONCE(ivh_preempt_event_source, val);
	return 0;
}

/*
 * ivh_vact_tick()'s (kernel/sched/core.c) own jump-vs-noise threshold, in
 * nanoseconds -- converted to raw TSC cycles at the point of use via
 * ivh_tsc_ns_to_cycles(), so it stays correct at any tsc_khz. Deliberately
 * NOT ivh_pv_beat_threshold (arch/x86/kernel/kvm.c): that knob is set to
 * 220000 cycles (~100us) by /root/spin_mode's IVH_PV/STOCK_TAS modes for
 * qspinlock early-bail tuning, one tenth of a CONFIG_HZ=1000 tick period,
 * which measured as ~100% of ordinary ticks misclassified as jumps. This
 * knob must never be added to /root/spin_mode's per-mode sysctl resets --
 * it needs to hold one stable value across every spin mode for
 * ivh_vact_jumps to be comparable cross-mode. Default is 1.5 tick periods
 * (TICK_NSEC + TICK_NSEC/2), which at CONFIG_HZ=1000 is 1,500,000ns --
 * bit-identical in cycles to the 1500us default ivh_pv_beat_threshold
 * itself boots with, the operating point this was validated against.
 */
#define IVH_VACT_JUMP_NS	(TICK_NSEC + TICK_NSEC / 2)
unsigned long ivh_vact_jump_ns = IVH_VACT_JUMP_NS;
/* G-LOCK-43: shortest host deschedule we will believe in. See
 * ivh_vact_certain_negative(). */
unsigned long ivh_vact_min_preempt_ns = 2000UL;	/* 2 us */
EXPORT_SYMBOL_GPL(ivh_vact_min_preempt_ns);
/* G-LOCK-43: was TICK_NSEC, which made sense only while the tick was the
 * detector's sole clock. ivh_tks_sampler_fn() can now drive it at 200 us, so
 * the hard floor is one sampler period; anything above that is the operator's
 * responsibility and ivh_vact_judgeable() adapts. Setting a threshold below
 * the ACTUAL driver period will produce false jumps -- that is the operator's
 * error to avoid, not something this bound can express. */
static unsigned long ivh_vact_jump_ns_min = 100000;	/* 100 us */

/*
 * IVH "uc" (used-capacity): in-kernel replica of vcap's used/(used+stolen)
 * EMA, replacing the vcap userspace daemon. See kernel/sched/core.c's
 * ivh_uc_tick()/ivh_uc_close()/ivh_uc_maybe_close_window().
 */
unsigned long ivh_uc_enabled = 1UL;
unsigned long ivh_uc_window_ns = 200000000UL;
unsigned long ivh_uc_duty_ns = 0UL;
unsigned long ivh_uc_ema_alpha_q16 = 868UL;
/* 0 (default, WALL) only -- 1 (ACCT) is a documented dead end (sec 1.7,
 * "selected on merit then retracted same day -- secretly PV-dependent via
 * kcpustat"). ivh_uc_close() still computes both variants every window
 * (matching production, cheap), this sysctl only selects which one
 * publishes to rq->ivh_uc_capacity. */
unsigned long ivh_uc_used_source = 0UL;
unsigned long ivh_uc_min_steal_ns = 10000UL;
unsigned long ivh_uc_min_avail_pct = 10UL;

/*
 * G-LOCK-51. ivh_cap_writer: 0 = the in-kernel ivh_uc_tick() estimator owns
 * rq->ivh_uc_capacity (default, unchanged behaviour); 1 = userspace (vcap)
 * owns it via /proc/ivh_cap_write and ivh_ucw_tick() republishes/expires it.
 * ivh_cap_source stays 3 either way and the ivh_cfg BPF map never moves --
 * only the writer changes, which is what keeps the BPF side a literal no-op
 * and stops the ivh_cfg != ivh_cap_source trap from re-arming.
 *
 * ivh_ucw_max_age_ns: how stale a userspace-written capacity may be before
 * it is treated as absent. ~3x vcap's full loop period is the right order.
 * 0 disables expiry -- available for a deliberate experiment, but it is the
 * setting that lets a dead daemon freeze every Gate 1 verdict silently, so
 * it is not a default.
 */
unsigned long ivh_cap_writer = 0UL;
unsigned long ivh_ucw_max_age_ns = 3000000000UL;	/* 3 s */

/*
 * G-LOCK-51 Gate 2 EWMA. alpha is Q16: 19195/65536 = 0.29289, matching
 * vcap's own steady-state decay (main.cpp:124-131) so the two estimators
 * are comparable. At ~148 detected preemptions/s/vCPU that is a ~20ms time
 * constant, the right order for a signal Gate 2 consults per acquisition.
 *
 * ivh_act_clamp_ns: ceiling applied to a burst BEFORE it enters the EWMA.
 * G-LOCK-50 measured the produced series at CV=10.4 -- a handful of ~1s
 * bursts on quiet vCPUs carry most of the variance. 100ms is ~30x the
 * measured produced mean (3389us), so it truncates only the genuine tail.
 * 0 disables clamping.
 */
unsigned long ivh_act_ema_alpha_q16 = 19195UL;
unsigned long ivh_act_clamp_ns = 100000000UL;		/* 100 ms */

static const struct ctl_table ivh_sysctls[] = {
	{
		.procname	= "ivh_cs_track_enabled",
		.data		= &ivh_cs_track_enabled,
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
		.procname	= "ivh_rcu_guard",
		.data		= &ivh_rcu_guard,
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
		.procname	= "ivh_vact_min_preempt_ns",
		.data		= &ivh_vact_min_preempt_ns,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_vact_jump_ns",
		.data		= &ivh_vact_jump_ns,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
		.extra1		= &ivh_vact_jump_ns_min,
	},
	{
		.procname	= "ivh_uc_enabled",
		.data		= &ivh_uc_enabled,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_uc_window_ns",
		.data		= &ivh_uc_window_ns,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_uc_duty_ns",
		.data		= &ivh_uc_duty_ns,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_uc_ema_alpha_q16",
		.data		= &ivh_uc_ema_alpha_q16,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_uc_used_source",
		.data		= &ivh_uc_used_source,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_uc_min_steal_ns",
		.data		= &ivh_uc_min_steal_ns,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_cap_writer",
		.data		= &ivh_cap_writer,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_ucw_max_age_ns",
		.data		= &ivh_ucw_max_age_ns,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_act_ema_alpha_q16",
		.data		= &ivh_act_ema_alpha_q16,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_act_clamp_ns",
		.data		= &ivh_act_clamp_ns,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_uc_min_avail_pct",
		.data		= &ivh_uc_min_avail_pct,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_steal_source",
		.data		= &ivh_steal_source,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_tks_deadband_ns",
		.data		= &ivh_tks_deadband_ns,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_tks_phase_pct",
		.data		= &ivh_tks_phase_pct,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_tks_carry_ticks",
		.data		= &ivh_tks_carry_ticks,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_tks_sampler_ns",
		.data		= &ivh_tks_sampler_ns,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= ivh_proc_tks_sampler,
	},
	{
		.procname	= "ivh_tks_duty_pct",
		.data		= &ivh_tks_duty_pct,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= ivh_proc_tks_duty,
	},
	{
		.procname	= "ivh_tks_on_ns",
		.data		= &ivh_tks_on_ns,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= ivh_proc_tks_on_ns,
	},
	{
		.procname	= "ivh_tks_idle_sub",
		.data		= &ivh_tks_idle_sub,
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

/*
 * sys_ivh_cs_enter - authoritative migration-decision entry point for
 * userspace critical sections (NHextend3's ivh_cs_enter()/
 * ivh_cs_enter_checked(), tools/bpf/docs/ivh_rebuild_plan.md sec 1.6).
 * Syscall number 470 (arch/x86/entry/syscalls/syscall_64.tbl), free in
 * upstream v6.17 and matching production's assignment exactly.
 *
 * Deliberately duplicates ivh_pre_lock()'s (kernel/locking/spinlock.c)
 * gate order rather than sharing a helper -- see production's own
 * comment, reproduced here: keeping the two bodies textually parallel
 * makes a future divergence between the kernel-lock path and the
 * userspace-CS path a deliberate, reviewable diff instead of a shared
 * function silently growing a special case for one caller.
 */
SYSCALL_DEFINE0(ivh_cs_enter)
{
	if (!bpf_sched_enabled())
		return 0;
	if (!READ_ONCE(ivh_universal_eligible) || current->ivh_exclude)
		return 0;
	if (!ivh_eval_cooldown_ok())
		return 0;

	/*
	 * Feed the gate's CS-length term with this task's real userspace CS
	 * duration (Step 5's rseq extension) instead of the stale
	 * kernel-lock residue cs_exit() left there. Best-effort: old/short
	 * rseq registration or a faulting read just leaves current->
	 * last_cs_ns as it was -- never fatal, never blocks the migration
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
