/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_IVH_TSC_BEAT_H
#define _ASM_X86_IVH_TSC_BEAT_H

#include <linux/cache.h>
#include <linux/compiler.h>	/* OPTIMIZER_HIDE_VAR(), see the conversions below */
#include <linux/ivh_lock_holder.h>
#include <linux/log2.h>
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
 * vcpu_is_preempted(prev->cpu) (kernel/locking/qspinlock_paravirt.h).  Each
 * vCPU stamps `stamp` with a raw rdtsc() whenever it is demonstrably executing
 * guest code; a reader concludes "that vCPU is not running" when the stamp has
 * aged past ivh_pv_beat_threshold.  Staleness IS the signal, so a stale read
 * is fine by construction and no seqlock is needed -- x86-64 aligned 8-byte
 * accesses are single-copy atomic, so a torn read is impossible.
 *
 * Why this is a SEPARATE header rather than living in <asm/qspinlock.h> next
 * to ivh_pv_wait_mechanism: rdtsc() means <asm/tsc.h>, which pulls
 * asm/msr.h -> linux/percpu.h -> linux/sched.h.  Reaching linux/sched.h from
 * inside asm/qspinlock.h defines the generic vcpu_is_preempted(int) fallback
 * before the x86 vcpu_is_preempted(long) is finished, and the build dies.
 * See the pointer comment left behind in <asm/qspinlock.h>.
 *
 * State the motivation plainly (build plan sec 1): the thing this replaces
 * costs ONE cmpb on a percpu byte, needs zero tuning, and is
 * host-authoritative.  This buys independence from the kvm_steal_time struct
 * and nothing else.  It is a portability / research-independence argument,
 * NOT a performance one, and it is expected to lose on throughput.  Default
 * OFF (ivh_pv_preempt_src == 0) reproduces today's behavior bit-for-bit and
 * does not even compute the heartbeat.
 *
 * Deliberately NOT a field in struct rq.  rq's vSched block puts
 * clock_preempt shoulder-to-shoulder with preemptions/max_latency/
 * last_preemption/last_idle_tp/ewma_act_ns/prmpt_flags -- all written from the
 * tick or migration path by the owning CPU.  A slot written at kHz-to-MHz
 * rates by its owner and read remotely by every waiter queued behind it is
 * exactly the wrong neighbour for those, so it gets its own cacheline.  The
 * one-writer/many-readers shape is the benign case for a shared line (no
 * writer-writer ping-pong), but every remote read still pulls the line across
 * the interconnect, which is why the Phase 2 publish sites in
 * kernel/locking/qspinlock_paravirt.h are rate-limited AND sysctl-gated.
 */
struct ivh_tsc_beat {
	u64 stamp;		/* raw rdtsc() of this CPU's last publish */
} ____cacheline_aligned_in_smp;

DECLARE_PER_CPU_ALIGNED(struct ivh_tsc_beat, ivh_tsc_beat);

/*
 * ivh_pv_preempt_src -- 0 = KVM steal bit only (default, bit-identical to
 *   pre-heartbeat behavior); 1 = shadow: compute both, count agreement, still
 *   RETURN the KVM bit; 2 = the heartbeat is authoritative.  Values > 2, and a
 *   write of 2 before every online CPU has published at least once, are both
 *   rejected by the proc handler in arch/x86/kernel/kvm.c.
 * ivh_pv_beat_threshold -- staleness threshold in RAW TSC CYCLES.  Calibrated
 *   at late_initcall from tsc_khz to 1500 us, i.e. deliberately the exact
 *   cycle-equivalent of the `> 1500000` ns in is_cpu_preempted()
 *   (kernel/sched/cputime.c), so that Phase 1 is a controlled comparison
 *   against the signal this tree already has and any divergence between the
 *   two is a bug rather than a new signal.
 * ivh_pv_beat_publish_mask -- Phase 2 only: the qspinlock spin loops publish
 *   when (loop & mask) == 0.  Must stay COARSER THAN OR EQUAL TO
 *   PV_PREV_CHECK_MASK (0xff); publishing more often than anyone reads is
 *   pure interconnect waste.
 */
extern unsigned long ivh_pv_preempt_src;
extern unsigned long ivh_pv_beat_threshold;
extern unsigned long ivh_pv_beat_publish_mask;

/*
 * Shadow-comparator validation counters and the threshold-tuning histograms,
 * all defined in arch/x86/kernel/kvm.c and summed into /proc/ivh_debug
 * (kernel/sched/fair.c).  Plain DEFINE_PER_CPU(u64, ...) rather than
 * lockevent_*: CONFIG_LOCK_EVENT_COUNTS is not set on this build, so every
 * lockevent_inc() in qspinlock_paravirt.h compiles to nothing and could not
 * carry these.  Same shape as the existing ivh_pv_wait_calls counter.
 *
 * The two histograms are the whole threshold-tuning method and they replace
 * guessing: age samples are split by what the HOST says at the same instant,
 * so the correct threshold is wherever the two distributions separate (<=1%
 * of _running above it, >=90% of _preempted above it).  If they do not
 * separate, that is the answer, and the answer is that the signal does not
 * work at that write cadence.  Raw counts only -- percentiles are computed in
 * userspace off a before/after delta, exactly like ivh_obs_cs_hist.
 *
 * ivh_beat_min_age is the cross-vCPU TSC drift guard (build plan sec 2.8):
 * the minimum (now - beat) this READER CPU has ever observed.  On
 * offset-aligned TSCs it sits near zero (or slightly negative) and stays
 * there; a minimum that drifts monotonically upward over hours is the drift
 * signature.  Drift matters because it is a silent ONE-DIRECTIONAL
 * false-positive bias -- it never crashes and never reads as a correctness
 * bug, it just quietly makes one vCPU look permanently preempted to its
 * neighbours.  Do not declare TSC comparability closed until this line has
 * been watched across a multi-hour run.
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
 * Publish this CPU's heartbeat.  rdtsc(), NOT rdtsc_ordered() -- this is a
 * heartbeat, not a fence, and rdtsc_ordered()'s LFENCE would be pure cost on
 * every tick and every spin-loop publish.
 *
 * this_cpu_write() rather than WRITE_ONCE(this_cpu_ptr(...)): it is a single
 * %gs-relative store on x86, atomic with respect to preemption by
 * construction (so it is legal from the halt-exit sites in
 * arch/x86/kernel/kvm.c, where this_cpu_ptr()'s CONFIG_DEBUG_PREEMPT check
 * would be a spurious warning rather than a bug), and single-copy atomic
 * against the remote readers below.
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
 * (sec 2.8 -- the ping-pong measurement put the minimum observed offset at
 * 108-126 cycles for every pair, with no growth by CPU distance) must read as
 * "fresh", not wrap to a huge positive and read as "preempted forever".
 *
 * A caller that needs both the age and the verdict must use this and compare
 * for itself rather than also calling ivh_beat_stale(): two rdtsc() reads of
 * the same event would disagree, and the histogram would then be binning a
 * different number from the one the verdict was taken on.
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
 * Raw TSC and cycles->ns, exported through the same "arch may override"
 * idiom as ivh_tsc_beat_publish() (kernel/sched/sched.h supplies the no-op
 * fallbacks off x86).  These exist because Part C (below, and
 * kernel/sched/core.c's ivh_vact_tick()) is TSC-NATIVE END TO END: every
 * quantity it stores is raw cycles, there is no nanosecond variant, and the
 * only conversion in the whole design happens at display time.
 *
 * That is deliberate and it is a correction to an earlier planning round,
 * which compared raw cycles against a nanosecond sched_clock() value -- a
 * literal unit-mismatch bug that would have produced a silently meaningless
 * gate.  Matching the heartbeat's units also means the two signals can be
 * read against each other without a conversion sitting between them, which
 * is exactly what the comparison work in sec 3.9 needs.
 *
 * ivh_tsc_cycles_to_ns() clamps rather than faults when tsc_khz is not yet
 * established: returning 0 makes an unconverted quantity read as "no time
 * has passed", which in every consumer here is the DON'T-ACT direction.
 * mul_u64_u32_div() rather than a plain multiply-then-divide for the reason
 * ivh_ref_accumulate() already documents: the naive form overflows u64 for
 * cumulative cycle counts and truncates silently when it does.
 *
 * ---------------------------------------------------------------------------
 * THE OPTIMIZER_HIDE_VAR() CALLS ARE LOAD-BEARING.  DO NOT DELETE THEM.
 * (root cause of Bug 4, found 2026-08-01 by disassembling the booted vmlinux)
 * ---------------------------------------------------------------------------
 *
 * On x86-64 mul_u64_u32_div() resolves to <asm/div64.h>'s
 *
 *	asm ("mulq %2; divq %3" : "=a" (q)
 *	                        : "a" (a), "rm" (mul), "rm" (div) : "rdx");
 *
 * `a` is TIED to %rax and `mulq` DESTROYS %rax before `divq` reads %3.  The
 * asm does not (and cannot, in GCC's constraint language) say so.  So if the
 * compiler can PROVE that the `mul` or the `div` operand holds the same value
 * as `a`, it is free to satisfy both with the one register -- %rax -- and it
 * emits `divq %rax`, which divides the product by itself.  The quotient is
 * then 1, silently, with no warning anywhere.
 *
 * That is not hypothetical.  ivh_vact_tick() computes
 * ivh_tsc_ns_to_cycles(TICK_NSEC), and at CONFIG_HZ=1000 TICK_NSEC is
 * 1000000 -- numerically identical to the USEC_PER_SEC divisor one line
 * below.  GCC coalesced them and 6.17.0-rseqport68 shipped with a nominal
 * tick period of ONE CYCLE, so ivh_vact_gap_split() booked (avail - 1) cycles
 * of every single tick as STOLEN and exactly 1 cycle as EXECUTING, pinning
 * rq->ivh_vact_capacity at 0 on all 16 vCPUs regardless of real steal.  A
 * disassembly sweep of the whole vmlinux found exactly one victim of the
 * pattern, this one, precisely because equal-valued operands are rare.
 *
 * Hiding the DIVIDEND is sufficient and is the cheapest place to cut: `mul`
 * and `div` are the two operands that can alias %rax, and both can only be
 * proven equal to something the compiler can still see.  Once `a` is opaque
 * no such proof exists, for either operand, in either direction, for any
 * present or future caller.  Cost is at most one register move on a path that
 * already issues a 64-bit divide.
 *
 * Fixing <asm/div64.h> itself would be the upstream-correct repair and would
 * cover the whole tree; it is deliberately NOT done here because that header
 * feeds sched_clock(), timekeeping and cpufreq, and the audit above says IVH
 * is the only caller currently miscompiled.  Revisit if that stops being true.
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

	OPTIMIZER_HIDE_VAR(cycles);	/* see the comment above -- required */
	return mul_u64_u32_div(cycles, USEC_PER_SEC, khz);
}
#define ivh_tsc_cycles_to_ns ivh_tsc_cycles_to_ns

static __always_inline u64 ivh_tsc_ns_to_cycles(u64 ns)
{
	u32 khz = tsc_khz;

	if (unlikely(!khz))
		return 0;

	OPTIMIZER_HIDE_VAR(ns);		/* see the comment above -- required */
	return mul_u64_u32_div(ns, khz, USEC_PER_SEC);
}
#define ivh_tsc_ns_to_cycles ivh_tsc_ns_to_cycles

/*
 * ---------------------------------------------------------------------------
 * IVH critical-section stamp (Build 1, tools/bpf/docs/
 * ivh_tsc_full_redesign_build_plan_2026-07-29.md sec 3.2)
 * ---------------------------------------------------------------------------
 *
 * Own cacheline, one writer, many remote readers -- for exactly the reason
 * spelled out for struct ivh_tsc_beat at the top of this file, and NOT as a
 * field in struct rq: a slot written at every outermost lock acquire and
 * release by its owner, and read remotely by whichever queue head happens to
 * be waiting on a lock this CPU holds, is the wrong neighbour for rq's
 * tick-written vSched block.
 *
 * READ THIS BEFORE TRUSTING THE PREDICATE (build plan sec 1.2, the largest
 * correction in that document).  The stamp is written ONCE, at the start of
 * the hold, and is NEVER refreshed while the hold is in progress.  So its age
 * is "how long has this critical section been running" -- nothing more.  A
 * holder that is executing perfectly happily but has a genuinely long
 * critical section crosses any threshold you pick and reads as preempted.
 * The honest statement of what form 0 below measures is:
 *
 *	"This hold has been running longer than ~99.9% of holds do, so
 *	 SOMETHING is wrong; host preemption is the most likely something."
 *
 * That is a defensible heuristic and it is why the threshold has to come from
 * the observed CS-hold distribution (ivh_cs_hold_hist[]) rather than from the
 * 1500 us used for the WAIT stamp -- the two numbers answer different
 * questions.  But it is emphatically NOT the same predicate as
 * is_wait_preempted(), whose stamp is refreshed at >= 1 kHz from the tick
 * plus per-spin-iteration from the spin loops, and whose age therefore really
 * does mean "time since this vCPU last proved it was executing guest code".
 *
 * Hence TWO predicate forms ship together and the data chooses between them:
 *
 *   form 0 (as originally specified): in a CS, and the CS stamp is older than
 *	ivh_cs_beat_threshold.  Tests hold DURATION.
 *   form 1 (the default, better founded): in a CS, AND the holder's
 *	continuously-refreshed liveness heartbeat has gone stale.  Uses the CS
 *	stamp only for the part it can actually answer ("is this CPU inside a
 *	critical section right now") and delegates "is it running" to the
 *	already-validated staleness predicate.  The tick is a hardirq and fires
 *	regardless of preempt_disable(), so ivh_tsc_beat.stamp stays fresh on a
 *	running CPU even while that CPU holds a spinlock -- form 1's second
 *	term is well defined at exactly the moment it is needed.  Form 1 also
 *	composes the new, unproven piece (holder identity) with a proven piece
 *	(staleness), which is the lower-risk composition.
 *
 * Form 0 is kept because it is what was specified, because it is free to
 * compute once form 1's plumbing exists, and because the two forms'
 * DISAGREEMENT RATE is itself a finding: a large "form 0 fires, form 1 does
 * not" population is a direct measurement of how much of the CS-duration tail
 * is long-but-healthy rather than preempted.
 *
 *   form 2 (added 2026-07-30, after the live 91% false-negative measurement):
 *	holder identity AND the holder's liveness heartbeat is stale -- form 1
 *	with the in-CS term DELETED.  The reason it is worth having is that
 *	form 1's first term is very likely redundant AND coverage-limited at
 *	the only call site there is:
 *
 *	  Redundant, because ivh_cs_head_check() only runs when we are the
 *	  queue head spinning on a lock that IS held.  "Is the holder inside a
 *	  critical section" is already known to be true from the fact that we
 *	  are waiting on it; asking the CS stamp to confirm it adds no
 *	  information.
 *	  Coverage-limited, because the CS stamp and the holder-identity table
 *	  instrument DIFFERENT POPULATIONS.  Identity is stamped at the
 *	  qspinlock layer and therefore covers every acquisition in the kernel;
 *	  the stamp is published from cs_enter() and therefore covers only
 *	  outermost (lock_depth == 1), non-interrupt acquisitions taken through
 *	  the kernel/locking/spinlock.c wrappers.  A holder that is in
 *	  softirq/hardirq context, or holding an inner lock, is correctly
 *	  IDENTIFIED and has no stamp at all -- so form 1 answers "not
 *	  preempted" for it no matter what the host is doing.  That is a
 *	  systematic FALSE NEGATIVE, and the measured 6.17.0-rseqport67
 *	  sensitivity of 123/1423 = 8.6% is exactly the shape it would produce.
 *
 *	Form 2 costs nothing to carry and settles it with an echo instead of a
 *	rebuild: run the same window at form 1 and at form 2 and compare
 *	ivh_cs_sensitivity_pct.  It is NOT the default, because it makes the
 *	predicate depend on holder identity alone and that composition has
 *	never been measured; the discriminating measurement comes first.
 *	Cross-check with the age histograms while doing it -- bucket 0 absorbs
 *	the age < 0 sentinel, so hist_running[0] + hist_preempted[0] against
 *	their totals IS the "holder had no CS stamp" rate.
 *
 * ===========================================================================
 * THE MEASUREMENT WAS RUN (2026-07-30).  FORM 0 WINS AND IS NOW THE DEFAULT.
 * ===========================================================================
 *
 * 6.17.0-rseqport67, hackbench -T -g 1 -f 8 -l 400000 x3 per arm under real
 * host contention, ~27M ivh_cs_head_check() samples per arm,
 * ivh_cs_preempt_src=1 so the arms differed in nothing but which counter was
 * incremented:
 *
 *	form   sensitivity   FPR      precision
 *	  0        34.15%    0.203%     18.36%
 *	  1        10.22%    0.166%      7.68%
 *	  2         6.89%    0.183%      3.34%
 *
 * Form 0 dominates: more than 3x the sensitivity of form 1 AND more than 2x
 * its precision, at an FPR the two share. So the argument above -- that form
 * 1 composes the unproven piece with a proven one and is therefore the safer
 * default -- was wrong, and it was wrong for a reason worth stating plainly,
 * because it invalidates forms 1 and 2 STRUCTURALLY and not just on this
 * workload:
 *
 *   THE HEARTBEAT IS THE WRONG INSTRUMENT FOR A LOCK HOLDER.  is_wait_-
 *   preempted() asks about prev->cpu, an MCS predecessor, which is SPINNING
 *   and therefore republishing at ivh_beat_publish_in_spin()'s ~90 us
 *   cadence.  Forms 1 and 2 ask about the LOCK HOLDER, which by definition is
 *   not spinning -- it is executing inside the critical section -- so the only
 *   thing refreshing its stamp is the 1 kHz tick in account_process_tick().
 *   Same field, same predicate, two populations whose republish rates differ
 *   by more than an order of magnitude.  Everything else follows from that.
 *
 * It is a THRESHOLD problem in the sense that ivh_pv_beat_threshold really
 * does control the sensitivity, over an enormous range -- and it is NOT a
 * threshold problem in the sense that no setting of it is any good.  Sweeping
 * it live against form 2, same workload, same boot:
 *
 *	ivh_pv_beat_threshold   sens%    FPR%    prec%
 *	  3300000 (1500 us)     10.53    0.177    5.90   <-- shipped
 *	  1100000 ( 500 us)     30.75    0.427   11.20
 *	   131072 (  60 us)     36.45    1.304    2.96
 *	    16384 (   7 us)     97.21   12.241    0.99
 *
 * Compare each row against form 0 at MATCHED SENSITIVITY and form 0 wins
 * every time: at ~31% sensitivity form 0 costs 0.12% FPR against form 2's
 * 0.43%, and at ~76% sensitivity form 0 costs 0.64% against form 2's 12.2%.
 * A 1 kHz-refreshed stamp simply cannot be read at microsecond resolution;
 * pushing the threshold down converts running holders into false positives
 * roughly as fast as it converts preempted ones into true ones.  1500 us was
 * never a calibration -- it is is_cpu_preempted()'s number, chosen so Phase 1
 * would be a controlled comparison -- and the honest reading of the sweep is
 * that the constraint it accidentally satisfies (stay above one tick period)
 * is a real one for this population.
 *
 * TWO THINGS THIS DOES *NOT* SHOW, both of which were plausible going in:
 *
 *   NOT "the CS is too short to catch".  hackbench's holds are 100-300 ns
 *   (ivh_obs_cs_hist p50 = 128 ns, mean 175 ns), and the tempting inference
 *   is that a host preemption cannot be observed inside a window that short.
 *   The data says otherwise: ivh_cs_age_hist_preempted[] puts the observed
 *   age of a preempted holder's CS at 3.7 us to 4 ms -- the preemption
 *   STRETCHES the hold by three to four orders of magnitude, and 75.9% of
 *   preempted samples land in a measurable, live-stamped bucket.  The CS
 *   being short is what makes form 0 WORK: a healthy hold almost never
 *   crosses even 1 us, so the separation is enormous.
 *
 *   NOT a holder-identity failure.  ivh_holder_self reads 0 and
 *   unknown_empty/unknown_collision/raced are all populated and plausible,
 *   so the table is doing its job in all three arms.
 *
 * THE ONE THING THAT IS A WORKLOAD ARTEFACT, stated so it is not mistaken for
 * a property of the signal: PREVALENCE.  Only 0.134% of queue-head checks on
 * hackbench are against a holder the host has actually preempted, and
 * ivh_obs_stolen_pct is 0.0005% -- roughly 1 critical section in 200,000.
 * At that prevalence an FPR of 0.2% already means false positives outnumber
 * true ones ~4:1, so 18% precision is close to the arithmetic ceiling for
 * ANY predicate here, and no amount of tuning moves it.  hackbench is a fine
 * workload for measuring the FPR and the running-age distribution, both of
 * which are stable to ~1% run to run.  It is a poor one for measuring
 * sensitivity: the preempted population arrives in bursts, so 3-second runs
 * put sensitivity anywhere from 0% to 55% for a FIXED configuration (this is
 * why the numbers above are aggregated over 3 x 12 s and why the earlier
 * single-run figures in this project's notes disagree with each other and
 * with these).  Validating this signal on its merits needs a workload that
 * actually produces sustained lock-holder preemption.
 */
struct ivh_cs_beat {
	u64 stamp;		/* raw rdtsc() at cs_enter(); 0 == not in a CS */
} ____cacheline_aligned_in_smp;

DECLARE_PER_CPU_ALIGNED(struct ivh_cs_beat, ivh_cs_beat);

/*
 * ivh_cs_preempt_src -- 0 off (default; nothing is stamped, nothing is read,
 *   nothing is counted), 1 shadow (stamp, read, count, compare -- and still
 *   return false so behaviour is unchanged), 2 authoritative (the queue head
 *   really does bail out of its spin).  Same three-valued shape and the same
 *   meanings as ivh_pv_preempt_src above, on purpose: one established idiom
 *   for "flip a signal from measured to trusted", not four ad-hoc ones.
 * ivh_cs_predicate_form -- 0, 1 or 2, per the table above.  Defaults to 0 as
 *   of 2026-07-30 (was 1); the measurement that changed it is in that table.
 * ivh_cs_beat_threshold -- form 0's threshold, in RAW TSC CYCLES, calibrated
 *   at late_initcall from tsc_khz and sysctl-writable so it can be swept live.
 *   The shipped value is a STARTING POINT FOR A SWEEP, not a committed value;
 *   the authoritative calibration is read off the separation point of
 *   ivh_cs_age_hist_running[] against ivh_cs_age_hist_preempted[].
 */
extern unsigned long ivh_cs_preempt_src;
extern unsigned long ivh_cs_predicate_form;
extern unsigned long ivh_cs_beat_threshold;

#define IVH_CS_AGE_HIST_BUCKETS		32
#define IVH_CS_HOLD_HIST_BUCKETS	32
#define IVH_CS_LOOP_HIST_BUCKETS	16

/*
 * Counters, all defined in arch/x86/kernel/kvm.c beside the existing
 * ivh_beat_* set and all summed into /proc/ivh_debug (kernel/sched/fair.c).
 * Plain DEFINE_PER_CPU(u64, ...) rather than lockevent_*, for the reason
 * already documented above: CONFIG_LOCK_EVENT_COUNTS is not set on this
 * build, so every lockevent_inc() in qspinlock_paravirt.h compiles to nothing
 * and could not carry any of these.
 *
 * The list is long ON PURPOSE.  Every entry is here because omitting it would
 * force a "we need one more counter" rebuild+reboot, and avoiding exactly
 * that is the reason this whole build is bundled the way it is.  In detail:
 *
 *   ivh_cs_age_hist_running[] / _preempted[] ARE the threshold-calibration
 *	method and the sign-convention check in one.  Age samples are split by
 *	what the HOST says at the same instant, so the threshold to pick is
 *	wherever the two distributions separate (target: <=1% of _running above
 *	it, >=90% of _preempted above it).  If they do NOT separate, that is
 *	the answer, and the answer is that the predicate does not work at this
 *	write cadence.  And if _preempted does not sit ABOVE _running, the
 *	`age > threshold` sign convention is backwards -- asserted here, not
 *	assumed, and these two lines are what settles it in one boot.
 *   ivh_cs_hold_hist[] is the POPULATION-CORRECT hold-time distribution.  The
 *	existing ivh_obs_cs_hist (kernel/locking/spinlock.c) cannot serve: it
 *	is incremented only under current->ivh_observe, i.e. only for tasks
 *	launched under `ivh_exec -v`, so calibrating from its p99.9 would
 *	calibrate against the BENCHMARK's CS distribution rather than the
 *	kernel's.  This one is fed from every cs_exit() that reaches the
 *	outermost-release block, gated only on ivh_cs_preempt_src != 0.
 *   ivh_cs_clear_mismatch is the direct measurement of how often
 *	_raw_spin_unlock_bh()'s clear-after-release ordering actually bites --
 *	see the pointer-match discussion in kernel/locking/spinlock.c.
 *   ivh_holder_stamps / _clears must track each other closely.  A persistent
 *	imbalance means an ownership-transfer site was missed, which is the
 *	failure mode that corrupts holder identity SILENTLY.
 *   ivh_holder_unknown_empty vs _unknown_collision: physics vs geometry, kept
 *	separate because a lumped counter cannot answer "is the table big
 *	enough".
 *   ivh_holder_raced is the read-verify-read skew rate; ivh_holder_self
 *	should be ~0 RELATIVE TO ivh_cs_checks and to
 *	ivh_holder_unknown_collision -- not literally zero.  Its three causes
 *	(missed release site, caller reentrancy, and the irreducible torn slot
 *	that two interleaved stampers of colliding locks can compose) are
 *	enumerated at the increment site in kernel/locking/qspinlock_paravirt.h.
 *   ivh_head_bail_early / _loop_hist[] / ivh_lock_steals quantify the SIDE
 *	EFFECT of the queue-head bail, which must not be assumed harmless --
 *	see ivh_cs_head_check() in kernel/locking/qspinlock_paravirt.h.
 */
DECLARE_PER_CPU(u64, ivh_cs_checks);
DECLARE_PER_CPU(u64, ivh_cs_publishes);
DECLARE_PER_CPU(u64, ivh_cs_agree_true);
DECLARE_PER_CPU(u64, ivh_cs_agree_false);
DECLARE_PER_CPU(u64, ivh_cs_false_pos);
DECLARE_PER_CPU(u64, ivh_cs_false_neg);
DECLARE_PER_CPU(u64, ivh_cs_clear_mismatch);
DECLARE_PER_CPU(u64, ivh_cs_age_hist_running[IVH_CS_AGE_HIST_BUCKETS]);
DECLARE_PER_CPU(u64, ivh_cs_age_hist_preempted[IVH_CS_AGE_HIST_BUCKETS]);
DECLARE_PER_CPU(u64, ivh_cs_hold_hist[IVH_CS_HOLD_HIST_BUCKETS]);

DECLARE_PER_CPU(u64, ivh_holder_stamps);
DECLARE_PER_CPU(u64, ivh_holder_clears);
DECLARE_PER_CPU(u64, ivh_holder_unknown_empty);
DECLARE_PER_CPU(u64, ivh_holder_unknown_collision);
DECLARE_PER_CPU(u64, ivh_holder_raced);
DECLARE_PER_CPU(u64, ivh_holder_self);

DECLARE_PER_CPU(u64, ivh_head_bail_early);
DECLARE_PER_CPU(u64, ivh_head_bail_loop_hist[IVH_CS_LOOP_HIST_BUCKETS]);
DECLARE_PER_CPU(u64, ivh_lock_steals);

/*
 * Publish / clear this CPU's CS stamp.  this_cpu_write() for the same three
 * reasons ivh_tsc_beat_publish() documents: one %gs-relative store, atomic
 * with respect to preemption by construction, and single-copy atomic against
 * the remote readers.
 *
 * The clear is not optional bookkeeping.  0 is the "not in a critical
 * section" SENTINEL, so leaving a stale value behind would leave this CPU
 * looking permanently mid-hold to every remote reader.
 */
static __always_inline void ivh_cs_beat_publish(void)
{
	this_cpu_write(ivh_cs_beat.stamp, rdtsc());
}

static __always_inline void ivh_cs_beat_clear(void)
{
	this_cpu_write(ivh_cs_beat.stamp, 0);
}

/*
 * Age of @cpu's CS stamp in raw TSC cycles, as seen from here, or a NEGATIVE
 * sentinel when @cpu is not currently inside a critical section.
 *
 * The negative return is load-bearing rather than tidy.  A cleared stamp is
 * 0, so an unguarded `rdtsc() - stamp` would be an enormous positive and
 * would read as "preempted forever" -- the worst possible failure direction,
 * and a silent one.  Callers test `age >= 0` first and bucket 0 absorbs the
 * sentinel, exactly as it already absorbs small negative cross-vCPU TSC skew
 * in the heartbeat histograms.
 *
 * ONE rdtsc(), and a caller that needs both the age and the verdict must use
 * this and compare for itself rather than also calling is_cs_preempted():
 * two readings of the same event disagree, and the histogram would then be
 * binning a different number from the one the verdict was taken on.  Same
 * rule, same reason, as ivh_beat_age() above.
 */
static __always_inline s64 ivh_cs_age(int cpu)
{
	u64 stamp = READ_ONCE(per_cpu(ivh_cs_beat, cpu).stamp);

	if (!stamp)
		return -1;

	return (s64)(rdtsc() - stamp);
}

/*
 * The predicate itself, both forms, for callers that do NOT also want the age
 * for a histogram.  ivh_cs_head_check() deliberately does not use this -- it
 * needs the age and the verdict to come from one reading, so it open-codes
 * the same two expressions off a single ivh_cs_age().  Keep the two in sync
 * by hand if either changes; they are three lines each and a shared helper
 * would have to return both values, which is the thing being avoided.
 */
static __always_inline bool is_cs_preempted(int cpu)
{
	unsigned long form = READ_ONCE(ivh_cs_predicate_form);
	s64 age;

	/*
	 * Form 2 drops the in-CS term entirely -- see the form table above for
	 * why that term is both redundant and coverage-limited at the one call
	 * site this predicate has.  Tested first so that a CPU with no CS stamp
	 * (the exact population form 1 cannot see) is still answered.
	 */
	if (form == 2)
		return ivh_beat_stale(cpu);

	age = ivh_cs_age(cpu);
	if (age < 0)
		return false;			/* not in a CS: never "preempted" */

	if (form)
		return ivh_beat_stale(cpu);

	return age > (s64)READ_ONCE(ivh_cs_beat_threshold);
}

/*
 * ---------------------------------------------------------------------------
 * IVH lock-path non-idle halt accounting
 * ---------------------------------------------------------------------------
 *
 * WHY THIS EXISTS (root cause, 2026-07-27).  ivh_ref_accumulate()
 * (kernel/sched/core.c) infers steal as
 *
 *	stolen = (TSC elapsed) - (REF_TSC elapsed) - (idle+iowait elapsed)
 *
 * REF_TSC is CPU_CLK_UNHALTED.REF: by definition it stops for *any* reason the
 * logical CPU stops executing, not just host descheduling.  The subtraction
 * term is get_cpu_idle_time_us() + get_cpu_iowait_time_us(), which is
 * tick_nohz_stop_idle()'s accounting and therefore covers exactly ONE kind of
 * voluntary stop: the one taken from the *idle loop*.  That was a complete
 * accounting of guest-side halts when the design was written -- and it stopped
 * being complete the moment ivh_pv_wait() (arch/x86/kernel/kvm.c) started
 * halting/napping from inside the qspinlock slowpath:
 *
 *   - mechanism 2 executes a real safe_halt() from pv_wait().  REF_TSC stops.
 *     The idle accumulators do not move (we are not the idle task).  Every one
 *     of those cycles is therefore booked as STOLEN.
 *   - mechanisms 1 and 2 both fall through to the bounded poll, which naps in
 *     __tpause(TPAUSE_C02_STATE) whenever WAITPKG is available (it is, on this
 *     host).  C0.1/C0.2 park the logical processor; if REF_TSC stops there too
 *     -- which is what the counter's own definition implies but which this
 *     tree has not measured -- then the poll manufactures phantom steal at
 *     nearly wall-clock rate for up to IVH_PV_ADAPTIVE_TSC (~1.36 ms) per
 *     pv_wait() call.  That "if" is exactly why the correction is split into
 *     two independently selectable terms below rather than one lump: the
 *     TPAUSE question is settled by an A/B on kernel.ivh_ref_halt_correct
 *     (1 = HLT only, 2 = HLT + poll), not by argument.
 *   - mechanism 0's PV_UNHALT halt()/safe_halt() has the same shape and is
 *     accounted identically, so the correction is not mechanism-specific.
 *
 * Consequence when kernel.ivh_steal_source=1: vcap's
 *	capacity_perc = used / (used + stolen)
 * collapses on every vCPU, set_custom_capacity() drives rq->cpu_capacity down,
 * and ivh_steal_imminent()'s `rq->cpu_capacity > ivh_capacity_threshold`
 * rejection (1010/1024, i.e. a mere 1.4% apparent steal) stops firing --
 * so IVH runs its full selection+migration path on essentially every eligible
 * acquisition instead of only during real host preemption.  That is the
 * "combined is broken, either alone is fine" signature: with
 * ivh_steal_source=0 the phantom number is computed but never consumed, and on
 * a `nopvspin` boot pv_wait() is never reached so the phantom number is never
 * produced.
 *
 * Accounting shape.  A begin/end pair around each halt or nap records raw TSC
 * cycles into a per-CPU cumulative counter that ivh_ref_accumulate() deltas
 * exactly like it deltas TSC/REF_TSC/idle.  `start` is public rather than
 * private so the tick can CLOSE AN IN-FLIGHT INTERVAL (ivh_lock_halt_flush())
 * -- without that, the timer interrupt that un-halts a mechanism-2 waiter
 * would run account_process_tick() -> ivh_ref_accumulate() *before* the
 * matching end(), see the full halt in d_tsc with no d_hlt to pay for it, book
 * it as steal, and then find the correction only on the next tick where the
 * clamp discards it.  Flushing at the tick's own `tsc` keeps all four
 * quantities measured across identical intervals, which is the invariant the
 * whole calculation rests on.
 *
 * Deliberately UNCONDITIONAL (not gated on ivh_ref_steal_enabled), same
 * reasoning as ivh_beat_halt_exit(): two rdtsc on a path whose *cheapest*
 * outcome is a multi-microsecond wait is unmeasurable, gating it would make
 * the counters lie across a live sysctl flip, and leaving it always-on means
 * "how much phantom steal does this configuration produce" is answerable from
 * /proc/ivh_debug at ivh_ref_halt_correct=0, i.e. without changing behavior.
 *
 * `depth` makes begin/end nest-safe: a hardirq taken during mechanism 2's
 * IF=1 safe_halt() can itself reach a contended spinlock and re-enter
 * ivh_pv_wait().  The outer interval wins; the nested one adds nothing and
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
 * raw_cpu_ptr(), not this_cpu_ptr(): every caller runs with preemption already
 * disabled (the qspinlock slowpath) or in hardirq context (the tick), but
 * this_cpu_ptr()'s CONFIG_DEBUG_PREEMPT check does not know that at the
 * halt-exit sites and would be a spurious warning rather than a bug -- exactly
 * the reasoning ivh_tsc_beat_publish() already documents.
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

/*
 * Close the in-flight interval at @now and re-open it there, so that a halt
 * spanning a tick is split across the two accumulate intervals it actually
 * occupies.  Signed compare: @now comes from the caller's own rdtsc(), which
 * is always at or after h->start on this CPU, but refuse a negative rather
 * than wrap.  Does NOT bump *_events -- this is not a new halt.
 */
static __always_inline void ivh_lock_halt_flush(u64 now)
{
	struct ivh_lock_halt *h = raw_cpu_ptr(&ivh_lock_halt);
	u64 start = h->start;

	if (!start || (s64)(now - start) <= 0)
		return;

	if (h->in_poll)
		h->poll_cycles += now - start;
	else
		h->hlt_cycles += now - start;
	h->start = now;
}

#define ivh_lock_halt_begin ivh_lock_halt_begin

#endif /* _ASM_X86_IVH_TSC_BEAT_H */
