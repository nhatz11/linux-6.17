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
 * ivh_adaptive_mode, which only arch/x86 defines.  arch/powerpc's
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

/*
 * IVH Idea 2 (head-role takeover) Stage 0 -- observe-only, no takeover logic
 * yet. head_ctl packs {gen:32 | yields:16 | state:16}; only `state` is used
 * this stage (HEAD_IDLE/HEAD_ARMED/HEAD_SPINNING -- HEAD_YIELDED is declared
 * for Stage 1 forward-compat but nothing sets it yet, so
 * ivh_head_woke_yielded must read 0 in every Stage-0 run). See tools/bpf/docs/
 * ivh_adaptive_spinning_build_plan_2026-09-05.md §2 for the full design.
 *
 * HEAD_SPINNING (Stage 0b, added 2026-09-08) closes a blind spot found in the
 * first Stage-0 data: HEAD_ARMED is set only immediately before the head's
 * real pv_wait(), i.e. only AFTER the head has already burned its whole
 * SPIN_THRESHOLD and has already stored VCPU_HASHED into its own pn->state.
 * A node waiter that classifies its predecessor by re-reading pp->state
 * therefore ALWAYS resolves that window to tier 1, and tier 2 can never be
 * observed in it -- live data confirmed exactly that (try_tier1 = 5210,
 * try_tier2 = 0, against 7214 global tier-2 fires).
 *
 * The window that actually matters for Idea 2 is the structurally EARLIER
 * one: the head is still in its own spin loop, has not exhausted
 * SPIN_THRESHOLD, its pn->state is still VCPU_RUNNING -- and yet its vCPU has
 * genuinely been preempted by the host mid-spin. That is precisely what the
 * TSC heartbeat (tier 2) exists to detect, and Stage 0 as first shipped could
 * not see it at all. HEAD_SPINNING marks that window, so a waiter can now
 * distinguish three predecessor states rather than two: not-the-head
 * (HEAD_IDLE), head-still-spinning (HEAD_SPINNING), head-committed-to-halt
 * (HEAD_ARMED).
 *
 * Numeric values of the pre-existing states are deliberately left unchanged
 * so any existing offline decoder of head_ctl keeps working.
 */
#define HEAD_IDLE 0
#define HEAD_ARMED 1
#define HEAD_YIELDED 2
#define HEAD_SPINNING 3
#define HC(gen, y, st) (((u64)(gen) << 32) | ((u64)(y) << 16) | (st))

struct pv_node {
	struct mcs_spinlock	mcs;
	int			cpu;
	u8			state;
	u64			head_ctl;
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
			lockevent_inc(pv_lock_stealing);
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
 * See arch/x86/include/asm/qspinlock.h for the storage and the knobs, and
 * tools/bpf/docs/ivh_tsc_heartbeat_refcycles_build_plans_2026-07-26.md sec 2
 * for why this is a portability argument and not a performance one.
 *
 * Cost when ivh_pv_preempt_src == 0 (the default) is one READ_ONCE of a
 * read-mostly global plus one perfectly-predicted branch, on a path that
 * already does READ_ONCE(ivh_adaptive_mode) two lines above -- the same
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
 * the histogram sample are derived from that single reading.
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
	 * Tier-2 observability, unconditional for every src!=0 call (src==1
	 * AND src==2) -- placed before the src==2 early-exit below so a
	 * threshold's real fire rate is measurable in the one configuration
	 * (src==2) that actually acts on it. See ivh_tsc_beat.h for why this
	 * was missing.
	 */
	this_cpu_inc(ivh_beat_tier2_checked);
	if (beat)
		this_cpu_inc(ivh_beat_tier2_fired);

	/*
	 * Unconditional (src==1 AND src==2) raw age histogram -- added
	 * 2026-09-07. The EXISTING ivh_beat_age_hist_running/preempted below
	 * are gated behind src==1's ground-truth comparison, which is dead on
	 * any host without a real steal-time page (this one included:
	 * vcpu_is_preempted() is hardwired false here, so src==1's "ground
	 * truth" is meaningless and that histogram has never once been
	 * populated under real src==2 operation this whole investigation).
	 * This one is not split by any ground truth -- it just answers "what
	 * does the real age distribution look like under real src==2 use,"
	 * which is the one thing the existing histogram cannot answer here.
	 * Same log2 bucketing as the existing histogram, deliberately: bucket
	 * i is 2^i..2^(i+1)-1 cycles, bucket 0 absorbs zero/negative age, top
	 * bucket saturates.
	 */
	{
		int raw_bucket = (age > 0) ? ilog2((u64)age) : 0;

		if (raw_bucket >= IVH_BEAT_AGE_HIST_BUCKETS)
			raw_bucket = IVH_BEAT_AGE_HIST_BUCKETS - 1;
		this_cpu_inc(ivh_beat_age_hist_raw[raw_bucket]);
	}

	/*
	 * Cross-vCPU TSC drift guard (build plan sec 2.8).  Track the minimum
	 * age this reader has ever seen.  Costs one compare, and the store is
	 * taken only when a new minimum is found.  TSC-only, no PV read --
	 * kept unconditional so drift tracking stays continuous across a live
	 * src flip between 1 and 2.
	 */
	min_age = raw_cpu_read(ivh_beat_min_age);
	if (age < min_age)
		raw_cpu_write(ivh_beat_min_age, age);

	/*
	 * Diagnostic-only gate (2026-08-24): everything below this point --
	 * the PV vcpu_is_preempted() read (a real steal_time-page touch) and
	 * the shadow-comparison histograms/counters -- exists to calibrate
	 * the heartbeat against host ground truth.  src==1 ("measurement
	 * mode") still needs all of it: it explicitly returns kvm and uses
	 * the histograms to find the threshold.  Once src==2 is committed the
	 * decision is `return beat` regardless of what kvm says, so nothing
	 * here is read by anything -- skipping it removes the last
	 * unconditional paravirt touch from the qspinlock wait path when
	 * running PV-free.
	 */
	if (src == 2)
		return beat;

	kvm = vcpu_is_preempted(cpu);

	/*
	 * Log2-bucketed age histogram, split by the HOST's verdict: bucket i
	 * is 2^i..2^(i+1)-1 cycles, bucket 0 absorbs zero and every negative
	 * age, and the top bucket saturates.  Both ends clamped explicitly
	 * rather than trusted: ilog2(0) would index [-1].
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
 * While we spin on node->locked our MCS predecessor is itself still
 * SPINNING, because the predecessor releases its successor at the moment it
 * ACQUIRES the lock, not when it releases it, so the long critical section
 * is being run by the lock owner, who is not prev.
 *
 * Gated twice over, and both gates matter:
 *   - on ivh_pv_preempt_src != 0, because at src == 0 nobody reads the stamp
 *     and this would be a pure dirtying store in the hottest spin loop in the
 *     kernel;
 *   - on (loop & ivh_pv_beat_publish_mask) == 0, because every remote read
 *     of this line pulls it across the interconnect.  At the default 0xfff
 *     that is one store per 4096 cpu_relax() iterations.
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
 * Return true if when it is time to check the previous node which is not
 * in a running state.
 */
static inline bool
pv_wait_early(struct pv_node *prev, int loop)
{
	if ((loop & PV_PREV_CHECK_MASK) != 0)
		return false;

	if (READ_ONCE(prev->state) != VCPU_RUNNING) {
		this_cpu_inc(ivh_beat_tier1_fired);
		return true;
	}

	/*
	 * Tier 2 (mode IVH_MODE_ADAPTIVE only): bail out of the hot MCS spin
	 * early when the predecessor's vCPU looks stale by the TSC-heartbeat
	 * check in is_wait_preempted() -- see <asm/ivh_tsc_beat.h>. Modes
	 * VANILLA and PURE_IPI add nothing here, matching upstream's own
	 * pv_wait_early() exactly (tier 1 above is upstream's whole check).
	 * Correctness is unaffected either way -- the caller's for(;;) loop
	 * re-checks node->locked regardless; this only changes *when* we
	 * transition from hot-spin to halt.
	 */
	if (READ_ONCE(ivh_adaptive_mode) != IVH_MODE_ADAPTIVE)
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
	pn->head_ctl = HC(0, 0, HEAD_IDLE);

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
	 * Gated on ivh_pv_preempt_src: at src == 0 nothing reads the stamp, so
	 * an unconditional seed would put an rdtsc plus a remotely-read
	 * dirtying store on EVERY qspinlock slowpath entry to buy nothing.
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
static void pv_wait_node(struct mcs_spinlock *node, struct mcs_spinlock *prev,
			  struct qspinlock *lock)
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
			if (READ_ONCE(node->locked)) {
				/*
				 * Denominator-completeness fix (GLOCK-11, found by
				 * independent review): this success return bypasses
				 * both the ivh_node_spin_iters_sum/attempts accounting
				 * below AND this identical accounting entirely, so
				 * GLOCK-10's "iters per attempt" metric silently
				 * excluded every pass that acquired the lock rather
				 * than bailing/exhausting -- the review reconstructed
				 * this excluded population from ivh_beat_tier2_checked
				 * and found it comparable in size to the measured
				 * A-vs-B difference in the steady-state rounds. Record
				 * it here so the two counter pairs can be summed for a
				 * complete, unbiased average over ALL inner-loop passes,
				 * not just the ones that bailed or exhausted.
				 */
				this_cpu_add(ivh_node_spin_success_iters_sum, SPIN_THRESHOLD - loop);
				this_cpu_inc(ivh_node_spin_success_attempts);
				return;
			}
			if (pv_wait_early(pp, loop)) {
				wait_early = true;

				/*
				 * IVH Idea 2 Stage 0: OBSERVE ONLY, no takeover
				 * logic yet -- counts what a real takeover would
				 * fire on, without acting on it. `pp` is our
				 * predecessor; if it's currently the queue head
				 * and looks stale enough to trigger this
				 * early-bail, check whether the lock is actually
				 * free right now -- that's the real
				 * opportunity-rate question for Stage 1.
				 *
				 * Stage 0b splits that by WHICH head window we
				 * caught, because the two are not the same
				 * opportunity and must not be summed:
				 *
				 *   HEAD_ARMED    - the head already exhausted
				 *                   its own SPIN_THRESHOLD and
				 *                   is committed to halting.
				 *                   Late. Sub-split by tier,
				 *                   as before.
				 *   HEAD_SPINNING - the head is still in its own
				 *                   spin loop and has NOT given
				 *                   up. Strictly earlier, and
				 *                   reachable only via tier 2.
				 *   HEAD_IDLE     - prev is not the queue head;
				 *                   nothing to observe.
				 */
				switch (READ_ONCE(pp->head_ctl) & 0xffff) {
				case HEAD_ARMED: {
					bool tier1 = READ_ONCE(pp->state) != VCPU_RUNNING;

					if (tier1) {
						this_cpu_inc(ivh_head_yield_try_tier1);
						if (!READ_ONCE(lock->locked))
							this_cpu_inc(ivh_head_yield_ok_tier1);
					} else {
						this_cpu_inc(ivh_head_yield_try_tier2);
						if (!READ_ONCE(lock->locked))
							this_cpu_inc(ivh_head_yield_ok_tier2);
					}
					break;
				}
				case HEAD_SPINNING:
					/*
					 * IVH Idea 2 Stage 0b: the window the
					 * HEAD_ARMED case above is structurally
					 * blind to -- prev is the queue head and
					 * is STILL SPINNING in its own
					 * SPIN_THRESHOLD loop, has not decided to
					 * halt, and yet looks stale to us. This
					 * is the real tier-2 catch: strictly
					 * earlier than the armed window, which by
					 * construction can only be entered after
					 * the head already gave up on its own.
					 *
					 * NO tier1/tier2 split here, and that is
					 * a fact about the code, not a shortcut.
					 * HEAD_SPINNING is stored in the same
					 * breath as pn->state = VCPU_RUNNING and
					 * nothing but the head itself writes
					 * pn->state during its tenure
					 * (pv_kick_node()'s cmpxchg only fires on
					 * VCPU_HALTED). So pp->state re-reads
					 * VCPU_RUNNING throughout this window,
					 * tier 1 cannot have been what fired, and
					 * duplicating the split would only
					 * manufacture a permanently-zero counter
					 * -- the exact defect that made the armed
					 * window's tier-2 half useless.
					 *
					 * The single exception is the head's own
					 * short pre-arm gap (its VCPU_HASHED
					 * store through pv_hash()/xchg() to the
					 * HEAD_ARMED store). Counted separately
					 * below, on purpose, so it can be shown
					 * to be small instead of silently
					 * inflating the tier-2 numbers.
					 */
					if (READ_ONCE(pp->state) != VCPU_RUNNING) {
						this_cpu_inc(ivh_head_spinning_prearm);
						break;
					}
					this_cpu_inc(ivh_head_yield_try_tier2_spinning);
					if (!READ_ONCE(lock->locked))
						this_cpu_inc(ivh_head_yield_ok_tier2_spinning);
					break;
				default:
					/* HEAD_IDLE: prev isn't the queue head. */
					break;
				}
				break;
			}
			ivh_beat_publish_in_spin(loop);
			cpu_relax();
		}

		/*
		 * Spin-iteration accounting (GLOCK-10): record how many of the
		 * SPIN_THRESHOLD iterations were actually spent before this pass
		 * gave up on lock-free acquisition -- SPIN_THRESHOLD - loop.
		 * `loop` still holds the pre-decrement value whether we got here
		 * via the wait_early break above or via natural exhaustion
		 * (loop == 0).
		 */
		this_cpu_add(ivh_node_spin_iters_sum, SPIN_THRESHOLD - loop);
		this_cpu_inc(ivh_node_spin_attempts);

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
			this_cpu_inc(ivh_halt_from_node);
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
	u8 old = VCPU_HALTED;
	/*
	 * If the vCPU is indeed halted, advance its state to match that of
	 * pv_wait_node(). If OTOH this fails, the vCPU was running and will
	 * observe its next->locked value and advance itself.
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
	 * Vanilla upstream sends no wake here, on purpose (see the comment
	 * above pv_kick_node()'s declaration below): the successor is merely
	 * advanced to waiting in pv_wait_head_or_lock() rather than woken, to
	 * avoid a wake/sleep cycle. All three IVH modes match this exactly --
	 * a mechanism-2-only smp_send_reschedule() used to live here (measured
	 * at ~1.5 IPIs/acquisition, on the ACQUIRER's own critical path, GLOCK-
	 * 12) but had no vanilla counterpart to "convert" to IPI, so it was
	 * deleted rather than carried into ivh_adaptive_mode.
	 */
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
		 * IVH Idea 2 Stage 0b: mark "head is actively spinning, not yet
		 * armed to halt". Deliberately in the same breath as the
		 * VCPU_RUNNING store above, and deliberately INSIDE the retry
		 * loop rather than once before it: lock stealing means this
		 * loop genuinely re-enters (see the comment at the bottom of
		 * it), and each re-entry is a fresh spin tenure that has to be
		 * re-marked, exactly as pn->state is re-stored here.
		 *
		 * Pairing these two stores is also what makes the observer side
		 * in pv_wait_node() unambiguous: for as long as head_ctl reads
		 * HEAD_SPINNING, pn->state reads VCPU_RUNNING, right up until
		 * this vCPU itself stores VCPU_HASHED below. Nobody else writes
		 * pn->state while we hold the head role -- pv_kick_node()'s
		 * cmpxchg only fires on VCPU_HALTED, which the head never is.
		 */
		WRITE_ONCE(pn->head_ctl, HC(0, 0, HEAD_SPINNING));
		this_cpu_inc(ivh_head_spin_enter);

		/*
		 * Set the pending bit in the active lock spinning loop to
		 * disable lock stealing before attempting to acquire the lock.
		 */
		set_pending(lock);
		for (loop = SPIN_THRESHOLD; loop; loop--) {
			if (trylock_clear_pending(lock))
				goto gotlock;
			/*
			 * The queue head publishes too: "prev spinning in
			 * pv_wait_head_or_lock()" needs to produce a fresh
			 * timestamp, otherwise the waiter queued directly
			 * behind the head would read the head as preempted
			 * for the whole time it holds that role.
			 */
			ivh_beat_publish_in_spin(loop);
			cpu_relax();
		}
		clear_pending(lock);

		/*
		 * Spin-iteration accounting control (GLOCK-10): only reached via
		 * natural exhaustion (loop == 0 here, goto gotlock bypasses this
		 * entirely), so this should average almost exactly SPIN_THRESHOLD
		 * every time -- this path has no early-bail logic in any
		 * mechanism. A sanity check on the accounting, not a variable
		 * under test.
		 */
		this_cpu_add(ivh_head_spin_iters_sum, SPIN_THRESHOLD - loop);
		this_cpu_inc(ivh_head_spin_attempts);

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
				goto gotlock;
			}
		}
		WRITE_ONCE(pn->state, VCPU_HASHED);
		lockevent_inc(pv_wait_head);
		lockevent_cond_inc(pv_wait_again, waitcnt);
		this_cpu_inc(ivh_halt_from_head);
		this_cpu_inc(ivh_head_arm);
		/*
		 * IVH Idea 2 Stage 0: arm right before the real halt -- this is
		 * ivh_head_arm's exact site, so the two counters must agree
		 * (>0.1% deviation means the arm is misplaced). Nothing sets
		 * HEAD_YIELDED yet (Stage 1 territory), so this is always
		 * un-done by the plain reset below, never the yielded branch --
		 * that's the whole Stage-0 acceptance check for this half.
		 *
		 * Stage 0b note: this store is deliberately NOT moved earlier.
		 * That leaves a short HEAD_SPINNING-but-VCPU_HASHED gap running
		 * from the WRITE_ONCE(pn->state, VCPU_HASHED) above through
		 * pv_hash()/xchg() to here. It is the only way an observer can
		 * see HEAD_SPINNING with pp->state != VCPU_RUNNING, so
		 * pv_wait_node() counts that case into its own separate
		 * ivh_head_spinning_prearm rather than folding it into the
		 * tier-2 spinning-window counters. Moving the arm earlier would
		 * close the gap but break the ivh_head_arm == ivh_halt_from_head
		 * site identity that is Stage 0's acceptance check.
		 */
		WRITE_ONCE(pn->head_ctl, HC(0, 0, HEAD_ARMED));
		pv_wait(&lock->locked, _Q_SLOW_VAL);
		if ((READ_ONCE(pn->head_ctl) & 0xffff) == HEAD_YIELDED)
			this_cpu_inc(ivh_head_woke_yielded);
		else
			this_cpu_inc(ivh_head_woke_moot);
		WRITE_ONCE(pn->head_ctl, HC(0, 0, HEAD_IDLE));

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
	/*
	 * IVH Idea 2 Stage 0b: retire the head role explicitly on the acquire
	 * path. Both `goto gotlock` sites above (trylock_clear_pending() inside
	 * the spin loop, and the xchg(&lock->locked, _Q_SLOW_VAL) == 0 race)
	 * leave the loop WITHOUT ever calling pv_wait(), so neither reaches the
	 * "reset head_ctl to HEAD_IDLE" store that follows pv_wait() below.
	 *
	 * As Stage 0 originally shipped that was harmless: HEAD_ARMED was
	 * stored only at :arm, immediately before pv_wait(), with no exit
	 * between the two, so head_ctl was provably already HEAD_IDLE at every
	 * gotlock. HEAD_SPINNING breaks that property -- it is set at the top
	 * of the loop, so it is live across both gotlock sites. Without this
	 * store, a node that acquired the lock here would keep advertising
	 * HEAD_SPINNING to its MCS successor for the whole window between our
	 * acquire and the successor observing node->locked == 1, miscounting a
	 * lock OWNER as a spinning head (and, at Stage 1, offering it up as a
	 * takeover target).
	 *
	 * Placing the reset here rather than at each goto covers both sites and
	 * every future one. It is ordered before the successor's release
	 * (arch_mcs_spin_unlock_contended()'s smp_store_release of
	 * next->locked, back in queued_spin_lock_slowpath()), so any successor
	 * that has observed node->locked == 1 is guaranteed to see HEAD_IDLE.
	 *
	 * Cross-tenure leakage beyond that window is separately impossible:
	 * pv_init_node() re-stores HC(0,0,HEAD_IDLE) on every slowpath entry,
	 * and does so before the smp_wmb() + xchg_tail() that first publishes
	 * this qnode where any successor could find it (qspinlock.c:272-296).
	 */
	WRITE_ONCE(pn->head_ctl, HC(0, 0, HEAD_IDLE));
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
	if (try_cmpxchg_release(&lock->locked, &locked, 0))
		return;

	__pv_queued_spin_unlock_slowpath(lock, locked);
}
#endif /* __pv_queued_spin_unlock */
