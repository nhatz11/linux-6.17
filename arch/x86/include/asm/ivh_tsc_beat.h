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
 * ivh_pv_tier1_confirm (G-LOCK-25 scoping) -- tier 1 (prev->state !=
 * VCPU_RUNNING) fires on ANY halted predecessor, but most halted node
 * predecessors are themselves halted because THEIR OWN predecessor tripped
 * tier 2, not because of independently-confirmed preemption -- one tier-2
 * inference walks down the MCS queue tail one cheap byte load at a time
 * (measured: ~2.77 tier-1 fires per tier-2 fire). A halted vCPU publishes no
 * heartbeat, so is_wait_preempted() on an already-halted prev is exactly a
 * "how long has prev actually been down" freshness check, with no new
 * per-node state needed at all.
 *   0 (default) = upstream tier-1 behavior, bit-identical: any prev->state
 *     != VCPU_RUNNING bails immediately.
 *   1 = SHADOW: run the confirmation check, record agree/disagree, but
 *     STILL BAIL either way -- zero behavior change, exists to measure the
 *     confirmation's selectivity (is it a filter or an indiscriminate
 *     throttle?) before anything real depends on it.
 *   2 = AUTHORITATIVE: an unconfirmed tier-1 trip (heartbeat still reads
 *     fresh) does NOT bail; only a confirmed-stale predecessor does.
 * Only reachable when ivh_adaptive_mode == ADAPTIVE (modes VANILLA and
 * PURE_IPI are untouched); value 2 is refused unless ivh_pv_preempt_src == 2
 * (see ivh_pv_proc_tier1_confirm(), arch/x86/kernel/kvm.c) -- at src == 0,
 * is_wait_preempted() is hardwired to vcpu_is_preempted(), which is
 * hardwired false on any host without a real steal-time page (this one
 * included), so authoritative confirm at src == 0 would silently suppress
 * every single tier-1 bail. Default 0: the sign of this trade is not proven,
 * same posture as ivh_adaptive_irqoff_bail_gate and ivh_pv_spin_threshold's
 * own "bail later" experiment, which measured ~9% SLOWER.
 */
extern unsigned long ivh_pv_tier1_confirm;

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
DECLARE_PER_CPU(u64, ivh_beat_tier1_fired);
/*
 * Split accounting for ivh_lock_halt: it is incremented unconditionally
 * inside ivh_pv_wait() regardless of which qspinlock_paravirt.h call site
 * invoked pv_wait(), so its aggregate hlt_cycles/hlt_events cannot tell
 * apart two structurally different sources:
 *   - pv_wait_node(): waiting on an MCS queue PREDECESSOR. Threshold-
 *     sensitive at ivh_adaptive_mode==ADAPTIVE (only halts if pv_wait_early()
 *     fired).
 *   - pv_wait_head_or_lock(): waiting for the actual lock HOLDER. Always
 *     spins exactly SPIN_THRESHOLD then halts, unconditionally, in every
 *     mechanism -- this path has NO adaptive logic at all.
 * These two counters tag which call site actually reached pv_wait(), so a
 * threshold sweep can see whether the sensitive path's halt volume moves at
 * all, instead of that signal being diluted by the insensitive path.
 */
DECLARE_PER_CPU(u64, ivh_halt_from_node);
DECLARE_PER_CPU(u64, ivh_halt_from_head);
/*
 * Tier-2 observability at src==2 (found missing via independent review,
 * GLOCK-9): every OTHER tier-2 diagnostic counter (ivh_beat_agree_*,
 * false_pos/neg, the age histograms) is computed only up to the `if (src ==
 * 2) return beat;` early-exit in is_wait_preempted() -- i.e. only at src==1
 * ("shadow mode", which never actually changes real behavior). At src==2,
 * the ONLY configuration where tier 2 can affect a real decision, none of
 * that existed: tier 2's fire rate was structurally uncountable. These two
 * are incremented unconditionally for every src!=0 call (both src==1 and
 * src==2), before that early-exit, so a live threshold sweep can measure
 * `tier2_fired / tier2_checked` directly instead of inferring it.
 */
DECLARE_PER_CPU(u64, ivh_beat_tier2_checked);
DECLARE_PER_CPU(u64, ivh_beat_tier2_fired);
/*
 * Spin-iteration accounting (GLOCK-10): ivh_lock_halt only measures time
 * spent AFTER a wait has already decided to sleep -- it says nothing about
 * how many SPIN_THRESHOLD iterations were burned busy-spinning beforehand,
 * which is where early-bail's actual value proposition lives (fewer wasted
 * cpu_relax() iterations, not a faster or slower wake once halted). These
 * record SPIN_THRESHOLD - loop (iterations actually spent) at the exact
 * point each inner spin loop gives up on lock-free acquisition, for both
 * call sites:
 *   - ivh_node_spin_*: pv_wait_node() (queue-predecessor wait). Threshold-
 *     sensitive at ivh_adaptive_mode==ADAPTIVE -- an early bail (tier1 or
 *     tier2) should show a LOWER average than tier1-only, if early bail is
 *     doing its job.
 *   - ivh_head_spin_*: pv_wait_head_or_lock() (lock-holder wait). Has no
 *     early-bail logic in any mechanism -- expected to average almost
 *     exactly SPIN_THRESHOLD always, as a sanity-check control on the
 *     accounting itself.
 */
DECLARE_PER_CPU(u64, ivh_node_spin_iters_sum);
DECLARE_PER_CPU(u64, ivh_node_spin_attempts);
DECLARE_PER_CPU(u64, ivh_head_spin_iters_sum);
DECLARE_PER_CPU(u64, ivh_head_spin_attempts);
/*
 * Denominator-completeness fix (GLOCK-11, found by independent review of
 * GLOCK-10's data): ivh_node_spin_iters_sum/attempts above only recorded
 * passes that bailed early or exhausted the budget -- a pass that acquired
 * the lock via the node->locked return in pv_wait_node()'s inner loop was
 * silently excluded from both the sum and the attempt count, biasing the
 * "iters per attempt" average toward only the unsuccessful subpopulation.
 * These record the same SPIN_THRESHOLD - loop quantity at that excluded
 * return site, so (ivh_node_spin_iters_sum + ivh_node_spin_success_iters_sum)
 * / (ivh_node_spin_attempts + ivh_node_spin_success_attempts) is the
 * complete, unbiased average over every inner-loop pass.
 */
DECLARE_PER_CPU(u64, ivh_node_spin_success_iters_sum);
DECLARE_PER_CPU(u64, ivh_node_spin_success_attempts);
DECLARE_PER_CPU(s64, ivh_beat_min_age);
DECLARE_PER_CPU(u64, ivh_beat_age_hist_running[IVH_BEAT_AGE_HIST_BUCKETS]);
DECLARE_PER_CPU(u64, ivh_beat_age_hist_preempted[IVH_BEAT_AGE_HIST_BUCKETS]);
/*
 * Unconditional raw age histogram, populated for src==1 AND src==2 alike,
 * not split by any ground truth -- see kernel/locking/qspinlock_paravirt.h's
 * is_wait_preempted() for why this exists (the two above are gated behind
 * src==1's ground-truth comparison, which is dead on any host without a
 * real steal-time page, this one included).
 */
DECLARE_PER_CPU(u64, ivh_beat_age_hist_raw[IVH_BEAT_AGE_HIST_BUCKETS]);

/*
 * IVH Idea 2 (head-role takeover) Stage 0 counters -- observe-only, see
 * kernel/locking/qspinlock_paravirt.h's pv_wait_node()/pv_wait_head_or_lock().
 * ivh_head_arm ~= ivh_halt_from_head is the Stage-0 sanity check (same site).
 * try/ok split by tier so tier-1 (predecessor not running) and tier-2
 * (TSC-heartbeat stale) can be compared as independent candidate triggers
 * before Stage 1 commits to either one.
 */
DECLARE_PER_CPU(u64, ivh_head_arm);
DECLARE_PER_CPU(u64, ivh_head_yield_try_tier1);
DECLARE_PER_CPU(u64, ivh_head_yield_ok_tier1);
DECLARE_PER_CPU(u64, ivh_head_yield_try_tier2);
DECLARE_PER_CPU(u64, ivh_head_yield_ok_tier2);
DECLARE_PER_CPU(u64, ivh_head_woke_yielded);
DECLARE_PER_CPU(u64, ivh_head_woke_moot);
/*
 * IVH Idea 2 Stage 0b counters (2026-09-08) -- the head-still-SPINNING window,
 * which the counters above are structurally blind to. HEAD_ARMED is only set
 * once the head has already burned its whole SPIN_THRESHOLD and stored
 * VCPU_HASHED, so ivh_head_yield_try_tier2 can never fire in that window and
 * measured 0 against 5210 tier-1 fires. These count the earlier and more
 * interesting case instead: tier 2 catching a head whose vCPU was preempted
 * MID-SPIN, while it still believes itself VCPU_RUNNING.
 *
 *   ivh_head_spin_enter               - head spin-loop entries; the
 *                                       denominator, and the HEAD_SPINNING
 *                                       mirror of ivh_head_arm. Includes the
 *                                       re-entries caused by lock stealing.
 *                                       ivh_head_spin_enter -
 *                                       ivh_head_spin_attempts = spin loops
 *                                       that ended in an acquire rather than
 *                                       in exhaustion.
 *   ivh_head_yield_try_tier2_spinning - tier 2 fired against a still-spinning
 *                                       head. Compare against
 *                                       ivh_head_yield_try_tier1, NOT summed
 *                                       with it: different windows.
 *   ivh_head_yield_ok_tier2_spinning  - of those, the lock read free -- the
 *                                       Stage-1 opportunity rate for this
 *                                       window.
 *   ivh_head_spinning_prearm          - HEAD_SPINNING seen with state !=
 *                                       VCPU_RUNNING, i.e. the head's short
 *                                       VCPU_HASHED-to-HEAD_ARMED gap. A
 *                                       hygiene counter, expected small;
 *                                       excluded from the two above so the
 *                                       tier-2 window stays clean.
 */
DECLARE_PER_CPU(u64, ivh_head_spin_enter);
DECLARE_PER_CPU(u64, ivh_head_yield_try_tier2_spinning);
DECLARE_PER_CPU(u64, ivh_head_yield_ok_tier2_spinning);
DECLARE_PER_CPU(u64, ivh_head_spinning_prearm);

/*
 * Mode-collapse canaries (2026-09-05 rebuild; contract updated
 * G-LOCK-22-hybrid): the wake-vehicle contract is machine-checkable, not
 * just documented. Exactly one of these increments per
 * __pv_queued_spin_unlock_slowpath() wake, and the vehicle is now a
 * function of (mode, ivh_pv_allowed()), NOT of mode alone:
 *   ivh_mode_uses_hypercall(mode) true  -> ivh_wake_hypercall only,
 *                                          ivh_wake_ipi == 0
 *   ivh_mode_uses_hypercall(mode) false -> ivh_wake_ipi only,
 *                                          ivh_wake_hypercall == 0
 * Concretely: mode PURE_IPI is unconditionally the second row; modes VANILLA
 * and ADAPTIVE are BOTH the first row whenever ivh_pv_allowed(), otherwise
 * the second -- treated identically, since ivh_pv_allow is a simulated-
 * environment override that has to look the same to every mode that
 * consults the real feature bit, not just to ADAPTIVE. So ivh_wake_hypercall
 * > 0 while ivh_adaptive_mode == ADAPTIVE is EXPECTED when PV is allowed --
 * this is the one bit of the pre-G-LOCK-22-hybrid contract that changed; do
 * not mistake it for the bug this canary exists to catch. See
 * <asm/qspinlock.h>'s ivh_mode_uses_hypercall().
 *
 * One more exception, VANILLA-only: when !ivh_pv_allowed(), VANILLA sends
 * NEITHER vehicle (see ivh_wake_vanilla_nopv_noop below) -- it is the only
 * mode whose wait side never halts in that case, so it has nothing to wake.
 */
DECLARE_PER_CPU(u64, ivh_wake_hypercall);
DECLARE_PER_CPU(u64, ivh_wake_ipi);
/*
 * Pairs with ivh_wait_vanilla_nopv_spin below. Mode VANILLA's !ivh_pv_allowed()
 * wake site: correctly a no-op (nobody halted), counted so "did we ever
 * accidentally IPI here" has a direct answer instead of inferring it from
 * ivh_wake_ipi staying zero for an unrelated reason.
 */
DECLARE_PER_CPU(u64, ivh_wake_vanilla_nopv_noop);
/*
 * Set for the whole duration of ivh_pv_wait()'s PV-native-halt branch (not
 * just the halt() call), cleared before every return from it. Renamed from
 * ivh_vanilla_inflight (G-LOCK-22-hybrid): mode ADAPTIVE now takes this same
 * branch whenever ivh_pv_allowed(), so it must be tracked too, not just
 * mode VANILLA. A live ivh_adaptive_mode write whose OLD mode used the
 * hypercall and NEW mode doesn't drains against this: the non-hypercall
 * modes never send KVM_HC_KICK_CPU, so a CPU already committed to a bare,
 * RFLAGS.IF=0 halt() when the mode flips would otherwise have no wake
 * vehicle left at all -- the 2026-07-24 hard-freeze class. See
 * ivh_pv_proc_adaptive_mode() / ivh_mode_uses_hypercall().
 */
DECLARE_PER_CPU(u32, ivh_pv_halt_inflight);
/*
 * G-LOCK-22-hybrid partition counters for ivh_pv_wait(). Every
 * ivh_pv_wait_calls falls into EXACTLY ONE of these five -- asserting that
 * exhaustive partition in the test harness is what proves no sub-population
 * is silently uncounted, the exact failure mode that produced weeks of null
 * tier-2 A/B results earlier in this project:
 *   ivh_wait_pv_halt_irqoff    - PV-native halt, IRQs already off at entry
 *                                (bare halt(), mode VANILLA or ADAPTIVE)
 *   ivh_wait_pv_halt_irqon     - PV-native halt, IRQs on at entry
 *                                (safe_halt(), mode VANILLA or ADAPTIVE)
 *   ivh_wait_ipi_halt_irqon    - IPI-wake halt, IRQs on at entry
 *                                (safe_halt(), mode PURE_IPI always, or
 *                                ADAPTIVE without PV)
 *   ivh_wait_irqoff_nohalt     - no hypercall available, IRQs already off:
 *                                busy-spin only (mode PURE_IPI, or ADAPTIVE
 *                                without PV -- see below)
 *   ivh_wait_vanilla_nopv_spin - mode VANILLA with !ivh_pv_allowed(): busy-
 *                                spin only, unconditionally (VANILLA never
 *                                even checks irqs_disabled() in this case)
 */
DECLARE_PER_CPU(u64, ivh_wait_pv_halt_irqoff);
DECLARE_PER_CPU(u64, ivh_wait_pv_halt_irqon);
DECLARE_PER_CPU(u64, ivh_wait_ipi_halt_irqon);
/*
 * Counts the one irreducible gap left once PV-native halt covers the IF=0
 * case whenever it's allowed: a waiter that reaches ivh_pv_wait() with IRQs
 * already disabled AND no hypercall available (mode PURE_IPI always, or mode
 * ADAPTIVE with !ivh_pv_allowed()) cannot halt at all (no hypercall means no
 * pv_unhalted latch, and a maskable IPI cannot wake an IF=0 HLT) and instead
 * falls through to an uninstrumented cpu_relax() loop. Large values mean a
 * given workload's irqsave-held-lock population is making that config
 * materially less halt-y than PV-native, which matters for interpreting any
 * comparison against it.
 */
DECLARE_PER_CPU(u64, ivh_wait_irqoff_nohalt);
DECLARE_PER_CPU(u64, ivh_wait_vanilla_nopv_spin);
/*
 * pv_wait_early()'s G-LOCK-22-hybrid early-bail suppression firing count
 * (kernel/locking/qspinlock_paravirt.h) -- see ivh_adaptive_irqoff_bail_gate
 * (arch/x86/kernel/kvm.c) for what this gates and why it defaults off.
 * Must-have, not decoration: without it, ivh_beat_tier1_fired's denominator
 * silently loses a population whenever the gate is on.
 */
DECLARE_PER_CPU(u64, ivh_earlybail_suppressed);

/*
 * G-LOCK-25 scoping: per-bail-cause halt-duration accounting for
 * pv_wait_node()'s pv_wait() call (kernel/locking/qspinlock_paravirt.h).
 * Behavior-neutral -- these are read, never acted on. Answers the question
 * ivh_lock_halt's aggregate hlt_cycles/hlt_events cannot: does the blocked
 * duration for a TIER-1-caused halt actually clear the fixed cost of taking
 * one (a real hypercall/vmexit round trip), or is tier 1 (mostly cascading
 * off tier-2-induced halts, see ivh_pv_tier1_confirm above) paying that fixed
 * cost for waits that were about to resolve anyway?
 *
 * Indexed by enum pv_bail_cause. PV_BAIL_TIER1_AGREED/_DISAGREED are only
 * distinguished when ivh_pv_tier1_confirm != 0; at confirm == 0 every tier-1
 * bail is recorded as plain PV_BAIL_TIER1 (no verdict computed, none to
 * record). Exhaustive partition: sum over all PV_BAIL_* must equal
 * ivh_halt_from_node exactly -- assert this in the test harness.
 */
enum pv_bail_cause {
	PV_BAIL_NONE = 0,
	PV_BAIL_TIER1,			/* confirm==0: no verdict computed */
	PV_BAIL_TIER1_AGREED,		/* confirm!=0: heartbeat ALSO reads stale */
	PV_BAIL_TIER1_DISAGREED,	/* confirm!=0: heartbeat reads FRESH */
	PV_BAIL_TIER2,
	PV_BAIL_EXHAUST,		/* SPIN_THRESHOLD ran out, no early bail */
	PV_BAIL_COUNT
};

DECLARE_PER_CPU(u64, ivh_node_halt_cycles[PV_BAIL_COUNT]);
DECLARE_PER_CPU(u64, ivh_node_halt_events[PV_BAIL_COUNT]);
DECLARE_PER_CPU(u64, ivh_node_halt_hist[PV_BAIL_COUNT][IVH_BEAT_AGE_HIST_BUCKETS]);

/*
 * ivh_pv_tier1_confirm's own counters -- see the knob's comment above for
 * the 0/1/2 semantics. _checked/_agreed/_disagreed only increment at
 * confirm != 0 (mirrors ivh_beat_tier2_checked/_fired's own posture: cost is
 * paid only when someone might act on the answer). ivh_tier1_suppressed is
 * confirm==2 only -- the count of tier-1 trips that did NOT bail.
 */
DECLARE_PER_CPU(u64, ivh_tier1_confirm_checked);
DECLARE_PER_CPU(u64, ivh_tier1_confirm_agreed);
DECLARE_PER_CPU(u64, ivh_tier1_confirm_disagreed);
DECLARE_PER_CPU(u64, ivh_tier1_suppressed);

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
 * A HLT taken from ivh_pv_wait() (mode VANILLA's PV_UNHALT path, modes
 * PURE_IPI/ADAPTIVE's safe_halt()) is invisible to tick_nohz's idle
 * accumulators, because it is not the idle loop's own HLT. Left unmeasured,
 * that time would be misbooked as phantom steal by anything that infers
 * steal from elapsed-minus-accounted-busy. This struct measures it at the
 * source so a later step's steal correction has the number to subtract;
 * nothing in this step reads these counters yet.
 *
 * `depth` makes begin/end nest-safe: a hardirq taken during an IF=1
 * safe_halt() can itself reach a contended spinlock and re-enter
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
