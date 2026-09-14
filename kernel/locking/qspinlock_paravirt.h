/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _GEN_PV_LOCK_SLOWPATH
#error "do not include this file"
#endif

#include <linux/hash.h>
#include <linux/memblock.h>
#include <linux/debug_locks.h>
#include <linux/log2.h>
/*
 * IVH TSC heartbeat storage/knobs/counters.  x86-only, and this file is
 * already x86-only in practice for the same reason: it dereferences
 * ivh_pv_wait_mechanism, which only arch/x86 defines.  arch/powerpc's
 * pseries PARAVIRT_SPINLOCKS uses its own arch/powerpc/lib/qspinlock.c and
 * never includes this header, so nothing else is affected.
 */
#include <asm/ivh_tsc_beat.h>

/*
 * Implement paravirt qspinlocks; the general idea is to halt the vcpus instead
 * of spinning them.
 *
 * This relies on the architecture to provide two paravirt hypercalls:
 *
 *   pv_wait(u8 *ptr, u8 val) -- suspends the vcpu if *ptr == val
 *   pv_kick(cpu)             -- wakes a suspended vcpu
 *
 * Using these we implement __pv_queued_spin_lock_slowpath() and
 * __pv_queued_spin_unlock() to replace native_queued_spin_lock_slowpath() and
 * native_queued_spin_unlock().
 */

#define _Q_SLOW_VAL	(3U << _Q_LOCKED_OFFSET)

/*
 * Queue Node Adaptive Spinning
 *
 * A queue node vCPU will stop spinning if the vCPU in the previous node is
 * not running. The one lock stealing attempt allowed at slowpath entry
 * mitigates the slight slowdown for non-overcommitted guest with this
 * aggressive wait-early mechanism.
 *
 * The status of the previous node will be checked at fixed interval
 * controlled by PV_PREV_CHECK_MASK. This is to ensure that we won't
 * pound on the cacheline of the previous node too heavily.
 */
#define PV_PREV_CHECK_MASK	0xff

/*
 * Queue node uses: VCPU_RUNNING & VCPU_HALTED.
 * Queue head uses: VCPU_RUNNING & VCPU_HASHED.
 */
enum vcpu_state {
	VCPU_RUNNING = 0,
	VCPU_HALTED,		/* Used only in pv_wait_node */
	VCPU_HASHED,		/* = pv_hash'ed + VCPU_HALTED */
};

struct pv_node {
	struct mcs_spinlock	mcs;
	int			cpu;
	u8			state;
};

/*
 * Hybrid PV queued/unfair lock
 *
 * By replacing the regular queued_spin_trylock() with the function below,
 * it will be called once when a lock waiter enter the PV slowpath before
 * being queued.
 *
 * The pending bit is set by the queue head vCPU of the MCS wait queue in
 * pv_wait_head_or_lock() to signal that it is ready to spin on the lock.
 * When that bit becomes visible to the incoming waiters, no lock stealing
 * is allowed. The function will return immediately to make the waiters
 * enter the MCS wait queue. So lock starvation shouldn't happen as long
 * as the queued mode vCPUs are actively running to set the pending bit
 * and hence disabling lock stealing.
 *
 * When the pending bit isn't set, the lock waiters will stay in the unfair
 * mode spinning on the lock unless the MCS wait queue is empty. In this
 * case, the lock waiters will enter the queued mode slowpath trying to
 * become the queue head and set the pending bit.
 *
 * This hybrid PV queued/unfair lock combines the best attributes of a
 * queued lock (no lock starvation) and an unfair lock (good performance
 * on not heavily contended locks).
 */
#define queued_spin_trylock(l)	pv_hybrid_queued_unfair_trylock(l)
static inline bool pv_hybrid_queued_unfair_trylock(struct qspinlock *lock)
{
	/*
	 * Stay in unfair lock mode as long as queued mode waiters are
	 * present in the MCS wait queue but the pending bit isn't set.
	 */
	for (;;) {
		int val = atomic_read(&lock->val);
		u8 old = 0;

		if (!(val & _Q_LOCKED_PENDING_MASK) &&
		    try_cmpxchg_acquire(&lock->locked, &old, _Q_LOCKED_VAL)) {
			/*
			 * IVH ownership-transfer site A6 (build plan sec
			 * 3.3.4): PV lock stealing.
			 */
			ivh_lock_set_holder(lock);
			lockevent_inc(pv_lock_stealing);
			/*
			 * ...and a plain per-CPU counter beside it, because
			 * lockevent_inc() above compiles to NOTHING on this
			 * build (CONFIG_LOCK_EVENT_COUNTS is not set; see the
			 * same note on the ivh_beat_* counters in
			 * <asm/ivh_tsc_beat.h>).
			 *
			 * This is not redundancy, it is the only way to answer
			 * a question Build 1 must answer.  ivh_cs_head_check()
			 * below can make the queue head break out of its spin
			 * early, and doing so runs clear_pending() sooner than
			 * it otherwise would -- reopening the very window this
			 * function steals through.  Without a live steal count
			 * the question "did the early bail reopen the stealing
			 * window, and by how much" is unanswerable and would
			 * force a rebuild purely to add a counter, which is
			 * precisely the failure this build is shaped to avoid.
			 *
			 * Unconditional rather than gated on ivh_cs_preempt_src
			 * on purpose: the steal rate at src == 0 is the
			 * BASELINE the src == 1 and src == 2 rates are compared
			 * against, and a counter that only exists once the
			 * feature is on has no baseline to be compared to.
			 */
			this_cpu_inc(ivh_lock_steals);
			return true;
		}
		if (!(val & _Q_TAIL_MASK) || (val & _Q_PENDING_MASK))
			break;

		cpu_relax();
	}

	return false;
}

/*
 * The pending bit is used by the queue head vCPU to indicate that it
 * is actively spinning on the lock and no lock stealing is allowed.
 */
#if _Q_PENDING_BITS == 8
static __always_inline void set_pending(struct qspinlock *lock)
{
	WRITE_ONCE(lock->pending, 1);
}

/*
 * The pending bit check in pv_queued_spin_steal_lock() isn't a memory
 * barrier. Therefore, an atomic cmpxchg_acquire() is used to acquire the
 * lock just to be sure that it will get it.
 */
static __always_inline bool trylock_clear_pending(struct qspinlock *lock)
{
	u16 old = _Q_PENDING_VAL;

	return !READ_ONCE(lock->locked) &&
	       try_cmpxchg_acquire(&lock->locked_pending, &old, _Q_LOCKED_VAL);
}
#else /* _Q_PENDING_BITS == 8 */
static __always_inline void set_pending(struct qspinlock *lock)
{
	atomic_or(_Q_PENDING_VAL, &lock->val);
}

static __always_inline bool trylock_clear_pending(struct qspinlock *lock)
{
	int old, new;

	old = atomic_read(&lock->val);
	do {
		if (old & _Q_LOCKED_MASK)
			return false;
		/*
		 * Try to clear pending bit & set locked bit
		 */
		new = (old & ~_Q_PENDING_MASK) | _Q_LOCKED_VAL;
	} while (!atomic_try_cmpxchg_acquire (&lock->val, &old, new));

	return true;
}
#endif /* _Q_PENDING_BITS == 8 */

/*
 * Lock and MCS node addresses hash table for fast lookup
 *
 * Hashing is done on a per-cacheline basis to minimize the need to access
 * more than one cacheline.
 *
 * Dynamically allocate a hash table big enough to hold at least 4X the
 * number of possible cpus in the system. Allocation is done on page
 * granularity. So the minimum number of hash buckets should be at least
 * 256 (64-bit) or 512 (32-bit) to fully utilize a 4k page.
 *
 * Since we should not be holding locks from NMI context (very rare indeed) the
 * max load factor is 0.75, which is around the point where open addressing
 * breaks down.
 *
 */
struct pv_hash_entry {
	struct qspinlock *lock;
	struct pv_node   *node;
};

#define PV_HE_PER_LINE	(SMP_CACHE_BYTES / sizeof(struct pv_hash_entry))
#define PV_HE_MIN	(PAGE_SIZE / sizeof(struct pv_hash_entry))

static struct pv_hash_entry *pv_lock_hash;
static unsigned int pv_lock_hash_bits __read_mostly;

/*
 * Allocate memory for the PV qspinlock hash buckets
 *
 * This function should be called from the paravirt spinlock initialization
 * routine.
 */
void __init __pv_init_lock_hash(void)
{
	int pv_hash_size = ALIGN(4 * num_possible_cpus(), PV_HE_PER_LINE);

	if (pv_hash_size < PV_HE_MIN)
		pv_hash_size = PV_HE_MIN;

	/*
	 * Allocate space from bootmem which should be page-size aligned
	 * and hence cacheline aligned.
	 */
	pv_lock_hash = alloc_large_system_hash("PV qspinlock",
					       sizeof(struct pv_hash_entry),
					       pv_hash_size, 0,
					       HASH_EARLY | HASH_ZERO,
					       &pv_lock_hash_bits, NULL,
					       pv_hash_size, pv_hash_size);
}

#define for_each_hash_entry(he, offset, hash)						\
	for (hash &= ~(PV_HE_PER_LINE - 1), he = &pv_lock_hash[hash], offset = 0;	\
	     offset < (1 << pv_lock_hash_bits);						\
	     offset++, he = &pv_lock_hash[(hash + offset) & ((1 << pv_lock_hash_bits) - 1)])

static struct qspinlock **pv_hash(struct qspinlock *lock, struct pv_node *node)
{
	unsigned long offset, hash = hash_ptr(lock, pv_lock_hash_bits);
	struct pv_hash_entry *he;
	int hopcnt = 0;

	for_each_hash_entry(he, offset, hash) {
		struct qspinlock *old = NULL;
		hopcnt++;
		if (try_cmpxchg(&he->lock, &old, lock)) {
			WRITE_ONCE(he->node, node);
			lockevent_pv_hop(hopcnt);
			return &he->lock;
		}
	}
	/*
	 * Hard assume there is a free entry for us.
	 *
	 * This is guaranteed by ensuring every blocked lock only ever consumes
	 * a single entry, and since we only have 4 nesting levels per CPU
	 * and allocated 4*nr_possible_cpus(), this must be so.
	 *
	 * The single entry is guaranteed by having the lock owner unhash
	 * before it releases.
	 */
	BUG();
}

static struct pv_node *pv_unhash(struct qspinlock *lock)
{
	unsigned long offset, hash = hash_ptr(lock, pv_lock_hash_bits);
	struct pv_hash_entry *he;
	struct pv_node *node;

	for_each_hash_entry(he, offset, hash) {
		if (READ_ONCE(he->lock) == lock) {
			node = READ_ONCE(he->node);
			WRITE_ONCE(he->lock, NULL);
			return node;
		}
	}
	/*
	 * Hard assume we'll find an entry.
	 *
	 * This guarantees a limited lookup time and is itself guaranteed by
	 * having the lock owner do the unhash -- IFF the unlock sees the
	 * SLOW flag, there MUST be a hash entry.
	 */
	BUG();
}

/*
 * IVH TSC heartbeat: shadow comparator and (at ivh_pv_preempt_src == 2) the
 * live substitute for vcpu_is_preempted() at pv_wait_early()'s call site.
 *
 * NAMED is_wait_preempted() rather than the older ivh_prev_is_preempted()
 * purely so that it and its Build 1 sibling is_cs_preempted()
 * (<asm/ivh_tsc_beat.h>) read as a pair at their call sites.  They answer
 * genuinely different questions and the names are the cheapest place to say
 * so: this one is "has the vCPU we are queued behind stopped proving it is
 * executing", refreshed at >= 1 kHz from the tick plus per-spin-iteration
 * from the spin loops; the other is "is the CPU that OWNS this lock inside a
 * critical section, and is something wrong with it".  The rename was done in
 * one shot in Build 1 and must not be revisited later -- renaming across a
 * boot boundary would make every before/after diff harder to read for no
 * benefit.
 *
 * See arch/x86/include/asm/qspinlock.h for the storage and the knobs, and
 * tools/bpf/docs/ivh_tsc_heartbeat_refcycles_build_plans_2026-07-26.md sec 2
 * for why this is a portability argument and not a performance one.
 *
 * Cost when ivh_pv_preempt_src == 0 (the default) is one READ_ONCE of a
 * read-mostly global plus one perfectly-predicted branch, on a path that
 * already does READ_ONCE(ivh_pv_wait_mechanism) two lines above -- the same
 * "safe to leave compiled in permanently" posture ivh_pv_wait_trace already
 * documents in arch/x86/kernel/kvm.c.  Nothing below is reached at src == 0,
 * including the rdtsc.
 *
 * src == 1 is the measurement mode and is the one that matters: compute both
 * signals, bin the heartbeat age into the histogram selected by what the HOST
 * says, count the 2x2 agreement matrix -- and still RETURN THE KVM BIT, so
 * behavior is unchanged and the numbers are collected under the real
 * workload rather than a synthetic one.  Only src == 2 changes what
 * pv_wait_early() decides.
 *
 * ONE rdtsc, not two: ivh_beat_age() is called once and both the verdict and
 * the histogram sample are derived from that single reading.  Calling
 * ivh_beat_stale() as well would take a second, later reading, and the
 * histogram would then be describing a different event from the one the
 * verdict was taken on.
 */
static inline bool is_wait_preempted(int cpu)
{
	unsigned long src = READ_ONCE(ivh_pv_preempt_src);
	s64 age, min_age;
	bool beat, kvm;
	int bucket;

	if (likely(!src))
		return vcpu_is_preempted(cpu);

	age  = ivh_beat_age(cpu);
	beat = age > (s64)READ_ONCE(ivh_pv_beat_threshold);

	/*
	 * Cross-vCPU TSC drift guard (build plan sec 2.8).  Track the minimum
	 * age this reader has ever seen.  On offset-aligned TSCs it parks near
	 * zero and stays there; a minimum that climbs monotonically over hours
	 * is the drift signature.  Drift is dangerous precisely because it is
	 * silent and one-directional -- it never faults, it just makes one vCPU
	 * look permanently preempted to one set of neighbours.  Costs one
	 * compare, and the store is taken only when a new minimum is found.
	 * TSC-only, no PV read -- kept unconditional so drift tracking stays
	 * continuous across a live src flip between 1 and 2.
	 */
	min_age = raw_cpu_read(ivh_beat_min_age);
	if (age < min_age)
		raw_cpu_write(ivh_beat_min_age, age);

	/*
	 * Diagnostic-only gate (2026-08-24): everything below this point --
	 * the PV vcpu_is_preempted() read (a real steal_time-page touch) and
	 * the shadow-comparison histograms/counters -- exists to calibrate
	 * the heartbeat against host ground truth.  src==1 ("measurement
	 * mode", see the comment above this function) still needs all of it:
	 * it explicitly returns kvm and uses the histograms to find the
	 * threshold.  Once src==2 is committed in production the decision is
	 * `return beat` regardless of what kvm says, so nothing here is read
	 * by anything -- skipping it removes the last unconditional paravirt
	 * touch from the qspinlock wait path when running PV-free.
	 */
	if (src == 2)
		return beat;

	kvm = vcpu_is_preempted(cpu);

	/*
	 * Log2-bucketed age histogram, split by the HOST's verdict, same raw
	 * layout as ivh_obs_cs_hist (kernel/locking/spinlock.c): bucket i is
	 * 2^i..2^(i+1)-1 cycles, bucket 0 absorbs zero and every negative age
	 * (small negative TSC skew reads as "fresher than now", which belongs
	 * at the bottom, not off the end of the array), and the top bucket
	 * saturates.  Both ends clamped explicitly rather than trusted:
	 * ilog2(0) would index [-1].
	 *
	 * This pair IS the threshold-tuning method.  The threshold is wherever
	 * the two distributions separate; if the _running tail overlaps the
	 * _preempted body, the signal does not work at this write cadence and
	 * that is the result.
	 */
	bucket = (age > 0) ? ilog2((u64)age) : 0;
	if (bucket >= IVH_BEAT_AGE_HIST_BUCKETS)
		bucket = IVH_BEAT_AGE_HIST_BUCKETS - 1;
	if (kvm)
		this_cpu_inc(ivh_beat_age_hist_preempted[bucket]);
	else
		this_cpu_inc(ivh_beat_age_hist_running[bucket]);

	/* 2x2 agreement matrix against host ground truth. */
	if (beat == kvm) {
		if (kvm)
			this_cpu_inc(ivh_beat_agree_true);
		else
			this_cpu_inc(ivh_beat_agree_false);
	} else if (beat) {
		this_cpu_inc(ivh_beat_false_pos);  /* TSC says preempted, host says no */
	} else {
		this_cpu_inc(ivh_beat_false_neg);  /* host says preempted, TSC missed it */
	}

	return kvm;
}

/*
 * Phase 2 publish: stamp our own heartbeat from inside the qspinlock spin
 * loops, at microsecond-or-better cadence rather than the 1 ms tick cadence.
 * This -- not Phase 1 -- is the actual proposal; see sec 2.2 of the build plan
 * for the coverage argument (in short: while we spin on node->locked our MCS
 * predecessor is itself still SPINNING, because the predecessor releases its
 * successor at the moment it ACQUIRES the lock, not when it releases it, so
 * the long critical section is being run by the lock owner, who is not prev).
 *
 * Gated twice over, and both gates matter:
 *   - on ivh_pv_preempt_src != 0, because at src == 0 nobody reads the stamp
 *     and this would be a pure dirtying store in the hottest spin loop in the
 *     kernel;
 *   - on (loop & ivh_pv_beat_publish_mask) == 0, because the cost side here
 *     is real: every remote read of this line pulls it across the
 *     interconnect.  At the default 0xfff that is one store per 4096
 *     cpu_relax() iterations, ~90 us at this project's measured ~50 cycles
 *     per PAUSE -- already ~16x finer than the 1 ms tick floor at 1/16th the
 *     store rate of the 0xff read cadence.
 *
 * The src check is placed FIRST and reads a read-mostly global, so the
 * default path is one predicted branch and no rdtsc.
 */
static __always_inline void ivh_beat_publish_in_spin(int loop)
{
	if (likely(!READ_ONCE(ivh_pv_preempt_src)))
		return;
	if (loop & (int)READ_ONCE(ivh_pv_beat_publish_mask))
		return;

	ivh_tsc_beat_publish();
	this_cpu_inc(ivh_beat_publishes);
}

/*
 * ivh_cs_head_check - the queue head asks whether the CURRENT LOCK HOLDER is
 * in trouble, and (only at ivh_cs_preempt_src == 2) bails out of its spin if
 * the answer is yes.  Build 1, build plan sec 3.4.
 *
 * WHY THE QUEUE HEAD AND NOT THE MCS NODES.  pv_wait_early() already has the
 * right signal for its own question: for an MCS node, prev->cpu genuinely IS
 * "the thing we are waiting on", so vcpu_is_preempted(prev->cpu) /
 * is_wait_preempted(prev->cpu) is exactly on point there.  The queue head is
 * different in kind -- it is spinning on the lock BYTE, waiting for an owner
 * it has no name for.  That is the gap holder identity exists to close, and
 * this is the only site that consumes it.
 *
 * Called at the (loop & PV_PREV_CHECK_MASK) == 0 cadence pv_wait_early()
 * already uses, for the identical reason: the point of that mask is not to
 * pound a remote cacheline, and a lookup that reads a remote holder slot and
 * then a remote CS stamp is at least as expensive to do per-iteration as the
 * check it is modelled on.
 *
 * THREE SHAPE DECISIONS, each mirroring existing in-tree reasoning:
 *
 *   - ivh_cs_age() is called ONCE, and both the verdict and the histogram
 *     sample come from that single reading.  Calling is_cs_preempted()
 *     separately would take a second, later rdtsc(), and the histogram would
 *     then describe a different event from the one the verdict was taken on.
 *     Same rule, same reason, as is_wait_preempted() above.
 *   - `age >= 0` is the stamp == 0 sentinel guard.  A cleared stamp would
 *     otherwise make rdtsc() - 0 an enormous positive that reads as
 *     "preempted forever", silently and in the worst direction.
 *   - src == 2 is the ONLY thing that changes the return value.  At src == 1
 *     every counter and every histogram is fed under the real workload and
 *     the function still returns false, so behaviour is bit-identical to
 *     src == 0 while the data is collected.  Precisely the
 *     is_wait_preempted() posture.
 *
 * THE SIDE EFFECT, WHICH MUST NOT BE ASSUMED HARMLESS.  Returning true makes
 * the caller break out of its spin loop, which runs clear_pending(lock)
 * EARLIER than it otherwise would.  The pending bit is the anti-lock-stealing
 * flag -- pv_hybrid_queued_unfair_trylock() steals only when
 * !(val & _Q_LOCKED_PENDING_MASK), and the whole starvation-freedom argument
 * at the top of this file rests on the queue head holding that bit set while
 * it spins.  Clearing it sooner reopens the stealing window and weakens that
 * guarantee.  This is categorically unlike the MCS-node call site, where
 * pv_wait_early() returning true only changes WHEN a waiter moves from hot
 * spin to backoff and the caller's for(;;) re-checks node->locked regardless.
 *
 * Hence ivh_head_bail_early and ivh_head_bail_loop_hist[].  SPIN_THRESHOLD is
 * 1 << 15 and the loop counts DOWN, so a bail at loop L means the pending bit
 * is dropped L iterations early: a distribution concentrated near
 * SPIN_THRESHOLD means the bit is being released almost immediately and the
 * starvation guarantee is effectively gone, while a distribution near 0 means
 * the bail happens just before the loop would have exhausted anyway and costs
 * nothing.  Read that histogram together with ivh_lock_steals at src 1 vs 2
 * before ever leaving src == 2 in place.
 */
static inline bool ivh_cs_head_check(struct qspinlock *lock, int loop)
{
	unsigned long src = READ_ONCE(ivh_cs_preempt_src);
	unsigned long form;
	int h1, h2, bucket;
	bool verdict, kvm;
	s64 age;

	if (likely(!src))
		return false;
	if (loop & PV_PREV_CHECK_MASK)
		return false;

	this_cpu_inc(ivh_cs_checks);

	h1 = ivh_lock_holder_cpu(lock);
	if (h1 < 0)
		return false;		/* counted inside the lookup */

	if (h1 == raw_smp_processor_id()) {
		/*
		 * We are the queue head waiting for a lock we ourselves are
		 * recorded as holding.  The holder table is lying when this
		 * fires; the question is only which of THREE causes did it.
		 *
		 *  1. A MISSED RELEASE SITE.  This was the live cause on
		 *     6.17.0-rseqport67, which measured 239 of these: with the
		 *     release-side clear sitting on dead code (see the R2b
		 *     writeup in <asm/qspinlock.h>) a CPU that had previously
		 *     held this lock left its own stamp behind forever, so when
		 *     it re-contended on the same lock and read the slot during
		 *     the winner's acquire->stamp window it found ITSELF.  With
		 *     R2b in place that window answers "unknown" instead, so
		 *     this counter should now collapse toward zero -- and if it
		 *     does not, cause 1 is still present somewhere and this
		 *     counter is the thing that says so.
		 *  2. A GENUINE REENTRANCY BUG in the caller, which is what the
		 *     counter was originally written for.
		 *  3. A TORN SLOT, which is irreducible and is NOT a bug.
		 *     __ivh_lock_set_holder() writes holder_cpu then tag as two
		 *     separate stores, so two CPUs stamping two DIFFERENT locks
		 *     that hash to one slot can interleave into a slot reading
		 *     (this lock, this CPU) that no single stamper ever wrote.
		 *     The rate is bounded by the collision rate, so read this
		 *     counter against ivh_holder_unknown_collision: a residue
		 *     that scales with collisions and vanishes when
		 *     ivh_holder_bits is raised is cause 3 and can be ignored.
		 *
		 * "Must be ~0" therefore means ~0 RELATIVE TO ivh_cs_checks and
		 * to ivh_holder_unknown_collision, not literally zero.
		 *
		 * Either way the answer returned here is "do not act", so no
		 * cause of this can produce a wrong verdict -- only a lost one.
		 *
		 * Counted rather than pr_warn_once()'d, deliberately.  This
		 * runs inside the qspinlock slowpath with the lock still
		 * contended; printk() takes locks of its own and reaching it
		 * from here is a recursion hazard that would turn a diagnostic
		 * into a hang.  /proc/ivh_debug prints this counter beside
		 * ivh_cs_checks, which is where it will actually be looked at.
		 */
		this_cpu_inc(ivh_holder_self);
		return false;
	}

	/*
	 * Kept in sync BY HAND with is_cs_preempted() (<asm/ivh_tsc_beat.h>),
	 * which that function's own comment records as deliberate: this site
	 * needs the age AND the verdict to come from ONE ivh_cs_age() reading,
	 * so it open-codes the same three expressions.  Form 2 ignores the age
	 * entirely; the age is still sampled because the histograms below want
	 * it regardless of which form is selected.
	 */
	form = READ_ONCE(ivh_cs_predicate_form);
	age = ivh_cs_age(h1);
	if (form == 2)
		verdict = ivh_beat_stale(h1);
	else if (form)
		verdict = (age >= 0 && ivh_beat_stale(h1));
	else
		verdict = (age >= 0 &&
			   age > (s64)READ_ONCE(ivh_cs_beat_threshold));
	kvm = vcpu_is_preempted(h1);

	/*
	 * Log2-bucketed CS-stamp age, split by the HOST's verdict at the same
	 * instant, identical layout to ivh_beat_age_hist_*: bucket 0 absorbs
	 * zero, negatives and the not-in-a-CS sentinel; the top bucket
	 * saturates.  Both ends clamped explicitly rather than trusted --
	 * ilog2(0) would index [-1].
	 *
	 * This pair is simultaneously the threshold-calibration method AND the
	 * sign-convention check.  If _preempted does not sit above _running,
	 * `age > threshold` is backwards, and these two lines say so directly
	 * in one boot without a rebuild.
	 */
	bucket = (age > 0) ? ilog2((u64)age) : 0;
	if (bucket >= IVH_CS_AGE_HIST_BUCKETS)
		bucket = IVH_CS_AGE_HIST_BUCKETS - 1;
	if (kvm)
		this_cpu_inc(ivh_cs_age_hist_preempted[bucket]);
	else
		this_cpu_inc(ivh_cs_age_hist_running[bucket]);

	/*
	 * 2x2 agreement matrix against host ground truth.  This IS the
	 * "does the TSC signal make the same decision as the real steal bit"
	 * comparator for the CS predicate -- nothing further is needed and
	 * nothing is deferred to a later build.
	 */
	if (verdict == kvm) {
		if (kvm)
			this_cpu_inc(ivh_cs_agree_true);
		else
			this_cpu_inc(ivh_cs_agree_false);
	} else if (verdict) {
		this_cpu_inc(ivh_cs_false_pos);	/* CS stamp says preempted, host says no */
	} else {
		this_cpu_inc(ivh_cs_false_neg);	/* host says preempted, CS stamp missed it */
	}

	/*
	 * Read-verify-read.  holder_cpu and the CS stamp (or the liveness
	 * heartbeat) are two independent loads and CAN skew -- no barrier fixes
	 * that, because it is a timing problem and not a reordering problem.
	 * Re-read the holder and act only if it is unchanged; a mismatch means
	 * the lock changed hands underneath the verdict and the verdict
	 * describes a CPU that is no longer relevant.
	 */
	h2 = ivh_lock_holder_cpu(lock);
	if (h2 != h1) {
		this_cpu_inc(ivh_holder_raced);
		return false;
	}

	if (verdict) {
		this_cpu_inc(ivh_head_bail_early);
		bucket = ilog2((unsigned int)loop | 1U);
		if (bucket >= IVH_CS_LOOP_HIST_BUCKETS)
			bucket = IVH_CS_LOOP_HIST_BUCKETS - 1;
		this_cpu_inc(ivh_head_bail_loop_hist[bucket]);
	}

	return (src == 2) && verdict;
}

/*
 * Return true if when it is time to check the previous node which is not
 * in a running state.
 */
static inline bool
pv_wait_early(struct pv_node *prev, int loop)
{
	unsigned long mech;

	if ((loop & PV_PREV_CHECK_MASK) != 0)
		return false;

	if (READ_ONCE(prev->state) != VCPU_RUNNING)
		return true;

	/*
	 * IVH (default OFF, sysctl ivh_pv_wait_mechanism != 0): bail out of the
	 * hot MCS spin early — into ivh_pv_wait() (mechanism 1's non-halting
	 * TPAUSE backoff, or mechanism 2's real yielding HLT) — when the
	 * predecessor's vCPU is currently preempted by the host, in addition
	 * to the stock check above.  For mechanism 2 in particular, yielding
	 * the pCPU (HLT) precisely when the predecessor we are waiting on is
	 * itself descheduled is exactly the behavior we want: stop competing
	 * with the host for the pCPU it needs to reschedule that predecessor.
	 * vcpu_is_preempted(prev->cpu) reads the
	 * live KVM steal bit, and for an MCS queue node prev->cpu is exactly
	 * "the thing we are waiting on", so this is the in-kernel analogue of
	 * NHextend3's validated steal-flag check: stop burning a hot pCPU
	 * spinning for a descheduled predecessor and back off politely
	 * (TPAUSE) instead.  Correctness is unaffected either way — the
	 * caller's for(;;) loop re-checks node->locked regardless; this only
	 * changes *when* we transition from hot-spin to backoff.  Gated so
	 * that sysctl==0 reproduces the exact upstream pv_wait_early() check.
	 *
	 * As of the mechanism-2 "scoped halt" design (see pv_wait_node()'s
	 * wait_early-gated continue), this function's return value is no
	 * longer just an *early-exit hint* for mechanism 2 — it is the SOLE
	 * gate deciding whether that waiter ever reaches HLT at all. If this
	 * returns false every time for a given wait cycle (loop exhausts),
	 * mechanism 2 does not sleep; it loops back and keeps spinning
	 * instead of falling through to an unconditional halt. Mechanisms 0
	 * and 1 are unaffected: they still sleep on threshold exhaustion
	 * regardless of this function's return value on that final check.
	 *
	 * Mechanism 3 ("no adaptive spinning", a live stand-in for `nopvspin`)
	 * opts out alongside mechanism 0 and for the same reason: its whole
	 * purpose is to add nothing to the stock check above. Letting the steal
	 * bit bail it out early would not change *what* it does -- ivh_pv_wait()
	 * only cpu_relax()-spins under mechanism 3 either way -- but it would
	 * move the VCPU_HALTED store earlier, dragging more acquisitions through
	 * pv_kick_node()'s cmpxchg, _Q_SLOW_VAL, pv_hash() and the unlock
	 * slowpath, which is exactly the PV overhead this mechanism is meant to
	 * minimize when used as a baseline. Mechanisms 1 and 2 are unaffected.
	 */
	mech = READ_ONCE(ivh_pv_wait_mechanism);
	if (!mech || mech == 3)
		return false;

	return is_wait_preempted(prev->cpu);
}

/*
 * Initialize the PV part of the mcs_spinlock node.
 */
static void pv_init_node(struct mcs_spinlock *node)
{
	struct pv_node *pn = (struct pv_node *)node;

	BUILD_BUG_ON(sizeof(struct pv_node) > sizeof(struct qnode));

	pn->cpu = smp_processor_id();
	pn->state = VCPU_RUNNING;

	/*
	 * IVH heartbeat cold-start seed (build plan sec 2.2, "a second hole").
	 * The first time PV_PREV_CHECK_MASK fires after `prev` enters the queue,
	 * prev may not have published yet, so its slot still holds a value from
	 * a previous and entirely unrelated spin -- an instant false positive,
	 * and the worst kind, because it fires at queue-entry when the queue is
	 * most likely to be short and the wait most likely to be brief.  Seeding
	 * here fixes it for every node, since every node runs pv_init_node() on
	 * queue entry.
	 *
	 * Gated on ivh_pv_preempt_src, which is a deliberate deviation from the
	 * build plan's "unconditional seed publish": at src == 0 nothing reads
	 * the stamp, so an unconditional seed would put an rdtsc plus a
	 * remotely-read dirtying store on EVERY qspinlock slowpath entry to buy
	 * nothing.  The cost of the gate is that flipping src from 0 to 1 live
	 * leaves already-queued nodes unseeded for one spin; the tick publish is
	 * unconditional, so those still carry a stamp at worst one tick old,
	 * i.e. exactly Phase 1 freshness, and the shadow comparator counts the
	 * resulting disagreements rather than acting on them.
	 */
	if (unlikely(READ_ONCE(ivh_pv_preempt_src))) {
		ivh_tsc_beat_publish();
		this_cpu_inc(ivh_beat_publishes);
	}
}

/*
 * Wait for node->locked to become true, halt the vcpu after a short spin.
 * pv_kick_node() is used to set _Q_SLOW_VAL and fill in hash table on its
 * behalf.
 */
static void pv_wait_node(struct mcs_spinlock *node, struct mcs_spinlock *prev)
{
	struct pv_node *pn = (struct pv_node *)node;
	struct pv_node *pp = (struct pv_node *)prev;
	bool wait_early;
	int loop;

	for (;;) {
		for (wait_early = false, loop = SPIN_THRESHOLD; loop; loop--) {
			/*
			 * node->locked stays UNCONDITIONALLY FIRST. That is
			 * also the mitigation for sec 2.2's "one real hole":
			 * between prev acquiring the lock (it stops publishing)
			 * and us observing node->locked == 1, prev's stamp
			 * ages. This loop reads node->locked on EVERY
			 * iteration while the heartbeat is consulted only every
			 * 256th, so that window is a few hundred cycles against
			 * a threshold in the millions. Do not reorder these.
			 */
			if (READ_ONCE(node->locked))
				return;
			if (pv_wait_early(pp, loop)) {
				wait_early = true;
				break;
			}
			ivh_beat_publish_in_spin(loop);
			cpu_relax();
		}

		/*
		 * IVH mechanism 2 ("scoped halt"): only ever go to sleep (real
		 * HLT via pv_wait()/ivh_pv_wait()) when pv_wait_early() actually
		 * fired -- i.e. wait_early == true, meaning the predecessor
		 * `prev` we are queued behind looks unhealthy (host-preempted
		 * per vcpu_is_preempted(prev->cpu), or itself MCS-halted per
		 * prev->state != VCPU_RUNNING; pv_wait_early() folds both, and
		 * both are genuine "prev is not making progress for us" signals).
		 * If instead the inner SPIN_THRESHOLD loop merely EXHAUSTED
		 * while `prev` still looked healthy (wait_early == false), do
		 * NOT fall through to HLT: loop back to the outer for(;;),
		 * re-arm SPIN_THRESHOLD, and keep spinning. This degrades to a
		 * plain MCS busy-poll on node->locked -- bounded by prev's FIFO
		 * progress exactly like native qspinlock -- rather than paying
		 * an unwarranted HLT vmexit + host reschedule when nothing is
		 * actually wrong. pn->state stays VCPU_RUNNING throughout the
		 * spin, so a concurrent pv_kick_node()'s cmpxchg(HALTED->HASHED)
		 * fails and correctly leaves us to self-observe node->locked
		 * (qspinlock.c sets node->locked = 1, store-release, strictly
		 * before calling pv_kick_node() -- see that function's comment).
		 *
		 * Scoped to mechanism 2 ONLY. Mechanisms 0 and 1 keep their
		 * existing behavior unchanged: fall through and sleep once the
		 * threshold is spent regardless of wait_early (mechanism 0's
		 * host-cooperative HLT/kick, mechanism 1's bounded TPAUSE poll).
		 * The `== 2` conjunct is what preserves that.
		 *
		 * Honest caveat: under pure in-guest contention with no host
		 * preemption anywhere in the chain, this means mechanism 2 never
		 * yields the pCPU at all here -- it degrades to native qspinlock
		 * busy-spin, not to mechanism 0's halt-after-threshold. That is
		 * the intended "yield only when there is real preemption to get
		 * out of the way of" semantics, but it is a real, measurable
		 * divergence from mechanism 0 under heavy non-preemption
		 * contention (more busy-spin CPU, no pCPU yield) -- see
		 * tools/bpf/docs/ivh_scoped_halt_ipi_mechanism2_plan_2026-07-23.md
		 * §7 before assuming this is a universal improvement.
		 */
		if (!wait_early && READ_ONCE(ivh_pv_wait_mechanism) == 2)
			continue;

		/*
		 * Order pn->state vs pn->locked thusly:
		 *
		 * [S] pn->state = VCPU_HALTED	  [S] next->locked = 1
		 *     MB			      MB
		 * [L] pn->locked		[RmW] pn->state = VCPU_HASHED
		 *
		 * Matches the cmpxchg() from pv_kick_node().
		 */
		smp_store_mb(pn->state, VCPU_HALTED);

		if (!READ_ONCE(node->locked)) {
			lockevent_inc(pv_wait_node);
			lockevent_cond_inc(pv_wait_early, wait_early);
			pv_wait(&pn->state, VCPU_HALTED);
		}

		/*
		 * If pv_kick_node() changed us to VCPU_HASHED, retain that
		 * value so that pv_wait_head_or_lock() knows to not also try
		 * to hash this lock.
		 */
		cmpxchg(&pn->state, VCPU_HALTED, VCPU_RUNNING);

		/*
		 * If the locked flag is still not set after wakeup, it is a
		 * spurious wakeup and the vCPU should wait again. However,
		 * there is a pretty high overhead for CPU halting and kicking.
		 * So it is better to spin for a while in the hope that the
		 * MCS lock will be released soon.
		 */
		lockevent_cond_inc(pv_spurious_wakeup,
				  !READ_ONCE(node->locked));
	}

	/*
	 * By now our node->locked should be 1 and our caller will not actually
	 * spin-wait for it. We do however rely on our caller to do a
	 * load-acquire for us.
	 */
}

/*
 * Called after setting next->locked = 1 when we're the lock owner.
 *
 * Instead of waking the waiters stuck in pv_wait_node() advance their state
 * such that they're waiting in pv_wait_head_or_lock(), this avoids a
 * wake/sleep cycle.
 */
static void pv_kick_node(struct qspinlock *lock, struct mcs_spinlock *node)
{
	struct pv_node *pn = (struct pv_node *)node;
	unsigned long mech;
	u8 old = VCPU_HALTED;
	/*
	 * If the vCPU is indeed halted, advance its state to match that of
	 * pv_wait_node(). If OTOH this fails, the vCPU was running and will
	 * observe its next->locked value and advance itself.
	 *
	 * Under IVH mechanism 2 ("scoped halt"), a role-C successor is often
	 * still VCPU_RUNNING here (it never halted at all -- see the
	 * wait_early-gated continue in pv_wait_node()), in which case this
	 * cmpxchg simply fails as the "OTOH" case above already describes: no
	 * hash, no IPI, the spinning waiter self-observes next->locked. This
	 * is not a new case, just a more common one under mechanism 2.
	 *
	 * Matches with smp_store_mb() and cmpxchg() in pv_wait_node()
	 *
	 * The write to next->locked in arch_mcs_spin_unlock_contended()
	 * must be ordered before the read of pn->state in the cmpxchg()
	 * below for the code to work correctly. To guarantee full ordering
	 * irrespective of the success or failure of the cmpxchg(),
	 * a relaxed version with explicit barrier is used. The control
	 * dependency will order the reading of pn->state before any
	 * subsequent writes.
	 */
	smp_mb__before_atomic();
	if (!try_cmpxchg_relaxed(&pn->state, &old, VCPU_HASHED))
		return;

	/*
	 * Put the lock into the hash table and set the _Q_SLOW_VAL.
	 *
	 * As this is the same vCPU that will check the _Q_SLOW_VAL value and
	 * the hash table later on at unlock time, no atomic instruction is
	 * needed.
	 */
	WRITE_ONCE(lock->locked, _Q_SLOW_VAL);
	(void)pv_hash(lock, pn);

	/*
	 * IVH (sysctl ivh_pv_wait_mechanism != 0, i.e. mechanism 1 or 2): the
	 * successor is waiting in ivh_pv_wait(&pn->state, VCPU_HALTED)
	 * (arch/x86/kernel/kvm.c) -- mechanism 1 re-checking pn->state each
	 * TPAUSE nap, mechanism 2 in a real HLT. The try_cmpxchg_relaxed()
	 * above just published pn->state == VCPU_HASHED with full-barrier
	 * ordering (smp_mb__before_atomic() + the RMW), so that write is
	 * already visible to the successor's next READ_ONCE() before this IPI
	 * is sent -- the IPI only needs to wake the successor (cut mechanism
	 * 1's current TPAUSE nap short, or un-halt mechanism 2's HLT), it is
	 * not what makes the new state visible. Do not move this IPI before
	 * the cmpxchg: doing so would race the wake against the state write
	 * and merely waste a wake (successor resumes, still observes
	 * VCPU_HALTED, re-waits) rather than corrupt anything, but it defeats
	 * the point of adding it.
	 *
	 * Gated on the same cmpxchg that already gates the whole slow path:
	 * a successor still hot-spinning in pv_wait_node()'s SPIN_THRESHOLD
	 * loop is VCPU_RUNNING, fails the cmpxchg above, and returns before
	 * reaching here -- no IPI is sent for fast, uncontended handoffs.
	 *
	 * A lost/misdelivered IPI is not a hang: pv_wait_node()'s caller loop
	 * re-checks node->locked in its own for(;;) regardless of how (or
	 * whether) ivh_pv_wait() returned. Mechanism 1's ivh_pv_wait() always
	 * returns by IVH_PV_ADAPTIVE_TSC (~1 ms) even with zero IPIs; mechanism
	 * 2's HLT un-halts on the next timer tick at the latest -- so either
	 * mechanism makes forward progress with no IPI at all.
	 *
	 * Mechanism 3 is excluded: its successor is not halted or napping, it is
	 * in ivh_pv_wait()'s plain cpu_relax() loop on pn->state, and the
	 * try_cmpxchg_relaxed() above has already published VCPU_HASHED to it --
	 * so the IPI has nothing left to do and would only add a vmexit per
	 * hashed handoff to the one mechanism whose entire purpose is to add
	 * nothing. This is the same "no kick at all" decision ivh_pv_kick() makes
	 * for mechanism 3, applied at the second, independent kick site. It
	 * cannot strand a straggler left over from a live toggle out of mechanism
	 * 0 either: that waiter is in a bare halt() with RFLAGS.IF=0, which a
	 * maskable RESCHEDULE_VECTOR IPI provably could never have woken anyway
	 * (only ivh_pv_kick()'s KVM_HC_KICK_CPU hypercall can, and mechanism 3
	 * deliberately keeps issuing that one).
	 */
	mech = READ_ONCE(ivh_pv_wait_mechanism);
	if (mech && mech != 3)
		smp_send_reschedule(pn->cpu);
}

/*
 * Wait for l->locked to become clear and acquire the lock;
 * halt the vcpu after a short spin.
 * __pv_queued_spin_unlock() will wake us.
 *
 * The current value of the lock will be returned for additional processing.
 */
static u32
pv_wait_head_or_lock(struct qspinlock *lock, struct mcs_spinlock *node)
{
	struct pv_node *pn = (struct pv_node *)node;
	struct qspinlock **lp = NULL;
	int waitcnt = 0;
	int loop;

	/*
	 * If pv_kick_node() already advanced our state, we don't need to
	 * insert ourselves into the hash table anymore.
	 */
	if (READ_ONCE(pn->state) == VCPU_HASHED)
		lp = (struct qspinlock **)1;

	/*
	 * Tracking # of slowpath locking operations
	 */
	lockevent_inc(lock_slowpath);

	for (;; waitcnt++) {
		/*
		 * Set correct vCPU state to be used by queue node wait-early
		 * mechanism.
		 */
		WRITE_ONCE(pn->state, VCPU_RUNNING);

		/*
		 * Set the pending bit in the active lock spinning loop to
		 * disable lock stealing before attempting to acquire the lock.
		 */
		set_pending(lock);
		for (loop = SPIN_THRESHOLD; loop; loop--) {
			if (trylock_clear_pending(lock)) {
				/*
				 * IVH ownership-transfer site A7 (build plan
				 * sec 3.3.4): the queue head acquires straight
				 * out of the spin.  trylock_clear_pending() is
				 * a try_cmpxchg_acquire() on the
				 * locked_pending halfword in the
				 * _Q_PENDING_BITS == 8 variant that is live
				 * here.
				 */
				ivh_lock_set_holder(lock);
				goto gotlock;
			}
			/*
			 * IVH CS predicate consult site (build plan sec 3.4).
			 * At ivh_cs_preempt_src == 0 this is one READ_ONCE of a
			 * read-mostly global and one predicted branch, taken
			 * before anything else -- the same posture
			 * ivh_beat_publish_in_spin() below already has.  At
			 * src == 1 it measures and still returns false.  Only
			 * at src == 2 can it break the loop, and breaking the
			 * loop runs clear_pending() below EARLIER than it would
			 * otherwise have run, which reopens the lock-stealing
			 * window -- see the function's own comment, and read
			 * ivh_head_bail_loop_hist[] before trusting src == 2.
			 */
			if (ivh_cs_head_check(lock, loop))
				break;
			/*
			 * The queue head publishes too: sec 2.2's coverage
			 * table needs "prev spinning in pv_wait_head_or_lock()"
			 * to produce a fresh timestamp, otherwise the waiter
			 * queued directly behind the head would read the head
			 * as preempted for the whole time it holds that role.
			 */
			ivh_beat_publish_in_spin(loop);
			cpu_relax();
		}
		clear_pending(lock);


		if (!lp) { /* ONCE */
			lp = pv_hash(lock, pn);

			/*
			 * We must hash before setting _Q_SLOW_VAL, such that
			 * when we observe _Q_SLOW_VAL in __pv_queued_spin_unlock()
			 * we'll be sure to be able to observe our hash entry.
			 *
			 *   [S] <hash>                 [Rmw] l->locked == _Q_SLOW_VAL
			 *       MB                           RMB
			 * [RmW] l->locked = _Q_SLOW_VAL  [L] <unhash>
			 *
			 * Matches the smp_rmb() in __pv_queued_spin_unlock().
			 */
			if (xchg(&lock->locked, _Q_SLOW_VAL) == 0) {
				/*
				 * The lock was free and now we own the lock.
				 * Change the lock value back to _Q_LOCKED_VAL
				 * and unhash the table.
				 */
				WRITE_ONCE(lock->locked, _Q_LOCKED_VAL);
				WRITE_ONCE(*lp, NULL);
				/*
				 * IVH ownership-transfer site A8 (build plan
				 * sec 3.3.4): the queue head found the lock
				 * free while hashing it, and now owns it.
				 * Stamped after the lock byte is restored to
				 * _Q_LOCKED_VAL so that the holder record and
				 * the lock word become visible in that order.
				 */
				ivh_lock_set_holder(lock);
				goto gotlock;
			}
		}
		WRITE_ONCE(pn->state, VCPU_HASHED);
		lockevent_inc(pv_wait_head);
		lockevent_cond_inc(pv_wait_again, waitcnt);
		pv_wait(&lock->locked, _Q_SLOW_VAL);

		/*
		 * Because of lock stealing, the queue head vCPU may not be
		 * able to acquire the lock before it has to wait again.
		 */
	}

	/*
	 * The cmpxchg() or xchg() call before coming here provides the
	 * acquire semantics for locking. The dummy ORing of _Q_LOCKED_VAL
	 * here is to indicate to the compiler that the value will always
	 * be nozero to enable better code optimization.
	 */
gotlock:
	return (u32)(atomic_read(&lock->val) | _Q_LOCKED_VAL);
}

/*
 * Include the architecture specific callee-save thunk of the
 * __pv_queued_spin_unlock(). This thunk is put together with
 * __pv_queued_spin_unlock() to make the callee-save thunk and the real unlock
 * function close to each other sharing consecutive instruction cachelines.
 * Alternatively, architecture specific version of __pv_queued_spin_unlock()
 * can be defined.
 */
#include <asm/qspinlock_paravirt.h>

/*
 * PV versions of the unlock fastpath and slowpath functions to be used
 * instead of queued_spin_unlock().
 */
__visible __lockfunc void
__pv_queued_spin_unlock_slowpath(struct qspinlock *lock, u8 locked)
{
	struct pv_node *node;

	if (unlikely(locked != _Q_SLOW_VAL)) {
		WARN(!debug_locks_silent,
		     "pvqspinlock: lock 0x%lx has corrupted value 0x%x!\n",
		     (unsigned long)lock, atomic_read(&lock->val));
		return;
	}

	/*
	 * A failed cmpxchg doesn't provide any memory-ordering guarantees,
	 * so we need a barrier to order the read of the node data in
	 * pv_unhash *after* we've read the lock being _Q_SLOW_VAL.
	 *
	 * Matches the cmpxchg() in pv_wait_head_or_lock() setting _Q_SLOW_VAL.
	 */
	smp_rmb();

	/*
	 * Since the above failed to release, this must be the SLOW path.
	 * Therefore start by looking up the blocked node and unhashing it.
	 */
	node = pv_unhash(lock);

	/*
	 * Now that we have a reference to the (likely) blocked pv_node,
	 * release the lock.
	 */
	/*
	 * IVH release site R4 (build plan sec 3.3.4).  Idempotent with respect
	 * to R3 below, which has already cleared this slot on the way in: a
	 * clear of an already-cleared slot fails the tag check and does
	 * nothing.  That idempotence is deliberate -- see R3 for why clearing
	 * unconditionally BEFORE the conditional release is correct and
	 * clearing only on success would not be.
	 */
	ivh_lock_clear_holder(lock);
	smp_store_release(&lock->locked, 0);

	/*
	 * At this point the memory pointed at by lock can be freed/reused,
	 * however we can still use the pv_node to kick the CPU.
	 * The other vCPU may not really be halted, but kicking an active
	 * vCPU is harmless other than the additional latency in completing
	 * the unlock.
	 */
	lockevent_inc(pv_kick_unlock);
	pv_kick(node->cpu);
}

#ifndef __pv_queued_spin_unlock
__visible __lockfunc void __pv_queued_spin_unlock(struct qspinlock *lock)
{
	u8 locked = _Q_LOCKED_VAL;

	/*
	 * We must not unlock if SLOW, because in that case we must first
	 * unhash. Otherwise it would be possible to have multiple @lock
	 * entries, which would be BAD.
	 */
	/*
	 * IVH release site R3 (build plan sec 3.3.4).
	 *
	 * NOT COMPILED ON x86-64, which is the single largest contributor to
	 * the stamps/clears imbalance measured on 6.17.0-rseqport67 (see the
	 * R2b writeup in <asm/qspinlock.h>): <asm/qspinlock_paravirt.h> defines
	 * __pv_queued_spin_unlock to itself under CONFIG_64BIT, so the #ifndef
	 * above excludes this whole function and the live unlock is that
	 * header's hand-written PV_UNLOCK_ASM, which touches no C on its fast
	 * path.  This body is still the live one on x86-32 and on any other
	 * architecture that does not hand-code the thunk, so the clear stays.
	 *
	 * This is a CONDITIONAL
	 * release: on failure it falls through to R4 in
	 * __pv_queued_spin_unlock_slowpath().  Clear unconditionally here and
	 * let R4's clear be a no-op, rather than trying to clear only when the
	 * cmpxchg succeeds -- the latter would leave a window in which the lock
	 * byte is already 0 and the holder is still published, i.e. a live
	 * wrong answer handed to whichever queue head is spinning on this lock
	 * at that instant.  A spurious early clear only ever produces
	 * "unknown", which is the safe direction.
	 */
	ivh_lock_clear_holder(lock);
	if (try_cmpxchg_release(&lock->locked, &locked, 0))
		return;

	__pv_queued_spin_unlock_slowpath(lock, locked);
}
#endif /* __pv_queued_spin_unlock */
