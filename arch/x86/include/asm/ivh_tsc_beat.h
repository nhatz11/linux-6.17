/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_IVH_TSC_BEAT_H
#define _ASM_X86_IVH_TSC_BEAT_H

/*
 * IVH rebuild Step 4 (tools/bpf/docs/ivh_rebuild_plan.md sec 4): this is a
 * DELIBERATELY TRIMMED port of production's <asm/ivh_tsc_beat.h>. That file
 * mixes three independent subsystems in one header:
 *
 *   1. The per-CPU TSC heartbeat (struct ivh_tsc_beat) -- the candidate
 *      replacement for pv_wait_early()'s vcpu_is_preempted(prev->cpu).
 *      IN SCOPE for Step 4 (sec 1.4 item 5).
 *   2. struct ivh_lock_halt -- HLT/poll cycle accounting for
 *      ivh_pv_wait()'s mechanism==0/2 halt paths. IN SCOPE for Step 4 (it is
 *      called unconditionally from ivh_pv_wait(), item 2); its consumer
 *      (phantom-steal correction in the capacity engine) is Step 6/8
 *      material and is NOT ported here, so these counters accumulate
 *      unread for now -- same posture as Step 2's holder-table counters.
 *   3. struct ivh_cs_beat (the CS-preemption-stamp predicate), EXCLUDED:
 *      sec 1.7's artifact list bundles CS-stamp + holder-identity together
 *      as "fully wired, large, but default-OFF, never enabled in
 *      production, predicate has a measured hard ceiling of 78.57%
 *      sensitivity". Not reachable from anything this step ports.
 *
 * Do not "restore" section 3 above without re-reading sec 1.7 first.
 *
 * IVH rebuild Step 6 addendum: the raw TSC<->ns conversion helpers
 * (ivh_raw_tsc/ivh_tsc_cycles_to_ns/ivh_tsc_ns_to_cycles) were originally
 * left out of Step 4 under the same Part-C umbrella as struct ivh_cs_beat,
 * but turn out to be genuinely shared, low-level primitives: Step 6's
 * ivh_tick_steal_accumulate() (sec 1.5 item 5, the shipped ivh_steal_source=2
 * estimator) operates in raw TSC cycles too and needs them directly, with
 * zero dependency on Part C's rq->ivh_vact_capacity or its jump-detection
 * logic. Added below; Part C itself (the struct-rq capacity field, its tick
 * function, and its sysctls) remains fully excluded.
 */

#include <linux/cache.h>
#include <linux/compiler.h>
#include <linux/math64.h>
#include <linux/percpu.h>
#include <linux/types.h>
#include <asm/tsc.h>

/*
 * ---------------------------------------------------------------------------
 * IVH per-CPU TSC heartbeat (Plan 1, tools/bpf/docs/
 * ivh_tsc_heartbeat_refcycles_build_plans_2026-07-26.md sec 2)
 * ---------------------------------------------------------------------------
 *
 * A candidate in-guest replacement for pv_wait_early()'s
 * vcpu_is_preempted(prev->cpu) (kernel/locking/qspinlock_paravirt.h). Each
 * vCPU stamps `stamp` with a raw rdtsc() whenever it is demonstrably executing
 * guest code; a reader concludes "that vCPU is not running" when the stamp has
 * aged past ivh_pv_beat_threshold. Staleness IS the signal, so a stale read
 * is fine by construction and no seqlock is needed -- x86-64 aligned 8-byte
 * accesses are single-copy atomic, so a torn read is impossible.
 *
 * Own cacheline, one writer/many remote readers, deliberately NOT a field in
 * struct rq -- see production's comment for the full "wrong neighbour"
 * argument; unchanged here, just not reproduced verbatim.
 */
struct ivh_tsc_beat {
	u64 stamp;		/* raw rdtsc() of this CPU's last publish */
} ____cacheline_aligned_in_smp;

DECLARE_PER_CPU_ALIGNED(struct ivh_tsc_beat, ivh_tsc_beat);

/*
 * ivh_pv_preempt_src -- 0 = KVM steal bit only (default, bit-identical to
 *   pre-heartbeat behavior); 1 = shadow: compute both, count agreement, still
 *   RETURN the KVM bit; 2 = the heartbeat is authoritative. Values > 2, and a
 *   write of 2 before every online CPU has published at least once, are both
 *   rejected by the proc handler in arch/x86/kernel/kvm.c.
 * ivh_pv_beat_threshold -- staleness threshold in RAW TSC CYCLES. Calibrated
 *   at late_initcall from tsc_khz.
 * ivh_pv_beat_publish_mask -- the qspinlock spin loops publish when
 *   (loop & mask) == 0. Must stay COARSER THAN OR EQUAL TO
 *   PV_PREV_CHECK_MASK (0xff).
 */
extern unsigned long ivh_pv_preempt_src;
extern unsigned long ivh_pv_beat_threshold;
extern unsigned long ivh_pv_beat_publish_mask;

/*
 * Shadow-comparator validation counters and the threshold-tuning histograms,
 * defined in arch/x86/kernel/kvm.c. Plain DEFINE_PER_CPU(u64, ...) rather
 * than lockevent_*: CONFIG_LOCK_EVENT_COUNTS is not set on this build.
 *
 * The two histograms are the whole threshold-tuning method: age samples are
 * split by what the HOST says at the same instant, so the correct threshold
 * is wherever the two distributions separate.
 *
 * ivh_beat_min_age is the cross-vCPU TSC drift guard (build plan sec 2.8):
 * the minimum (now - beat) this READER CPU has ever observed.
 */
#define IVH_BEAT_AGE_HIST_BUCKETS 32
DECLARE_PER_CPU(u64, ivh_beat_agree_true);
DECLARE_PER_CPU(u64, ivh_beat_agree_false);
DECLARE_PER_CPU(u64, ivh_beat_false_pos);
DECLARE_PER_CPU(u64, ivh_beat_false_neg);
DECLARE_PER_CPU(u64, ivh_beat_publishes);
DECLARE_PER_CPU(s64, ivh_beat_min_age);
DECLARE_PER_CPU(u64, ivh_beat_age_hist_running[IVH_BEAT_AGE_HIST_BUCKETS]);
DECLARE_PER_CPU(u64, ivh_beat_age_hist_preempted[IVH_BEAT_AGE_HIST_BUCKETS]);

/*
 * Publish this CPU's heartbeat. rdtsc(), NOT rdtsc_ordered() -- this is a
 * heartbeat, not a fence, and rdtsc_ordered()'s LFENCE would be pure cost on
 * every tick and every spin-loop publish.
 *
 * this_cpu_write() rather than WRITE_ONCE(this_cpu_ptr(...)): a single
 * %gs-relative store on x86, atomic with respect to preemption by
 * construction (legal from the halt-exit sites in arch/x86/kernel/kvm.c,
 * where this_cpu_ptr()'s CONFIG_DEBUG_PREEMPT check would be a spurious
 * warning), and single-copy atomic against the remote readers below.
 */
static __always_inline void ivh_tsc_beat_publish(void)
{
	this_cpu_write(ivh_tsc_beat.stamp, rdtsc());
}
#define ivh_tsc_beat_publish ivh_tsc_beat_publish

/*
 * Age of @cpu's heartbeat in raw TSC cycles, as seen from here.
 *
 * SIGNED subtraction, deliberately: a small negative cross-vCPU TSC skew
 * must read as "fresh", not wrap to a huge positive and read as "preempted
 * forever".
 */
static __always_inline s64 ivh_beat_age(int cpu)
{
	u64 beat = READ_ONCE(per_cpu(ivh_tsc_beat, cpu).stamp);

	return (s64)(rdtsc() - beat);
}

static __always_inline bool ivh_beat_stale(int cpu)
{
	return ivh_beat_age(cpu) > (s64)READ_ONCE(ivh_pv_beat_threshold);
}

/*
 * ---------------------------------------------------------------------------
 * ivh_lock_halt -- HLT-taken-outside-the-idle-loop accounting.
 * ---------------------------------------------------------------------------
 *
 * A HLT taken from ivh_pv_wait() (mechanism 0's PV_UNHALT path, mechanism
 * 2's scoped halt) or a bounded TPAUSE/PAUSE poll (the non-halting
 * mechanisms) is invisible to tick_nohz's idle accumulators, because it is
 * not the idle loop's own HLT. Left unmeasured, that time would be
 * misbooked as phantom steal by anything that infers steal from
 * elapsed-minus-accounted-busy. This struct measures it at the source so a
 * later step's steal correction has the number to subtract; nothing in
 * this step reads these counters yet.
 *
 * `depth` makes begin/end nest-safe: a hardirq taken during mechanism 2's
 * IF=1 safe_halt() can itself reach a contended spinlock and re-enter
 * ivh_pv_wait(). The outer interval wins; the nested one adds nothing and
 * subtracts nothing.
 */
struct ivh_lock_halt {
	u64 start;		/* rdtsc() at outermost begin; 0 == nothing in flight */
	u64 hlt_cycles;		/* cumulative: real HLT taken from pv_wait() */
	u64 poll_cycles;	/* cumulative: bounded TPAUSE/PAUSE poll in pv_wait() */
	u64 hlt_events;
	u64 poll_events;
	u32 depth;
	u8  in_poll;		/* which bucket `start` belongs to */
} ____cacheline_aligned_in_smp;

DECLARE_PER_CPU_ALIGNED(struct ivh_lock_halt, ivh_lock_halt);

/*
 * raw_cpu_ptr(), not this_cpu_ptr(): every caller runs with preemption
 * already disabled (the qspinlock slowpath), but this_cpu_ptr()'s
 * CONFIG_DEBUG_PREEMPT check does not know that and would be a spurious
 * warning rather than a bug -- exactly the reasoning ivh_tsc_beat_publish()
 * already documents.
 */
static __always_inline void ivh_lock_halt_begin(bool poll)
{
	struct ivh_lock_halt *h = raw_cpu_ptr(&ivh_lock_halt);

	if (h->depth++)
		return;			/* nested: the outer interval covers us */

	h->in_poll = poll;
	h->start = rdtsc();
}

static __always_inline void ivh_lock_halt_end(void)
{
	struct ivh_lock_halt *h = raw_cpu_ptr(&ivh_lock_halt);
	u64 start, delta;

	if (!h->depth || --h->depth)
		return;

	start = h->start;
	h->start = 0;
	if (!start)
		return;

	delta = rdtsc() - start;
	if (h->in_poll) {
		h->poll_cycles += delta;
		h->poll_events++;
	} else {
		h->hlt_cycles += delta;
		h->hlt_events++;
	}
}

#define ivh_lock_halt_begin ivh_lock_halt_begin

/*
 * Raw TSC <-> ns conversion, shared by the heartbeat above and by
 * ivh_tick_steal_accumulate() (kernel/sched/core.c). OPTIMIZER_HIDE_VAR is
 * required, not decorative: mul_u64_u32_div()'s generic C implementation
 * does a 64-bit division whose two operand registers the compiler can
 * otherwise prove are related when the input is a compile-time-visible
 * function of a previous read of the same variable, folding the division
 * into a shift/multiply pair that is wrong for an arbitrary tsc_khz. Making
 * the operand opaque prevents that miscompile. Cost is at most one register
 * move on a path that already issues a 64-bit divide.
 */
static __always_inline u64 ivh_raw_tsc(void)
{
	return rdtsc();
}
#define ivh_raw_tsc ivh_raw_tsc

static __always_inline u64 ivh_tsc_cycles_to_ns(u64 cycles)
{
	u32 khz = tsc_khz;

	if (unlikely(!khz))
		return 0;

	OPTIMIZER_HIDE_VAR(cycles);
	return mul_u64_u32_div(cycles, USEC_PER_SEC, khz);
}
#define ivh_tsc_cycles_to_ns ivh_tsc_cycles_to_ns

static __always_inline u64 ivh_tsc_ns_to_cycles(u64 ns)
{
	u32 khz = tsc_khz;

	if (unlikely(!khz))
		return 0;

	OPTIMIZER_HIDE_VAR(ns);
	return mul_u64_u32_div(ns, khz, USEC_PER_SEC);
}
#define ivh_tsc_ns_to_cycles ivh_tsc_ns_to_cycles

#endif /* _ASM_X86_IVH_TSC_BEAT_H */
