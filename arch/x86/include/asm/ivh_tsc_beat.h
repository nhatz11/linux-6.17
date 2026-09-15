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
 * For ivh_cs_owner_enable and the ivh_cs_owner_stamp()/_clear() gates. That
 * API deliberately lives in an arch-neutral, dependency-free header because
 * its call sites are <asm/qspinlock.h> and kernel/locking/qspinlock.c, neither
 * of which can reach this file -- see that header's own writeup. (The claim
 * there that this include already exists was aspirational; it is added here.)
 */
#include <linux/ivh_lock_holder.h>

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
 * ============================================================================
 * Handoff-time rotation (lock skipping) -- PHASE 0, DETECT ONLY
 * ============================================================================
 *
 * The idea being measured: at the MCS handoff point, the thread that has just
 * acquired the lock (set_locked() at kernel/locking/qspinlock.c:447) is about
 * to promote its immediate successor via
 * arch_mcs_spin_unlock_contended(&next->locked) at :455. If that successor's
 * vCPU is currently host-preempted, the promotion produces a "dead head": a
 * queue head that cannot run, stalling everyone behind it for the full
 * preemption. The proposal is for the acquirer to instead rotate the first
 * LIVE waiter into the successor position and promote that one -- exactly
 * CNA's cna_order_queue() shape, with "vCPU runnable" substituted for "same
 * NUMA node". See tools/bpf/docs/ivh_handoff_rotation_feasibility_2026-09-13.md
 * (docs repo) for the full safety argument and the CNA/ShflLock derivation.
 *
 * PHASE 0 WRITES NOTHING. It only answers the economic question that decides
 * whether Phase 1 is worth building at all:
 *
 *   ivh_rot_preempted / ivh_rot_handoffs   how often is the handoff target
 *                                          actually preempted?
 *   ivh_rot_depth_hist[]                   when it is, how far down is the
 *                                          first live waiter?
 *
 * If ivh_rot_preempted/ivh_rot_handoffs is small, the whole mechanism is dead
 * on economics and no risky pointer-rewriting code ever gets written -- the
 * same discipline that correctly closed the tier-1 halt-cascade work.
 *
 * Gated by ivh_pv_rot_probe (default 0), read with likely(!...) first, so the
 * cost at the default setting is one predicted branch on a read-mostly global
 * -- identical posture to ivh_beat_publish_in_spin() and
 * ivh_adaptive_irqoff_bail_gate.
 */
extern unsigned long ivh_pv_rot_probe;

/*
 * Hop cap for the forward scan. MANDATORY, not defensive: a ->next chain can
 * be stale or (under a concurrent enqueue) transiently cyclic, and this is
 * diagnostic code that must never become an unbounded loop inside a spinlock
 * hold. The walk itself cannot fault -- every ->next is either NULL or a
 * pointer into a per-CPU qnodes[] slot that is never freed, only reused.
 */
#define IVH_ROT_HOP_CAP 8

DECLARE_PER_CPU(u64, ivh_rot_handoffs);	   /* handoffs examined */
DECLARE_PER_CPU(u64, ivh_rot_preempted);   /* ...whose target looked preempted */
DECLARE_PER_CPU(u64, ivh_rot_no_live);	   /* ...with no live node within the cap */
DECLARE_PER_CPU(u64, ivh_rot_tail_stop);   /* scan stopped early: candidate was
					    * the tail (next == NULL), which Phase 1
					    * must never splice */
/*
 * Depth of the first live waiter, 0 == the immediate successor itself was
 * live. Index IVH_ROT_HOP_CAP is the "none found" bucket. A flat array, not
 * log2-bucketed like the cycle histograms: hop count is already a small
 * bounded integer, not a wide-dynamic-range duration.
 */
DECLARE_PER_CPU(u64, ivh_rot_depth_hist[IVH_ROT_HOP_CAP + 1]);

/*
 * Phase 0b: LOCK IDLE TIME -- the cost-per-event half that Phase 0 was blind
 * to, and the only number that converts "N dead-head events per second" into
 * "X% of wall time".
 *
 * DO NOT measure promotion->ack (the obvious choice, and the wrong one). A
 * promoted successor does not acquire the lock, it becomes the queue head and
 * then spins; if the promoter is still inside its critical section for that
 * whole window, NOTHING is wasted -- the successor would have waited anyway.
 * Worse, pv_kick_node() does not kick (the only pv_kick() is in
 * __pv_queued_spin_unlock_slowpath()), so a halted successor is by
 * construction not woken until the promoter unlocks -- making its
 * promotion->ack interval bounded below by the entire critical section, at
 * zero cost. Timing that would inflate exactly the population Phase 1 wants
 * to act on.
 *
 * What is actually recoverable is the interval where the lock is FREE and its
 * designated head is absent:
 *
 *	t_release (previous holder drops lock->locked)
 *	  ... lock sits idle, nobody can use it ...
 *	t_acquire (queue head observes it free and claims it)
 *
 * Split by whether that head had been flagged stale at ITS promotion
 * (pv_node.rot_flags), the difference between the two distributions is the
 * time handoff rotation could have recovered.
 */
struct ivh_rot_rel {
	void	*lock;	/* which lock was released, and the validity flag:
			 * NULL == slot empty. Compared, NEVER dereferenced --
			 * __pv_queued_spin_unlock_slowpath() documents that the
			 * lock memory may be freed and reused immediately after
			 * the releasing store. */
	u64	tsc;
} ____cacheline_aligned_in_smp;
/*
 * Written remotely (by whoever releases a lock to this CPU), read locally.
 * Aligned for the same reason struct ivh_tsc_beat is: without it this shares a
 * line with unrelated per-CPU state and every release false-shares with it.
 */
DECLARE_PER_CPU_ALIGNED(struct ivh_rot_rel, ivh_rot_rel);

/*
 * ---------------------------------------------------------------------------
 * IVH critical-section owner stamp -- is_cs_preempted()'s input
 * ---------------------------------------------------------------------------
 *
 * Written by a vCPU at the instant it acquires a CONTENDED qspinlock through
 * the MCS queue-head path (kernel/locking/qspinlock.c:462, the one site every
 * MCS-handoff predecessor provably passes through -- see
 * tools/bpf/docs/ivh_is_cs_preempted_build_plan_2026-09-14.md sec 1 for the
 * proof). Read remotely by the NEXT queue head, which reaches this CPU's slot
 * through its own `prev->cpu`.
 *
 * ->lock does double duty. It is the identity of the hold AND its validity
 * flag: a reader that finds a different pointer here knows its `prev` has
 * moved on to some other lock and must abstain. It is COMPARED, NEVER
 * DEREFERENCED -- __pv_queued_spin_unlock_slowpath() documents that lock
 * memory may be freed and reused the instant the releasing store lands.
 *
 * NOT a reuse of struct ivh_rot_rel above, deliberately: that one is written
 * remotely and read locally (the exact opposite direction, so sharing a line
 * would false-share every access), its ->lock means "released TO you" rather
 * than "held BY me", and it is still armed by the separate, live
 * ivh_pv_rot_probe sysctl.
 *
 * Own cacheline for the same one-writer/many-remote-readers reason as
 * struct ivh_tsc_beat.
 */
struct ivh_cs_owner {
	void	*lock;	/* the qspinlock this CPU is holding; NULL == none */
	u64	tsc;	/* raw rdtsc() at the moment of acquisition */
	u64	last_cs;	/* G-LOCK-31: this CPU's last completed stamped hold, cycles; 0 == unknown */
} ____cacheline_aligned_in_smp;

DECLARE_PER_CPU_ALIGNED(struct ivh_cs_owner, ivh_cs_owner);

/*
 * One scheduler tick in raw TSC cycles, and the margin in ticks.
 *
 * DERIVED at late_initcall from tsc_khz and HZ (kvm.c), never hardcoded: the
 * same kernel must answer correctly on a host with a different TSC, and
 * ivh_pv_beat_calibrate() (kvm.c:1556) is the standing precedent. At
 * tsc_khz = 2200000 and HZ = 1000 this is 2200000 cycles.
 *
 * ivh_cs_owed_ticks is the margin, default 2 rather than 1. A hold that began
 * one cycle after tick N is owed tick N+1 within one full period, so 1 period
 * is the theoretical floor; the second period is slack for hrtimer jitter,
 * for tick_sched_do_timer()'s MAX_STALLED_JIFFIES=5 forced-update behaviour
 * (kernel/time/tick-sched.c:204,236-239), and for cross-vCPU TSC skew. It
 * costs reaction time -- ~2-3 ms -- and buys the "no false positives from
 * long critical sections" property that is this predicate's entire claim.
 */
extern unsigned long ivh_cs_tick_period;
extern unsigned long ivh_cs_owed_ticks;

/*
 * Promptness bound for the RUNNING-at-handoff tenure-0 gate (build plan sec
 * 1.2 c-RUNNING): a head abstains for the whole tenure if, at the moment its
 * pending bit is committed, more than this many cycles have passed since its
 * predecessor's acquisition stamp. Derived at late_initcall as
 * IVH_CS_PROMPT_US microseconds from tsc_khz (default 9 us = ~20000 cycles
 * here), and meant to be re-set from ivh_cs_prompt_hist[] once Stage A has
 * data. It NARROWS the residual race; it does not close it. Only the
 * release-side clear (ivh_cs_owner_clear) does.
 */
extern unsigned long ivh_cs_prompt_cycles;

/*
 * Arms the queue head's detect-and-count probe in pv_wait_head_or_lock()
 * (kernel/locking/qspinlock_paravirt.h). Read once per head tenure. Stage A:
 * counts only, never changes control flow. Refused by its sysctl handler
 * unless ivh_cs_owner_enable == 1 and ivh_pv_rot_enable == 0.
 */
extern unsigned long ivh_cs_head_probe;

/*
 * Stage B: THE only behaviour knob. With ivh_cs_head_probe == 1 and
 * ivh_adaptive_mode == IVH_MODE_ADAPTIVE, a fired is_cs_preempted() breaks the
 * queue head out of its spin loop into the existing clear_pending() ->
 * pv_hash() -> pv_wait() halt path early. Read once per head tenure. Default
 * 0; refused by its sysctl handler unless both preconditions hold and
 * ivh_pv_rot_enable == 0.
 */
extern unsigned long ivh_cs_head_bail;

/* rot_flags bits deposited into the successor's pv_node at promotion time. */
#define IVH_ROT_F_STALE		0x1	/* target looked preempted/halted */
#define IVH_ROT_F_SKIPPABLE	0x2	/* ...and a live node existed to skip to */
#define IVH_ROT_NR_CLASS	4	/* rot_flags & 0x3 */

/*
 * [class][log2(cycles)] where class is (rot_flags & 0x3).
 *
 * HOW TO READ THIS -- it is NOT a baseline subtraction, and treating it as one
 * would be wrong. Only the hashed release path is instrumentable (see
 * ivh_rot_stamp_release()), so EVERY sample here, class 0 included, is a head
 * that had already halted. Class 0 is therefore not a "healthy" control and
 * class_N minus class_0 is not a meaningful quantity: both terms are dominated
 * by the same pv_kick + vCPU-wake round trip.
 *
 * What is meaningful is the ABSOLUTE idle time of class
 * IVH_ROT_F_STALE|IVH_ROT_F_SKIPPABLE. For that class the promoter saw a stale
 * successor AND a live node behind it, so rotation would have handed the lock
 * to a spinning waiter that could take it at once, instead of to a sleeper the
 * lock must now wait out. The whole interval is recoverable, with nothing to
 * subtract.
 *
 * Class IVH_ROT_F_STALE alone (no live node found) is the same waste with no
 * remedy available -- it bounds how much rotation could NOT have helped.
 * Class 0 is idle time rotation would never have acted on at all, because the
 * successor looked live at the moment the decision would have been taken.
 */
DECLARE_PER_CPU(u64, ivh_rot_idle_hist[IVH_ROT_NR_CLASS][IVH_BEAT_AGE_HIST_BUCKETS]);
DECLARE_PER_CPU(u64, ivh_rot_idle_cycles[IVH_ROT_NR_CLASS]);
DECLARE_PER_CPU(u64, ivh_rot_idle_events[IVH_ROT_NR_CLASS]);

/*
 * Acquisitions the idle clock could NOT be attributed to a release: either we
 * were the first node queued (no predecessor to read a stamp from) or the
 * predecessor's slot names a different lock, which happens when a stealer got
 * in between. This is the honesty gate on the whole measurement -- if it is a
 * large fraction of ivh_rot_idle_events[], the histograms are not
 * representative and must not be quoted.
 */
DECLARE_PER_CPU(u64, ivh_rot_idle_unknown);

/*
 * idle came out negative -- the stamp is written just after the releasing
 * store, so a head woken by anything other than that kick can acquire inside
 * the window. Counted rather than silently dropped because the discards are
 * preferentially SHORT intervals, and dropping those quietly biases every
 * reported mean upward.
 */
DECLARE_PER_CPU(u64, ivh_rot_idle_backward);

/*
 * idle exceeded 100x the staleness threshold and was rejected as an artifact
 * of a freed-and-reallocated lock matching a surviving stamp. Must be small;
 * if it is not, the address-matching scheme is not sound for that workload and
 * the histograms should not be quoted.
 */
DECLARE_PER_CPU(u64, ivh_rot_idle_capped);

/*
 * pv_hybrid_queued_unfair_trylock() successes. CONFIG_LOCK_EVENT_COUNTS is off
 * in this build, so upstream's pv_lock_stealing is unavailable -- and this is
 * the counter that decides whether rotation is even needed: a newly arriving
 * waiter stealing a free lock ALREADY recovers the throughput that a dead head
 * would otherwise waste. If this is large, rotation is competing with a
 * mechanism the tree already has.
 */
DECLARE_PER_CPU(u64, ivh_rot_steals);

/*
 * ============================================================================
 * Handoff-time rotation -- PHASE 1, THE ACTUAL SPLICE
 * ============================================================================
 *
 * SEPARATE knob from ivh_pv_rot_probe on purpose. The probe answers "how often
 * would this fire and how much is on the table"; this one is the only thing
 * that ever rewrites an MCS ->next pointer. With ivh_pv_rot_enable == 0 the
 * lock behaves EXACTLY as it did in Phase 0 -- and with both knobs 0,
 * pv_handoff_rotate() returns on its first branch, bit-identical to upstream.
 *
 * WHAT IT DOES. At the handoff point the acquirer holds the lock and still
 * owns its qnode, and the queue behind it is
 *
 *	us -> A -> ... -> P -> B -> C -> ... -> T
 *
 * with A (the immediate successor) stale and B the first live node found by
 * the forward walk. Rotation makes it
 *
 *	us -> B -> A -> ... -> P -> C -> ... -> T
 *
 * with exactly two pointer stores -- P->next = C and B->next = A -- and then
 * promotes B instead of A. A loses one turn; nobody is removed from the queue,
 * nothing is reordered relative to the tail, and the lock word is not touched.
 *
 * WHY THE TWO STORES ARE SAFE. Two facts, both established from
 * kernel/locking/qspinlock.c and neither of them assumed:
 *
 * (1) A qnode's ->next is written at most twice per tenure: once to NULL at
 *     qspinlock.c:344, BEFORE smp_wmb() + xchg_tail() publishes the node where
 *     anyone could reach it, and once by the single arriving waiter whose
 *     xchg_tail() returned this node's tail code (qspinlock.c:380,
 *     WRITE_ONCE(prev->next, node)). Exactly one waiter ever obtains a given
 *     tail code, so a ->next that reads NON-NULL has had its one and only
 *     in-queue write already, has no second writer pending, and is therefore
 *     stable and safe for us to overwrite. A ->next that reads NULL is the
 *     opposite: either this node is the tail, or a waiter has already done
 *     xchg_tail() and is about to store into it. Writing it would either be
 *     clobbered (orphaning the node we spliced in, which then never gets
 *     ->locked and hangs the queue) or clobber the arriving waiter (which then
 *     never gets ->locked and hangs). Hence: NEVER splice through a NULL
 *     ->next. Both stores above target a node whose ->next we have just read
 *     as non-NULL, so neither can race an enqueue.
 *
 * (2) Every node behind us is frozen for the whole operation. Between
 *     WRITE_ONCE(prev->next, node) at qspinlock.c:380 and
 *     arch_mcs_spin_lock_contended(&node->locked) at :383 there is no exit
 *     path -- no goto, no return, no break -- and the only writer of a node's
 *     ->locked is its MCS predecessor. So no node behind us can end its
 *     tenure, free its qnode slot or re-enter the queue until we perform the
 *     promotion store, which we do after the splice. The chain we walk is
 *     therefore stable and acyclic, its memory is a never-freed per-CPU
 *     qnodes[] slot, and lock stealing does not reach it: both
 *     pv_hybrid_queued_unfair_trylock() call sites (qspinlock.c:324 and :352)
 *     run BEFORE xchg_tail(), i.e. only for waiters that are not in the queue.
 *
 * A corollary of (1) that matters: because neither spliced node was the tail,
 * and the tail code only ever moves to later arrivals, no future enqueue can
 * ever target them either. The tail, and the lock word, are left alone.
 *
 * MEMORY ORDERING. None beyond what the call site already has. Both splice
 * stores precede arch_mcs_spin_unlock_contended(&next->locked), which is an
 * smp_store_release, so any node that observes ->locked == 1 through the
 * matching smp_cond_load_acquire observes the rewritten chain, and the
 * release-acquire chain carries it transitively to A when B later releases A.
 *
 * PV WAKEUP. pv_kick_node() follows the promotion and is passed the rotated
 * successor, so B -- not A -- is the node advanced to VCPU_HASHED and put in
 * the hash table under _Q_SLOW_VAL. A is simply left halted, which is correct:
 * A has not been promoted and has nothing to wake for. A's wakeup is
 * guaranteed by B, which cannot take the (val & _Q_TAIL_MASK) == tail
 * fast-release path (B is provably not the tail -- we only rotate to a node
 * whose ->next is non-NULL) and so must run the full
 * arch_mcs_spin_unlock_contended(&A->locked) + pv_kick_node(lock, A) sequence.
 * Exactly one node per lock is hashed at a time, as before.
 *
 * STALE prev POINTERS. A goes on polling our (the promoter's) pn->state from
 * pv_wait_node(), and C goes on polling B's, for longer than upstream's window.
 * That is a heuristic input to pv_wait_early() only -- the wait loop re-reads
 * node->locked unconditionally every iteration -- and pn->cpu of a recycled
 * qnode is still that CPU's id, so the worst outcome is a needless early halt,
 * never a missed wakeup.
 */
extern unsigned long ivh_pv_rot_enable;

/*
 * Starvation bound. Rotation is a fairness violation by construction, so it
 * needs an explicit cap or a permanently-stale successor can be skipped
 * forever. pv_node.rot_flags has bits 0-1 taken by IVH_ROT_F_*; bits 2-7 hold
 * a per-tenure count of how many times THIS node has been rotated past. When
 * it reaches ivh_pv_rot_skip_max we refuse to rotate and promote it anyway.
 *
 * Counting only the immediate successor is sufficient, and this is the bound:
 * every rotation increments the position-1 node's counter, so at most
 * ivh_pv_rot_skip_max rotations can occur before the position-1 node is
 * force-promoted. A node at position p therefore waits at most
 * p * (ivh_pv_rot_skip_max + 1) handoffs -- finite, and p is itself bounded in
 * practice by the fact that only the first IVH_ROT_HOP_CAP nodes are ever
 * reachable by the walk.
 *
 * The counter is reset by being overwritten with fresh class bits at the
 * moment the node is promoted, and by pv_init_node()'s unconditional
 * rot_flags = 0 at the start of every tenure. 0 disables rotation entirely
 * (no node may ever be skipped even once); values above IVH_ROT_SKIP_MAX are
 * clamped so the counter cannot overflow into the class bits.
 */
extern unsigned long ivh_pv_rot_skip_max;

#define IVH_ROT_SKIP_SHIFT	2
#define IVH_ROT_SKIP_MAX	((1U << (8 - IVH_ROT_SKIP_SHIFT)) - 1)	/* 63 */

/*
 * The TRUE addressable opportunity count: stale successor, AND a live node
 * behind it, AND that live node's own ->next is non-NULL so the splice is
 * actually legal under rule (1) above. ivh_rot_depth_hist[] only ever proved
 * the first two, which overstates what Phase 1 can act on.
 *
 * Counted whenever the walk runs -- under ivh_pv_rot_probe alone, so the real
 * opportunity rate is measurable with rotation still switched off.
 */
DECLARE_PER_CPU(u64, ivh_rot_splice_ok);

/* Rotations actually performed. Requires ivh_pv_rot_enable. */
DECLARE_PER_CPU(u64, ivh_rot_splice_done);

/*
 * Wanted to rotate, refused because the live node's ->next read NULL (it is
 * the tail, or an enqueue is in flight into it). The difference between this
 * and ivh_rot_splice_ok is how much of the raw opportunity the NULL-->next
 * safety rule costs us; on short queues it is expected to be most of it.
 */
DECLARE_PER_CPU(u64, ivh_rot_splice_blocked_tail);

/*
 * Refused because the successor had already been rotated past
 * ivh_pv_rot_skip_max times. This is the starvation bound firing; if it is a
 * large fraction of ivh_rot_splice_ok the cap is doing most of the deciding
 * and the mechanism is not behaving as designed.
 */
DECLARE_PER_CPU(u64, ivh_rot_splice_blocked_starve);

/*
 * is_cs_preempted() Stage A -- DETECT ONLY. Two exhaustive partitions, and the
 * harness must assert both with ZERO deviation (these are integer counts taken
 * on one CPU with no sampling between them, so "within 0.1%" is too weak here):
 *
 *   ivh_cs_check_calls == ivh_cs_abstain_tenure + ivh_cs_abstain_hashed
 *                       + ivh_cs_abstain_late
 *                       + ivh_cs_abstain_noprev + ivh_cs_abstain_rot
 *                       + ivh_cs_abstain_tag    + ivh_cs_abstain_skew
 *                       + ivh_cs_abstain_young  + ivh_cs_long_hold
 *   ivh_cs_long_hold   == ivh_cs_abstain_nohz + ivh_cs_abstain_retag
 *                       + ivh_cs_healthy_long + ivh_cs_fired
 *
 * The three TENURE-GATE abstains (tenure/hashed/late) are per-CHECK counts of a
 * verdict taken once per head tenure by ivh_cs_tenure_gate(), so they scale
 * with spin length. To size coverage per TENURE use the ivh_cs_tenure0_*
 * counters below instead.
 *
 * ivh_cs_long_hold is the FORM-0 population -- "this hold is longer than
 * ivh_cs_owed_ticks ticks" with no liveness term -- and exists solely as the
 * denominator of the false-positive audit. ivh_cs_healthy_long is the holds
 * that were long AND whose holder beat within the last ivh_cs_owed_ticks
 * ticks, i.e. the ones form 0 would have fired on and form 2 correctly
 * exonerates. If
 * ivh_cs_healthy_long / ivh_cs_long_hold is near zero, the tick term is inert
 * and this predicate has silently degenerated into form 0. See
 * ivh_tsc_full_redesign_build_plan_2026-07-29.md sec 1.2 for why that matters.
 *
 * ivh_cs_abstain_nohz MUST read exactly 0 on this host: neither nohz_full= nor
 * dynticks (nohz=off) is in effect. A nonzero value means the command line
 * changed and every other number in the run is suspect.
 *
 * ivh_cs_abstain_skew counts (now - acq) <= 0, i.e. the remote stamp is in our
 * future. Expected ~0 on a TD with a synchronised TSC; a material rate
 * invalidates the whole design, not just this counter.
 */
DECLARE_PER_CPU(u64, ivh_cs_stamps);
DECLARE_PER_CPU(u64, ivh_cs_stamp_overwrote);
DECLARE_PER_CPU(u64, ivh_cs_check_calls);
DECLARE_PER_CPU(u64, ivh_cs_abstain_noprev);
/* G-LOCK-30: holder lookup for no-predecessor heads (subset of check_calls, not a partition term) */
DECLARE_PER_CPU(u64, ivh_cs_fast_lookup_hit);
DECLARE_PER_CPU(u64, ivh_cs_fast_lookup_miss);
extern unsigned long ivh_cs_owner_fast;
/*
 * G-LOCK-31 knobs.
 *   ivh_pv_tier2_enable  1 (default) = today; 0 = non-head waiters never call
 *                        is_wait_preempted() (tier 2 and the tier-1 confirm).
 *   ivh_cs_scan          0 (default) = holder identity from prev only;
 *                        1 = if prev is absent or its slot does not name the
 *                        lock, scan the per-CPU owner slots. Needs owner_clear.
 *   ivh_cs_criterion     0 (default) = G-LOCK-29/30 test: held > owed ticks AND
 *                        holder heartbeat older than owed ticks;
 *                        1 = held > holder CPU's last CS + ivh_cs_noise_cycles.
 *   ivh_cs_noise_cycles  the noise constant for criterion 1.
 */
extern unsigned long ivh_pv_tier2_enable;
extern unsigned long ivh_cs_scan;
extern unsigned long ivh_cs_criterion;
extern unsigned long ivh_cs_noise_cycles;
DECLARE_PER_CPU(u64, ivh_rot_stop_halted);	/* walk met a VCPU_HALTED waiter at hop >= 1 and stopped */
DECLARE_PER_CPU(u64, ivh_cs_scan_hit);
DECLARE_PER_CPU(u64, ivh_cs_scan_miss);
DECLARE_PER_CPU(u64, ivh_cs_abstain_nolastcs);	/* criterion 1: holder CPU has no last CS yet (partition 1 term) */
DECLARE_PER_CPU(u64, ivh_cs_bail_suppressed);	/* hit, but already hashed and SLOW_VAL gone: nobody would wake us */
DECLARE_PER_CPU(u64, ivh_cs_abstain_rot);
DECLARE_PER_CPU(u64, ivh_cs_abstain_tag);
DECLARE_PER_CPU(u64, ivh_cs_abstain_skew);
DECLARE_PER_CPU(u64, ivh_cs_abstain_young);
DECLARE_PER_CPU(u64, ivh_cs_abstain_nohz);
DECLARE_PER_CPU(u64, ivh_cs_long_hold);
DECLARE_PER_CPU(u64, ivh_cs_healthy_long);
DECLARE_PER_CPU(u64, ivh_cs_fired);
DECLARE_PER_CPU(u64, ivh_cs_abstain_tenure);	/* waitcnt >= 1 without the clear */
DECLARE_PER_CPU(u64, ivh_cs_abstain_hashed);	/* HASHED entry, _Q_SLOW_VAL witness failed */
DECLARE_PER_CPU(u64, ivh_cs_abstain_late);	/* RUNNING entry, promptness gate failed */
DECLARE_PER_CPU(u64, ivh_cs_abstain_retag);	/* tag changed between first and second read */
DECLARE_PER_CPU(u64, ivh_cs_clears);

/*
 * Per-TENURE soundness-gate accounting (build plan sec 1.2). Counted once per
 * tenure-0 head entry that has a prev and no rotation, in BOTH clear modes, so
 * the size of each hole is measured directly rather than inferred:
 *
 *   ivh_cs_tenure0_enter           - denominator
 *   ivh_cs_tenure0_hashed          - entered with pn->state == VCPU_HASHED
 *                                    (was halted at handoff)
 *   ivh_cs_tenure0_hashed_released - ...and lock->locked != _Q_SLOW_VAL after
 *                                    the pending commit: prev had ALREADY
 *                                    released. This IS the hole found in the
 *                                    first version of the theorem, counted.
 *   ivh_cs_tenure0_late            - RUNNING entry whose stamp age at the
 *                                    pending commit exceeded
 *                                    ivh_cs_prompt_cycles
 *   ivh_cs_shadow_gate_pass_released - (clear==1 only) RUNNING entry whose tag
 *                                    was ALREADY cleared at the pending commit
 *                                    but whose stamp age would have PASSED the
 *                                    promptness gate: the residual race the
 *                                    clear==0 configuration would have
 *                                    admitted. Approximate; see sec 1.4.
 *   ivh_cs_prompt_hist[]           - log2 stamp age at the pending commit for
 *                                    RUNNING entries with a matching tag: the
 *                                    data ivh_cs_prompt_cycles is tuned from.
 */
DECLARE_PER_CPU(u64, ivh_cs_tenure0_enter);
DECLARE_PER_CPU(u64, ivh_cs_tenure0_hashed);
DECLARE_PER_CPU(u64, ivh_cs_tenure0_hashed_released);
DECLARE_PER_CPU(u64, ivh_cs_tenure0_late);
DECLARE_PER_CPU(u64, ivh_cs_shadow_gate_pass_released);
DECLARE_PER_CPU(u64, ivh_cs_prompt_hist[IVH_BEAT_AGE_HIST_BUCKETS]);

/*
 * EPISODE accounting -- the part this project got wrong once and must not get
 * wrong again.
 *
 * Handoff rotation reported 3104 events/s and was worth approximately nothing,
 * because the DURATION of those events was never measured, and because its
 * event counter was sampled every PV_PREV_CHECK_MASK (0xff) iterations so one
 * stall was counted repeatedly by the same waiter. Both defects are structural
 * here, not incidental, so both are designed out:
 *
 *   - An EPISODE is keyed on the HOLDER'S acquisition TSC. A second, third and
 *     256th fire against the same acq stamp extend the open episode; they do
 *     not open a new one. Over-counting by re-sampling is therefore impossible
 *     by construction, not by convention.
 *   - ivh_cs_fired / ivh_cs_ep_events IS the over-count factor rotation never
 *     computed. Report it.
 *   - A new acq stamp (the holder changed) CLOSES the open episode and opens a
 *     fresh one, so an episode can never span two holds.
 *
 * Closed at three exits, kept separate because they mean different things:
 *   ACQUIRED        - we got the lock. duration = the time we spun after
 *                     detecting a dead holder. THIS IS THE RECOVERABLE TIME
 *                     and the only number the go/no-go in sec 5 turns on.
 *   HOLDER_CHANGED  - a different acq stamp appeared while we still spun. A
 *                     true upper bound on what was recoverable.
 *   EXHAUST         - we ran out of spin budget and are about to pv_wait().
 *                     TRUNCATED: a lower bound, never a measurement.
 */
#define IVH_CS_EP_ACQUIRED	0
#define IVH_CS_EP_HOLDER_CHANGED 1
#define IVH_CS_EP_EXHAUST	2
#define IVH_CS_EP_NR		3

DECLARE_PER_CPU(u64, ivh_cs_ep_events);
DECLARE_PER_CPU(u64, ivh_cs_ep_events_by_end[IVH_CS_EP_NR]);
DECLARE_PER_CPU(u64, ivh_cs_ep_cycles[IVH_CS_EP_NR]);
DECLARE_PER_CPU(u64, ivh_cs_ep_hist[IVH_CS_EP_NR][IVH_BEAT_AGE_HIST_BUCKETS]);

/*
 * CONTROL. Without this the episode numbers are unfalsifiable: a detected
 * episode being 300 us long means nothing unless undetected head tenures are
 * shorter. Index 0 = no detection during this tenure, 1 = at least one. Closed
 * at the same three exits, measuring the WHOLE tenure, not just the episode.
 *
 * ivh_cs_prev_hold_hist is the population-correct denominator for the
 * false-positive audit: every time a head acquires a lock whose predecessor's
 * stamp it could read, it records how long that predecessor actually held it.
 * That is the real distribution of CONTENDED hold durations -- which is the
 * only population this predicate ever judges. If ivh_cs_ep_hist sits inside
 * the bulk of this distribution we are firing on normal long holds; if it sits
 * in a separate mode above its p99.9, we are firing on anomalies. Obtained for
 * free on the observer side, so it needs no release-path instrumentation.
 */
DECLARE_PER_CPU(u64, ivh_cs_tenure_cycles[2]);
DECLARE_PER_CPU(u64, ivh_cs_tenure_hist[2][IVH_BEAT_AGE_HIST_BUCKETS]);
DECLARE_PER_CPU(u64, ivh_cs_prev_hold_hist[IVH_BEAT_AGE_HIST_BUCKETS]);

/* Stage B only. Head halts split by cause, mirroring ivh_node_halt_record(). */
#define IVH_CS_HALT_EXHAUST	0
#define IVH_CS_HALT_CS		1
#define IVH_CS_HALT_NR		2
DECLARE_PER_CPU(u64, ivh_cs_head_bailed);
DECLARE_PER_CPU(u64, ivh_head_spin_iters_bail_sum);
DECLARE_PER_CPU(u64, ivh_head_spin_bail_attempts);
DECLARE_PER_CPU(u64, ivh_head_halt_cycles[IVH_CS_HALT_NR]);
DECLARE_PER_CPU(u64, ivh_head_halt_events[IVH_CS_HALT_NR]);
DECLARE_PER_CPU(u64, ivh_head_halt_hist[IVH_CS_HALT_NR][IVH_BEAT_AGE_HIST_BUCKETS]);

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
