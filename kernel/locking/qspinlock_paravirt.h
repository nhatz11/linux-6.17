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
 * Queue node uses: VCPU_RUNNING & VCPU_HALTED (& VCPU_SKIPPED).
 * Queue head uses: VCPU_RUNNING & VCPU_HASHED.
 */
enum vcpu_state {
	VCPU_RUNNING = 0,
	VCPU_HALTED,		/* Used only in pv_wait_node */
	VCPU_HASHED,		/* = pv_hash'ed + VCPU_HALTED */
	/*
	 * G-LOCK-33: this node has been EVICTED FROM THE QUEUE by the holder
	 * because it looked host-preempted, and must re-enter the lock itself.
	 * Written only by pv_evict_walk(), via try_cmpxchg() from VCPU_RUNNING
	 * so it can never displace a VCPU_HALTED (a waiter that halted on
	 * purpose is never evicted) or a VCPU_HASHED (only the queue head is
	 * ever hashed, and the head is never an eviction candidate). Cleared by
	 * the evicted node's own pv_requeue_node().
	 *
	 * Deliberately > VCPU_RUNNING so that every existing
	 * `state != VCPU_RUNNING` test -- tier 1 in pv_wait_early(),
	 * ivh_rot_class() -- treats an evicted node as "not running", which is
	 * the conservative reading in all of them. See <asm/ivh_tsc_beat.h>.
	 *
	 * EVERY transition out of VCPU_RUNNING on a queued node must be a
	 * cmpxchg, never a plain store, or it can clobber an eviction that has
	 * already been committed and strand the waiter permanently. The halt
	 * transition in pv_wait_node() was an unconditional store when this was
	 * first written and hung the machine; see the comment there. kvm_wait()
	 * re-reading *ptr and the post-wait cmpxchg(HALTED -> RUNNING) are NOT
	 * sufficient on their own -- both run after the damage is done.
	 */
	VCPU_SKIPPED,
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
	/*
	 * Phase 0b. Deposited by our promoter just before it hands us the MCS
	 * baton, read by us at acquisition. Lives in the 3-byte padding hole
	 * after ->state, so sizeof(struct pv_node) stays exactly 32 and
	 * ->head_ctl stays at offset 24 -- the BUILD_BUG_ON against
	 * sizeof(struct qnode) below still holds and no cacheline layout
	 * changes. Written only under ivh_pv_rot_probe.
	 */
	u8			rot_flags;
	/*
	 * G-LOCK-33 starvation bound. How many times THIS tenure has been
	 * evicted by pv_evict_walk(). Zeroed by pv_init_node() on genuine queue
	 * entry and deliberately NOT by pv_requeue_node(), so it accumulates
	 * across re-entries; at ivh_pv_requeue_max the walk refuses to evict.
	 * Written only by its own CPU (pv_requeue_node) and read by the walker;
	 * a distinct byte from ->state and ->rot_flags, and byte stores do not
	 * tear into each other on x86. Lives in the padding hole after
	 * ->rot_flags, so sizeof(struct pv_node) stays exactly 32 and
	 * ->head_ctl stays at offset 24.
	 */
	u8			requeues;
	/*
	 * Set ONLY by pv_defer_promote() on the waiter it defers, cleared on
	 * every queue entry. The unlock slowpath used to infer "deferred first
	 * waiter" from mcs.locked == 0, but that premise is false: a thread
	 * whose xchg_tail() returns an empty tail never has ->locked set by
	 * anyone and becomes the head with locked == 0. Such a head was being
	 * misrouted into pv_deferred_handoff(), whose walk starts at the head
	 * ITSELF -- so the head got evicted, and since pv_wait_head_or_lock()
	 * never tests VCPU_SKIPPED and overwrites ->state with plain stores,
	 * the mark was destroyed and the waiter never requeued. Lives in the
	 * last padding byte; sizeof(struct pv_node) stays 32.
	 */
	u8			deferred;
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
			/*
			 * Phase 0b: CONFIG_LOCK_EVENT_COUNTS is off in this
			 * build, so pv_lock_stealing above compiles away. This
			 * is the same event, counted unconditionally because a
			 * steal is already rare relative to the cmpxchg that
			 * just succeeded, and because it is the control the
			 * whole rotation question turns on: a steal here is a
			 * dead head's cost ALREADY being recovered.
			 */
			if (unlikely(READ_ONCE(ivh_pv_rot_probe)) &&
			    (val & _Q_TAIL_MASK))
				this_cpu_inc(ivh_rot_steals);
			/*
			 * G-LOCK-30, site A6: a stealer is a holder a queue head
			 * with no predecessor may be waiting on. Same gate bit as
			 * the fast path (A1/A2 via ivh_lock_set_holder()).
			 */
			if (unlikely(READ_ONCE(ivh_lock_holder_enabled) &
				     IVH_HOLDER_EN_CS_FAST))
				__ivh_cs_owner_stamp(lock);
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

/* G-LOCK-33: live pv_hash entries; see the leak detector inside pv_hash(). */
atomic_t ivh_pv_hash_live = ATOMIC_INIT(0);
EXPORT_SYMBOL_GPL(ivh_pv_hash_live);
atomic_t ivh_pv_hash_hwm = ATOMIC_INIT(0);
EXPORT_SYMBOL_GPL(ivh_pv_hash_hwm);

/*
 * G-LOCK-33: does @lock already own a hash entry? Used by the head to avoid
 * creating a SECOND entry for one lock. Same probe walk pv_hash() would do, so
 * the cost is identical and it runs only on the head's cold halt path.
 */
static bool pv_lock_is_hashed(struct qspinlock *lock)
{
	unsigned long offset, hash = hash_ptr(lock, pv_lock_hash_bits);
	struct pv_hash_entry *he;

	for_each_hash_entry(he, offset, hash) {
		struct qspinlock *l = READ_ONCE(he->lock);

		if (l == lock)
			return true;
		/*
		 * DO NOT stop at a NULL. pv_unhash() deletes with a bare
		 * WRITE_ONCE(he->lock, NULL) and leaves NO TOMBSTONE, so holes
		 * appear in the middle of live probe sequences. Stopping at the
		 * first hole makes this return a false negative: the guard is
		 * silently bypassed (ivh_head_foreign_hash does not even fire),
		 * pv_hash() then fills that same hole -- placing the duplicate
		 * IN FRONT of the original in probe order -- and pv_unhash(),
		 * which correctly scans the whole sequence, frees the duplicate
		 * and orphans the original forever. Self-accelerating: every
		 * orphan lengthens chains and creates more hole-before-entry
		 * configurations. Scan the full sequence, exactly as pv_unhash()
		 * does; it is the same bounded walk.
		 */
	}
	return false;
}

static struct qspinlock **pv_hash(struct qspinlock *lock, struct pv_node *node, int site)
{
	unsigned long offset, hash = hash_ptr(lock, pv_lock_hash_bits);
	struct pv_hash_entry *he;
	int hopcnt = 0;

	/*
	 * G-LOCK-33 duplicate detector. The invariant is "every blocked lock
	 * only ever consumes a single entry". If eviction can get one lock
	 * hashed TWICE -- once by pv_kick_node() for the promoted node, once by
	 * a requeued node that became head -- then pv_unhash() removes only one
	 * of them and the other leaks forever. Scan the probe sequence for an
	 * existing entry naming this lock before inserting. Diagnostic build
	 * only: O(hops) on an already-cold path, and it answers the one
	 * remaining hypothesis directly instead of by inference.
	 */
	{
		struct pv_hash_entry *dhe;
		unsigned long doff;

		for_each_hash_entry(dhe, doff, hash) {
			if (READ_ONCE(dhe->lock) == lock) {
				this_cpu_inc(ivh_hash_dup);
				if (site)
					this_cpu_inc(ivh_hash_dup_head);
				else
					this_cpu_inc(ivh_hash_dup_kick);
				break;
			}
			/* No hole-stop: see pv_lock_is_hashed(). Stopping here made
			 * ivh_hash_dup a LOWER BOUND, not a count. */
		}
	}

	for_each_hash_entry(he, offset, hash) {
		struct qspinlock *old = NULL;
		hopcnt++;
		if (try_cmpxchg(&he->lock, &old, lock)) {
			WRITE_ONCE(he->node, node);
			lockevent_pv_hop(hopcnt);
			/*
			 * G-LOCK-33 leak gauge. The table cannot legitimately
			 * fill: entries <= qnodes in use <= 4 * nr_cpus (64 here)
			 * against 256 slots, because "every blocked lock only ever
			 * consumes a single entry" and "the lock owner unhashes
			 * before it releases". A climbing high-water mark means an
			 * entry was hashed whose lock never reaches the unlock
			 * slowpath again -- a stuck or stranded owner/waiter.
			 *
			 * DELIBERATELY NO WARN/printk HERE. pv_hash() runs from the
			 * qspinlock slowpath with the lock held and preemption off;
			 * printk takes locks of its own and re-enters this very
			 * path. An earlier version WARNed here and deadlocked the
			 * machine during boot. Record a high-water mark instead and
			 * let userspace poll ivh_pv_hash_hwm -- same diagnostic,
			 * no reentrancy.
			 */
			{
				int live = atomic_inc_return(&ivh_pv_hash_live);
				int hwm = atomic_read(&ivh_pv_hash_hwm);

				if (unlikely(live > hwm))
					atomic_cmpxchg(&ivh_pv_hash_hwm, hwm, live);
			}
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
			this_cpu_inc(ivh_hash_rel_unhash);
			atomic_dec(&ivh_pv_hash_live);
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
static inline bool is_wait_preempted(int cpu, bool tier2)
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
	 *
	 * G-LOCK-25: this function now has a SECOND call site --
	 * ivh_pv_tier1_confirm's tier-1 confirmation check
	 * (kernel/locking/qspinlock_paravirt.h's pv_wait_early()). That call
	 * is answering a different question ("is THIS specific prev actually
	 * stale" for a node that already looked non-running) than tier 2's own
	 * call ("does prev look preempted at all"), so it must NOT be folded
	 * into ivh_beat_tier2_checked/_fired or ivh_beat_age_hist_raw --
	 * mixing the two populations would corrupt tier 2's own fire-rate
	 * measurement with confirm-check traffic. `tier2` selects which
	 * counter pair this call increments; the callee cannot infer it from
	 * `src` alone since both call sites can be live at the same time.
	 */
	if (tier2) {
		this_cpu_inc(ivh_beat_tier2_checked);
		if (beat)
			this_cpu_inc(ivh_beat_tier2_fired);
	} else {
		this_cpu_inc(ivh_tier1_confirm_checked);
		if (beat)
			this_cpu_inc(ivh_tier1_confirm_agreed);
		else
			this_cpu_inc(ivh_tier1_confirm_disagreed);
	}

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
	 *
	 * G-LOCK-25: gated on `tier2` for the same reason as the counters
	 * above -- this histogram exists to characterize tier 2's OWN age
	 * distribution; confirm-check traffic answering a different question
	 * must not be mixed into it.
	 */
	if (tier2) {
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
 * G-LOCK-30: find the holder of @lock for a queue head with no predecessor, by
 * scanning the per-CPU owner slots for a tag match. Sound only with the
 * release-side clear armed (ivh_cs_owner_clear), which the caller checks: the
 * clear runs strictly before the releasing store, so a slot that still names
 * @lock belongs to a CPU that has not yet released it. Nested or IRQ-context
 * acquisitions on the holder's CPU overwrite or clear its single slot, which
 * can only turn a hit into a miss -- the abstain direction. 16 loads on this
 * guest, taken every PV_PREV_CHECK_MASK iterations, not per iteration.
 */
static noinline int ivh_cs_owner_find(struct qspinlock *lock)
{
	int self = smp_processor_id(), cpu;

	for_each_online_cpu(cpu) {
		if (cpu == self)
			continue;
		if (READ_ONCE(per_cpu(ivh_cs_owner, cpu).lock) == (void *)lock)
			return cpu;
	}
	return -1;
}

/*
 * Is the CURRENT HOLDER of @lock -- not our predecessor-as-a-waiter, which is
 * what pv_wait_early()'s tier 1 and tier 2 answer -- host-preempted?
 *
 * The test is "has the holder gone silent for more than ivh_cs_owed_ticks
 * ticks", NOT "is the holder's heartbeat stale by the waiter threshold". The distinction is the whole point and the earlier
 * specification got it wrong:
 *
 *   ivh_tsc_full_redesign_build_plan_2026-07-29.md sec 1.2 proposed
 *   `cs_stamp != 0 && ivh_beat_stale(holder_cpu)` ("form 1"), arguing that the
 *   tick is a hardirq and fires through preempt_disable(), so a running holder
 *   stays fresh. True -- but fresh at TICK cadence, 1 ms, while
 *   ivh_pv_beat_threshold is 220000 cycles = 100 us in the tuned configuration
 *   this box actually runs (spin_mode 2). A perfectly healthy holder therefore
 *   reads stale for ~90% of every tick period. Form 1 is a false-positive
 *   generator here. It is NOT one at the compiled default of 3300000 cycles
 *   (1.5 ms), which is why the earlier plan's reasoning looked sound.
 *
 * So: deliberately DO NOT read ivh_pv_beat_threshold. A running CPU publishes
 * at least once per tick period. If the holder's newest beat is older than
 * ivh_cs_owed_ticks periods AS OF NOW, it is not running. A holder in a
 * five-millisecond critical section still ticks, and is exonerated. That is
 * the property this predicate exists to have.
 *
 * The silence is aged against NOW, never against the acquisition TSC. An
 * earlier draft tested beat < acq ("no beat since acquiring"), which never
 * fires on a holder that ticked once and was THEN preempted -- the common
 * case for any hold long enough to matter. The acquisition stamp's jobs are
 * the tag (is prev still the holder) and the held_for guard, not liveness.
 *
 * Reaction time is floored at ivh_cs_owed_ticks ticks, ~2-3 ms at the default.
 * That is SLOWER than ivh_pv_spin_threshold's ~45 us at 32768 iterations, and
 * that is fine: the two are not competitors, see the head loop below.
 *
 * Returns true and fills *acq_out / *held_out only on a fire.
 */
static inline bool is_cs_preempted(struct qspinlock *lock, struct pv_node *prev,
				   u64 *acq_out, u64 *held_out)
{
	int cpu;
	bool clr, scan;
	struct ivh_cs_owner *o;
	u64 acq, beat, now;
	s64 held;

	/* ivh_cs_check_calls is counted by the caller, ivh_cs_head_probe_one(),
	 * so the tenure-gate abstains fall inside the same partition. */
	/*
	 * Handoff rotation rewrites ->next, so a head's prev need not be the
	 * holder. With the release-side clear on (G-LOCK-31) that is harmless:
	 * the skipped waiter queued on this lock cannot hold it and its slot
	 * cannot carry an old stamp of it, so the tag compare misses, and the
	 * head either scans (ivh_cs_scan) or abstains into exhaustion. Without
	 * the clear an old stamp could match, so abstain outright.
	 */
	clr = READ_ONCE(ivh_cs_owner_clear);
	if (unlikely(READ_ONCE(ivh_pv_rot_enable) && !clr)) {
		this_cpu_inc(ivh_cs_abstain_rot);
		return false;
	}

	/*
	 * Holder identity. prev->cpu is a per-CPU constant of the qnode slot and
	 * safe to read at any time. G-LOCK-31: with ivh_cs_scan (or G-LOCK-30's
	 * fast mode for a head with no predecessor), a missing prev or a prev
	 * slot that does not name this lock falls through to a scan of the
	 * per-CPU owner slots. Fast-path holders are unstamped unless fast mode
	 * is on, so the scan cannot find them: rule 2, exhaustion.
	 */
	cpu = prev ? prev->cpu : -1;
	scan = clr && (READ_ONCE(ivh_cs_scan) ||
		       (!prev && (READ_ONCE(ivh_lock_holder_enabled) &
				  IVH_HOLDER_EN_CS_FAST)));
	if (scan && (cpu < 0 ||
		     READ_ONCE(per_cpu(ivh_cs_owner, cpu).lock) != (void *)lock)) {
		int found = ivh_cs_owner_find(lock);

		if (found < 0) {
			this_cpu_inc(ivh_cs_scan_miss);
			if (prev) {
				this_cpu_inc(ivh_cs_abstain_tag);
			} else {
				this_cpu_inc(ivh_cs_fast_lookup_miss);
				this_cpu_inc(ivh_cs_abstain_noprev);
			}
			return false;
		}
		this_cpu_inc(ivh_cs_scan_hit);
		if (!prev)
			this_cpu_inc(ivh_cs_fast_lookup_hit);
		cpu = found;
	}
	if (cpu < 0) {
		/* Role B: no predecessor, no scan, no identity. */
		this_cpu_inc(ivh_cs_abstain_noprev);
		return false;
	}

	o = &per_cpu(ivh_cs_owner, cpu);
	if (READ_ONCE(o->lock) != (void *)lock) {
		this_cpu_inc(ivh_cs_abstain_tag);
		return false;
	}
	smp_rmb();		/* pairs with __ivh_cs_owner_stamp()'s smp_wmb() */
	acq = READ_ONCE(o->tsc);

	now  = rdtsc();
	held = (s64)(now - acq);

	/*
	 * SIGNED, for the same reason ivh_beat_age() is: a small negative
	 * cross-vCPU TSC skew must read as "too young", not wrap to an enormous
	 * positive and fire instantly.
	 */
	if (held <= 0) {
		this_cpu_inc(ivh_cs_abstain_skew);
		return false;
	}

	/*
	 * G-LOCK-31 criterion 1: halt once the holder has held longer than its
	 * CPU's last completed hold plus a noise margin. No heartbeat. Read order
	 * is tag -> smp_rmb -> tsc -> last_cs -> tag again: the clear writes
	 * last_cs only after NULLing the tag, and any nested hold that could
	 * update last_cs overwrites the tag first, so the re-read rejects it.
	 * Cannot tell a preempted holder from one running a longer CS than last
	 * time; ivh_cs_noise_cycles is the knob for that.
	 */
	if (READ_ONCE(ivh_cs_criterion) == 1) {
		u64 last = READ_ONCE(o->last_cs);

		if (!last) {
			this_cpu_inc(ivh_cs_abstain_nolastcs);
			return false;
		}
		if ((u64)held <= last + READ_ONCE(ivh_cs_noise_cycles)) {
			this_cpu_inc(ivh_cs_abstain_young);
			return false;
		}
		this_cpu_inc(ivh_cs_long_hold);
		if (READ_ONCE(o->lock) != (void *)lock) {
			this_cpu_inc(ivh_cs_abstain_retag);
			return false;
		}
		*acq_out  = acq;
		*held_out = (u64)held;
		this_cpu_inc(ivh_cs_fired);
		return true;
	}

	if ((u64)held <= (u64)READ_ONCE(ivh_cs_tick_period) *
			 READ_ONCE(ivh_cs_owed_ticks)) {
		this_cpu_inc(ivh_cs_abstain_young);
		return false;
	}

	/* FORM-0 population: long hold, liveness not yet consulted. */
	this_cpu_inc(ivh_cs_long_hold);

	/*
	 * NO_HZ_FULL guard, unconditional and not a command-line assumption.
	 *
	 * On an adaptive-ticks CPU the absence of a beat proves nothing. The
	 * tick-stop decision is taken at tick_nohz_irq_exit()
	 * (kernel/time/tick-sched.c:1295), reached from tick_irq_exit()
	 * (kernel/softirq.c:639-650) whose only context gate is !in_hardirq() --
	 * it tests HARDIRQ_MASK and says nothing about PREEMPT_MASK. And
	 * can_stop_full_tick() (tick-sched.c:358-375) checks six tick_dep bits
	 * and has no preempt_count() or lockdep check at all. So a nohz_full
	 * CPU CAN hold a contended spinlock with the tick stopped:
	 * Documentation/timers/no_hz.rst:139-142, "transitioning to kernel mode
	 * does not automatically change the mode".
	 *
	 * On THIS boot it cannot: /proc/cmdline carries neither nohz_full= nor
	 * dynticks (it carries nohz=off), tick_nohz_full_running is false, and
	 * tick_nohz_full_cpu() is a NOP-patched read-only static branch
	 * (context_tracking_key, DEFINE_STATIC_KEY_FALSE_RO) -- free. It is
	 * here so the predicate does not depend on that staying true, and
	 * ivh_cs_abstain_nohz must read exactly 0 in every run on this host.
	 */
	if (unlikely(tick_nohz_full_cpu(cpu))) {
		this_cpu_inc(ivh_cs_abstain_nohz);
		return false;
	}

	beat = READ_ONCE(per_cpu(ivh_tsc_beat, cpu).stamp);

	/*
	 * Second tag read. Under ivh_cs_owner_clear == 1 the clear commits
	 * ->lock = NULL before the releasing store, so a tag that still reads
	 * @lock HERE means the whole {lock, tsc, beat} observation was taken
	 * inside the hold (build plan sec 1.2 d). Under clear == 0 it is
	 * harmless and catches a nested re-stamp that raced the reads.
	 */
	if (READ_ONCE(o->lock) != (void *)lock) {
		this_cpu_inc(ivh_cs_abstain_retag);
		return false;
	}
	/*
	 * Age the beat against NOW, signed like held above so a beat stamped a
	 * hair after our rdtsc() on a skewed vCPU reads as fresh, not huge.
	 * NOT (beat - acq): that only asks whether the holder ever ticked since
	 * acquiring, and misses every holder preempted after its first tick.
	 */
	if ((s64)(now - beat) <= (s64)(READ_ONCE(ivh_cs_tick_period) *
				       READ_ONCE(ivh_cs_owed_ticks))) {
		/*
		 * Beat is recent. Long hold, but alive. This is the population
		 * form 0 would have fired on and this predicate correctly
		 * exonerates; ivh_cs_healthy_long / ivh_cs_long_hold is the
		 * false-positive audit ratio. Note the holder also publishes
		 * from ivh_beat_publish_in_spin() and pv_init_node() when it is
		 * itself contending on some inner lock, which can only move
		 * samples INTO this branch -- the safe direction.
		 *
		 * The one way a holder goes silent while NOT host-preempted is
		 * halting in pv_wait() on an inner lock it is contending for.
		 * That fires, deliberately: the holder is not making progress
		 * on @lock either way, and for the head's decision stale means
		 * act. Size it with ivh_halt_* if it ever matters.
		 */
		this_cpu_inc(ivh_cs_healthy_long);
		return false;
	}

	*acq_out  = acq;
	*held_out = (u64)held;
	this_cpu_inc(ivh_cs_fired);
	return true;
}

/* Shared log2 bucketing, same convention as ivh_beat_age_hist_raw. */
static __always_inline int ivh_cs_bucket(u64 v)
{
	int b = v ? ilog2(v) : 0;

	return b >= IVH_BEAT_AGE_HIST_BUCKETS ? IVH_BEAT_AGE_HIST_BUCKETS - 1 : b;
}

/*
 * Close the open episode, if any. Keyed on the holder's acquisition TSC in
 * *ep_acq: that key is what makes re-sampling harmless. Zeroes the key.
 */
static __always_inline void ivh_cs_ep_close(u64 *ep_acq, u64 ep_start, u64 now,
					    int why)
{
	u64 d;

	if (!*ep_acq)
		return;
	d = now - ep_start;
	this_cpu_add(ivh_cs_ep_cycles[why], d);
	this_cpu_inc(ivh_cs_ep_events_by_end[why]);
	this_cpu_inc(ivh_cs_ep_hist[why][ivh_cs_bucket(d)]);
	*ep_acq = 0;
}

/*
 * Whole-tenure CONTROL sample (see ivh_cs_tenure_hist in <asm/ivh_tsc_beat.h>):
 * index 1 if at least one detection fired during this tenure, else 0. Closed
 * at the same exits as the episode.
 */
static __always_inline void ivh_cs_tenure_record(u64 start, u64 now, bool det)
{
	u64 d = now - start;
	int i = det ? 1 : 0;

	this_cpu_add(ivh_cs_tenure_cycles[i], d);
	this_cpu_inc(ivh_cs_tenure_hist[i][ivh_cs_bucket(d)]);
}

/*
 * Per-tenure soundness gate (build plan sec 1.2). Called ONCE per head tenure,
 * immediately after set_pending(), only when ivh_cs_head_probe is armed.
 * Returns the verdict ivh_cs_head_probe_one() applies to every sampled check
 * in the tenure.
 */
#define IVH_CS_GATE_OK		0
#define IVH_CS_GATE_TENURE	1	/* waitcnt >= 1, no clear */
#define IVH_CS_GATE_HASHED	2	/* halted at handoff, prev already released */
#define IVH_CS_GATE_LATE	3	/* running at handoff, promptness gate failed */

static noinline u8 ivh_cs_tenure_gate(struct qspinlock *lock,
				      struct pv_node *prev, int waitcnt,
				      bool entered_hashed)
{
	bool clr = READ_ONCE(ivh_cs_owner_clear);
	s64 prompt = (s64)READ_ONCE(ivh_cs_prompt_cycles);
	struct ivh_cs_owner *o;
	void *tag;
	s64 age;

	/*
	 * W: commit our pending store. set_pending() is a plain WRITE_ONCE
	 * (_Q_PENDING_BITS == 8); until it drains from our store buffer, a
	 * stealer's atomic_read() in pv_hybrid_queued_unfair_trylock() on
	 * another CPU can still see pending == 0. On x86-64 smp_mb() is
	 * `lock addl $0,-4(%rsp)` (arch/x86/include/asm/barrier.h:53), a
	 * serialising instruction. Paid once per tenure, only with the probe
	 * armed; acceptance check A7 includes it.
	 */
	smp_mb();

	/* No identity, or rotation: is_cs_preempted() abstains and counts. */
	if (!prev || (READ_ONCE(ivh_pv_rot_enable) && !clr))
		return IVH_CS_GATE_OK;

	if (waitcnt)
		return clr ? IVH_CS_GATE_OK : IVH_CS_GATE_TENURE;

	this_cpu_inc(ivh_cs_tenure0_enter);

	if (entered_hashed) {
		/*
		 * Halted at handoff. pv_kick_node() wrote _Q_SLOW_VAL and did
		 * NOT wake us; we were woken either by prev's unlock-slowpath
		 * pv_kick() (AFTER its release -- a steal may have happened) or
		 * by an unrelated interrupt (IF=1 HLT; prev may still hold).
		 * _Q_SLOW_VAL is written only by prev's pv_kick_node() for THIS
		 * handoff and cleared only by prev's release, so reading it
		 * after W proves prev still holds and, pending now being
		 * committed, will until we acquire. Airtight. Sec 1.2 c-HASHED.
		 */
		this_cpu_inc(ivh_cs_tenure0_hashed);
		if (READ_ONCE(lock->locked) != _Q_SLOW_VAL) {
			this_cpu_inc(ivh_cs_tenure0_hashed_released);
			return clr ? IVH_CS_GATE_OK : IVH_CS_GATE_HASHED;
		}
		return IVH_CS_GATE_OK;
	}

	/*
	 * Running at handoff. No lock-byte witness exists (_Q_LOCKED_VAL both
	 * before and after a steal), so bound the window instead. NOT airtight:
	 * a hold shorter than the bound can complete and be stolen inside it.
	 * Tag first, then tsc: the stamp writes tsc then tag.
	 */
	o   = &per_cpu(ivh_cs_owner, prev->cpu);
	tag = READ_ONCE(o->lock);
	age = (s64)(rdtsc() - READ_ONCE(o->tsc));

	if (tag == (void *)lock) {
		this_cpu_inc(ivh_cs_prompt_hist[ivh_cs_bucket(age > 0 ? (u64)age : 0)]);
		if (age > prompt) {
			this_cpu_inc(ivh_cs_tenure0_late);
			return clr ? IVH_CS_GATE_OK : IVH_CS_GATE_LATE;
		}
		return IVH_CS_GATE_OK;
	}

	/*
	 * Tag already cleared at W under clr == 1: prev released before our
	 * pending committed. If the promptness gate would have PASSED this,
	 * it is exactly a tenure the clr == 0 configuration would have
	 * admitted with a stale stamp. Shadow-count it (sec 1.4).
	 */
	if (clr && !tag && age <= prompt)
		this_cpu_inc(ivh_cs_shadow_gate_pass_released);

	return IVH_CS_GATE_OK;	/* is_cs_preempted() abstains on the tag */
}

/*
 * One sampled probe. noinline on purpose: the head's spin loop is the hottest
 * loop in the kernel under contention and must stay small in icache. Returns
 * true if the predicate fired (Stage B acts on that; Stage A ignores it).
 */
static noinline bool ivh_cs_head_probe_one(struct qspinlock *lock,
					   struct pv_node *prev, u8 gate,
					   u64 *ep_acq, u64 *ep_start,
					   bool *ep_any)
{
	u64 acq = 0, held = 0, now;

	this_cpu_inc(ivh_cs_check_calls);

	/*
	 * Apply the per-tenure soundness verdict (ivh_cs_tenure_gate()). Any
	 * non-OK verdict means `prev` may no longer be the holder while its
	 * stamp tag still names @lock -- a potential FALSE POSITIVE, the bad
	 * direction -- so the whole tenure abstains.
	 */
	switch (gate) {
	case IVH_CS_GATE_TENURE:
		this_cpu_inc(ivh_cs_abstain_tenure);
		return false;
	case IVH_CS_GATE_HASHED:
		this_cpu_inc(ivh_cs_abstain_hashed);
		return false;
	case IVH_CS_GATE_LATE:
		this_cpu_inc(ivh_cs_abstain_late);
		return false;
	}

	if (!is_cs_preempted(lock, prev, &acq, &held)) {
		/*
		 * Not firing does NOT close the episode -- a single sampled
		 * miss inside a genuine stall (say the holder briefly ticked
		 * from an inner lock's spin loop) would otherwise fragment one
		 * episode into several and re-inflate exactly the event count
		 * this design exists to deflate. Only an ACQUIRE, a holder
		 * change, or tenure exit closes an episode.
		 */
		return false;
	}

	now = rdtsc();

	if (*ep_acq && *ep_acq != acq) {
		/* The holder changed under us: close the old episode as an
		 * upper bound and open a new one. */
		ivh_cs_ep_close(ep_acq, *ep_start, now, IVH_CS_EP_HOLDER_CHANGED);
	}
	if (!*ep_acq) {
		*ep_acq   = acq;
		*ep_start = now;
		*ep_any   = true;
		this_cpu_inc(ivh_cs_ep_events);
	}
	return true;
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
static __always_inline void ivh_beat_publish_in_spin(unsigned long loop)
{
	if (likely(!READ_ONCE(ivh_pv_preempt_src)))
		return;
	if (loop & READ_ONCE(ivh_pv_beat_publish_mask))
		return;

	ivh_tsc_beat_publish();
	this_cpu_inc(ivh_beat_publishes);
}

/*
 * G-LOCK-25 scoping: record how many raw TSC cycles ONE pv_wait() call
 * actually blocked for, bucketed by why the waiter decided to stop spinning
 * (enum pv_bail_cause). Behavior-neutral -- this only reads, records, and
 * returns; nothing here feeds back into any decision. Same log2 bucketing
 * as ivh_beat_age_hist_raw, for the same reason (a real wait distribution is
 * heavy-tailed; a mean alone would hide exactly the short-halt population
 * this exists to characterize).
 */
static __always_inline void ivh_node_halt_record(enum pv_bail_cause cause, u64 cycles)
{
	int bucket = cycles ? ilog2(cycles) : 0;

	if (bucket >= IVH_BEAT_AGE_HIST_BUCKETS)
		bucket = IVH_BEAT_AGE_HIST_BUCKETS - 1;

	this_cpu_add(ivh_node_halt_cycles[cause], cycles);
	this_cpu_inc(ivh_node_halt_events[cause]);
	this_cpu_inc(ivh_node_halt_hist[cause][bucket]);
}

/*
 * Return the reason (enum pv_bail_cause) this waiter should stop spinning
 * and halt, or PV_BAIL_NONE if it's not time to check yet / nothing fired.
 *
 * G-LOCK-25: was `bool`; widened to a cause so pv_wait_node() can record
 * per-bail-cause halt-duration histograms without re-deriving why a given
 * pass bailed. Every existing `return true`/`return false` site below maps
 * 1:1 onto a truthy/PV_BAIL_NONE enum value, so this is a pure signature
 * widening, not a behavior change by itself.
 */
static inline enum pv_bail_cause
pv_wait_early(struct pv_node *prev, unsigned long loop)
{
	unsigned long mode;

	if ((loop & PV_PREV_CHECK_MASK) != 0)
		return PV_BAIL_NONE;

	mode = READ_ONCE(ivh_adaptive_mode);

	/*
	 * G-LOCK-22-hybrid: for mode ADAPTIVE only, and only when the sysctl
	 * below is on, suppress early bail entirely when there is nowhere
	 * productive to bail TO -- !ivh_pv_allowed() (no hypercall available)
	 * and irqs_disabled() (the eventual ivh_pv_wait() call is just going
	 * to busy-spin no matter when it's reached, see its comment). Bailing
	 * early in that case doesn't change the FINAL wait behavior, it only
	 * moves this waiter to pv_kick_node()'s _Q_SLOW_VAL/hash bookkeeping
	 * sooner, forcing the eventual unlocker onto the slow-unlock path and
	 * a real, unneeded IPI on a third CPU's critical path. Default OFF:
	 * the sign of this trade is not proven, see the sysctl's own comment.
	 * irqs_disabled() is a valid predictor here -- IRQ state is a property
	 * of the caller's context and cannot change under a spinning waiter.
	 */
	if (mode == IVH_MODE_ADAPTIVE && READ_ONCE(ivh_adaptive_irqoff_bail_gate) &&
	    !ivh_pv_allowed() && irqs_disabled()) {
		this_cpu_inc(ivh_earlybail_suppressed);
		return PV_BAIL_NONE;
	}

	/*
	 * G-LOCK-21-spin: ivh_pv_tier1_enable == 0 removes tier 1 entirely --
	 * a waiter then only ever bails early via tier 2 below (and only then
	 * if ivh_adaptive_mode == ADAPTIVE and ivh_pv_preempt_src != 0; outside
	 * that, disabling tier 1 leaves NO early-bail signal at all, and a
	 * waiter only stops spinning by exhausting ivh_pv_spin_threshold).
	 * Default 1 reproduces stock behavior exactly -- tier 1 is upstream's
	 * entire pv_wait_early() check.
	 */
	if (READ_ONCE(ivh_pv_tier1_enable) &&
	    READ_ONCE(prev->state) != VCPU_RUNNING) {
		unsigned long confirm = READ_ONCE(ivh_pv_tier1_confirm);

		/*
		 * G-LOCK-25: prev->state == VCPU_HALTED is a FACT about prev,
		 * not a guess -- but it carries no information about HOW LONG
		 * prev has been down, and under mode ADAPTIVE most halted node
		 * predecessors are halted because THEIR OWN predecessor
		 * tripped tier 2, so one tier-2 inference walks down the queue
		 * tail one cheap byte load at a time (measured: ~2.77 tier-1
		 * fires per tier-2 fire).
		 *
		 * A halted vCPU publishes no heartbeat (see ivh_beat_publish_
		 * in_spin() and account_process_tick()'s tick-driven publish,
		 * both skipped while halted), so is_wait_preempted() on an
		 * already-halted prev is exactly a "how long has prev been
		 * down" freshness check, needing no new per-node state.
		 *
		 * confirm==0: unchanged, bail immediately (upstream behavior).
		 * confirm==1: SHADOW -- compute the verdict, record it, STILL
		 *   BAIL either way. Zero behavior change; exists to measure
		 *   whether confirmation is a genuine filter (short surviving
		 *   halts, long suppressed ones) or an indiscriminate throttle
		 *   (same distribution, just fewer) before anything acts on it.
		 * confirm==2: authoritative -- an unconfirmed trip (heartbeat
		 *   still reads fresh) does not bail.
		 *
		 * Default 0 because the sign of this trade is NOT proven: this
		 * is a "bail later instead of earlier" change, and the closely
		 * analogous ivh_pv_spin_threshold experiment measured ~9%
		 * SLOWER. Same posture and precedent as
		 * ivh_adaptive_irqoff_bail_gate above.
		 */
		if (mode == IVH_MODE_ADAPTIVE && confirm &&
		    READ_ONCE(ivh_pv_tier2_enable)) {
			if (is_wait_preempted(prev->cpu, false)) {
				this_cpu_inc(ivh_beat_tier1_fired);
				return PV_BAIL_TIER1_AGREED;
			}
			if (confirm == 2) {
				this_cpu_inc(ivh_tier1_suppressed);
				return PV_BAIL_NONE;
			}
			this_cpu_inc(ivh_beat_tier1_fired);
			return PV_BAIL_TIER1_DISAGREED;
		}

		this_cpu_inc(ivh_beat_tier1_fired);
		return PV_BAIL_TIER1;
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
	if (mode != IVH_MODE_ADAPTIVE)
		return PV_BAIL_NONE;

	/* G-LOCK-31: no waiter-side staleness check at all with tier 2 off. */
	if (!READ_ONCE(ivh_pv_tier2_enable))
		return PV_BAIL_NONE;

	return is_wait_preempted(prev->cpu, true) ? PV_BAIL_TIER2 : PV_BAIL_NONE;
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
	 * Per-tenure reset, unconditional: every node runs pv_init_node() on
	 * queue entry, so this is the exact point that guarantees we never read
	 * a flag deposited during a previous, unrelated occupancy of this
	 * qnodes[] slot -- including across a toggle of ivh_pv_rot_probe.
	 */
	pn->rot_flags = 0;
	/*
	 * G-LOCK-33: the eviction count is per-TENURE, and this is the only
	 * place it is cleared. pv_requeue_node() must not clear it -- carrying
	 * it across re-entries is exactly what bounds starvation.
	 */
	pn->requeues = 0;
	pn->deferred = 0;

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
 * G-LOCK-33: re-arm an EVICTED node so its owner can re-enter the acquire
 * path. Called only from queued_spin_lock_slowpath() after pv_wait_node()
 * returns PV_WAIT_REQUEUE, on the node's own CPU, with preemption disabled.
 *
 * Deliberately NOT pv_init_node(): that would clear ->requeues, which is the
 * starvation bound and must accumulate across re-entries. Everything else is
 * reset, because the node is about to republish its tail code and must look
 * exactly like a fresh arrival to anyone who links behind it.
 *
 * ORDERING. ->state must return to VCPU_RUNNING and ->next must be NULL before
 * the caller's xchg_tail() publishes this node's encode_tail(cpu, idx) code
 * again, or a later holder could reach a node that still reads VCPU_SKIPPED
 * (and would evict it a second time on stale evidence) or could follow a stale
 * ->next into a node that is no longer behind us. The caller's existing
 * smp_wmb() before xchg_tail() provides exactly that barrier -- the requeue
 * path jumps to a label above it, so this is free and not separately barriered
 * here.
 *
 * The node's memory is safe throughout: an evicted waiter has not left
 * queued_spin_lock_slowpath(), so it still owns its qnodes[] index and no
 * other user on this CPU can take that slot.
 */
static void pv_requeue_node(struct mcs_spinlock *node)
{
	struct pv_node *pn = (struct pv_node *)node;
	unsigned int r = READ_ONCE(pn->requeues);

	this_cpu_inc(ivh_evict_requeued);
	if (r < 255)
		pn->requeues = (u8)(r + 1);

	/*
	 * G-LOCK-33 fairness evidence, recorded HERE and nowhere else.
	 *
	 * Incrementing bucket min(new count, top) on every eviction makes the
	 * array a SURVIVAL CURVE: bucket i counts tenures that reached i
	 * evictions, so it is monotonically non-increasing and the highest
	 * non-empty bucket is the observed starvation bound directly. That is
	 * strictly better evidence than sampling ->requeues at acquisition,
	 * and -- more importantly -- it is complete. A tenure can acquire by
	 * three different routes (the trylock at requeue:, promotion out of
	 * pv_wait_node(), or finding an empty queue after xchg_tail() and
	 * going straight to the head loop), and any per-acquisition sampling
	 * site would silently miss at least one of them.
	 *
	 * ivh_pv_requeue_max is clamped to IVH_REQUEUE_MAX_CAP (255), so the
	 * top bucket saturating is a real possibility and is documented as
	 * ">= that many", not "exactly".
	 */
	{
		unsigned int b = pn->requeues;

		if (b >= IVH_EVICT_REQ_HIST_BUCKETS)
			b = IVH_EVICT_REQ_HIST_BUCKETS - 1;
		this_cpu_inc(ivh_evict_requeue_hist[b]);
	}

	pn->state = VCPU_RUNNING;
	pn->head_ctl = HC(0, 0, HEAD_IDLE);
	pn->rot_flags = 0;
	pn->deferred = 0;

	/*
	 * Re-seed the heartbeat for the same reason pv_init_node() does: we are
	 * about to become somebody's predecessor again, and a stamp left over
	 * from before the eviction would read stale to them immediately.
	 */
	if (unlikely(READ_ONCE(ivh_pv_preempt_src))) {
		ivh_tsc_beat_publish();
		this_cpu_inc(ivh_beat_publishes);
	}
}

/*
 * G-LOCK-33: the evicted waiter took the lock on the opportunistic trylock at
 * queued_spin_lock_slowpath()'s requeue: label instead of re-joining the tail.
 * That is the better half of the retry -- we were passed over precisely because
 * the holder was handing off, so the lock is often free by the time we look.
 *
 * Called on EVERY slowpath entry, not just retries, because it shares the
 * existing post-init trylock; hence the ->requeues test, which is one byte load
 * from the cacheline pv_init_node() just wrote and a predicted-not-taken branch.
 */
static __always_inline void pv_requeue_steal(struct mcs_spinlock *node)
{
	struct pv_node *pn = (struct pv_node *)node;

	if (unlikely(READ_ONCE(pn->requeues)))
		this_cpu_inc(ivh_evict_steal_ok);
}

/*
 * Wait for node->locked to become true, halt the vcpu after a short spin.
 * pv_kick_node() is used to set _Q_SLOW_VAL and fill in hash table on its
 * behalf.
 *
 * G-LOCK-33: returns PV_WAIT_REQUEUE if this node was evicted from the queue
 * while waiting, in which case node->locked will NEVER be set and the caller
 * must re-enter rather than spin. PV_WAIT_OK otherwise, with node->locked
 * already 1 (the caller still does the load-acquire for us).
 */
static int pv_wait_node(struct mcs_spinlock *node, struct mcs_spinlock *prev,
			  struct qspinlock *lock)
{
	struct pv_node *pn = (struct pv_node *)node;
	struct pv_node *pp = (struct pv_node *)prev;
	bool wait_early;
	enum pv_bail_cause cause = PV_BAIL_NONE;
	unsigned long loop;
	unsigned long threshold;
	u64 halt_tsc;

	for (;;) {
		/*
		 * G-LOCK-21-spin: read once per attempt, not per iteration --
		 * a live sysctl change only takes effect on the next attempt,
		 * never mid-spin, so SPIN_THRESHOLD - loop accounting below
		 * stays internally consistent within a single attempt.
		 */
		threshold = READ_ONCE(ivh_pv_spin_threshold);
		for (wait_early = false, loop = threshold; loop; loop--) {
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
				this_cpu_add(ivh_node_spin_success_iters_sum, threshold - loop);
				this_cpu_inc(ivh_node_spin_success_attempts);
				/*
				 * G-LOCK-33 accounting hole. ivh_evict_marked and
				 * ivh_evict_requeued should balance once every
				 * evicted vCPU has been rescheduled and looked at
				 * its own state -- and at idle they did, exactly
				 * (13308/13308). Under contention 9,091 of 65,884
				 * evictions (13.8%) never requeued, and the gap did
				 * NOT close after the machine went quiet.
				 *
				 * There are only two exits from this function, and
				 * PV_WAIT_REQUEUE has one call site that always
				 * counts. So those nodes must have left through
				 * HERE -- ->locked set on a node we had already
				 * marked VCPU_SKIPPED. The walk promotes `after`,
				 * never the node it just evicted, so that should
				 * be impossible. Count it rather than theorise:
				 * ->locked is tested BEFORE the SKIPPED test above,
				 * so if this fires, the check ordering is where the
				 * evictions are disappearing.
				 */
				if (unlikely(READ_ONCE(pn->state) == VCPU_SKIPPED))
					this_cpu_inc(ivh_evict_ok_while_skipped);
				return PV_WAIT_OK;
			}
			/*
			 * G-LOCK-33: evicted. Checked SECOND, after node->locked
			 * and before anything expensive, and on every iteration:
			 * pn->state lives at offset 20 of this same 32-byte
			 * pv_node, so it is the same cacheline the load above
			 * just brought in exclusive -- an L1 hit against a
			 * cpu_relax() measured at ~26 cycles.
			 *
			 * Ordering against the evictor is trivially safe in both
			 * directions: the eviction is a try_cmpxchg from
			 * VCPU_RUNNING, so it cannot fire once we have stored
			 * VCPU_HALTED below, and if it fired before that store
			 * the evictor promoted somebody else and nobody will
			 * ever set our ->locked -- which is precisely what this
			 * return tells the caller.
			 *
			 * This is also the ONLY site that needs the check. A
			 * halted node is never evicted (the cmpxchg requires
			 * VCPU_RUNNING), and after a wakeup we fall back through
			 * the outer for(;;) into this loop again.
			 */
			if (unlikely(READ_ONCE(pn->state) == VCPU_SKIPPED))
				return PV_WAIT_REQUEUE;
			cause = pv_wait_early(pp, loop);
			if (cause) {
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
		this_cpu_add(ivh_node_spin_iters_sum, threshold - loop);
		this_cpu_inc(ivh_node_spin_attempts);

		/*
		 * G-LOCK-25: `cause` holds whatever the LAST pv_wait_early()
		 * call in the loop above returned. If this pass ended via the
		 * wait_early break, that is the real bail cause. If it ended
		 * by natural exhaustion (loop == 0, wait_early still false),
		 * the last call actually returned PV_BAIL_NONE (that's WHY the
		 * loop kept going) -- override it here, once, rather than
		 * carrying a stale non-cause into the halt-duration histogram.
		 */
		if (!wait_early)
			cause = PV_BAIL_EXHAUST;

		/*
		 * Order pn->state vs pn->locked thusly:
		 *
		 * [S] pn->state = VCPU_HALTED	  [S] next->locked = 1
		 *     MB			      MB
		 * [L] pn->locked		[RmW] pn->state = VCPU_HASHED
		 *
		 * Matches the cmpxchg() from pv_kick_node().
		 */
		/*
		 * G-LOCK-33 FIX (hard hang, found 2026-09-16). This was an
		 * unconditional smp_store_mb(pn->state, VCPU_HALTED), which
		 * silently CLOBBERED a VCPU_SKIPPED committed by an evictor any
		 * time the eviction landed between the SKIPPED test in the spin
		 * loop above and this store.
		 *
		 * That is unrecoverable, not merely a missed optimisation: the
		 * evictor's try_cmpxchg had already succeeded, so it walked past
		 * this node and promoted somebody else. The node is OUT OF THE
		 * QUEUE -- no predecessor points at it, so nothing will ever set
		 * its ->locked, and the evictor kicked the node it promoted, not
		 * this one. Storing HALTED here and then sleeping in pv_wait()
		 * parks the waiter forever inside queued_spin_lock_slowpath with
		 * preemption off and its qnode index held, and every later
		 * acquirer of that lock queues behind the corpse. Observed as a
		 * whole-VM hang with no oops, because no CPU got far enough to
		 * log one.
		 *
		 * Making the transition conditional closes it: we may only halt
		 * if we are still VCPU_RUNNING. The only value that can beat us
		 * here is VCPU_SKIPPED -- our own CPU is the sole writer of
		 * RUNNING/HALTED for our node, and VCPU_HASHED requires
		 * pv_kick_node() to win a HALTED->HASHED cmpxchg, which cannot
		 * have happened while we are still RUNNING.
		 *
		 * Ordering is unchanged. smp_store_mb() is an xchg (full
		 * barrier) and a non-relaxed try_cmpxchg() is a lock cmpxchg
		 * (also full), so the required
		 *     [S] pn->state = HALTED ; MB ; [L] node->locked
		 * against pv_kick_node()'s
		 *     [S] next->locked = 1   ; MB ; [RmW] pn->state = HASHED
		 * still holds on both the success and the failure path.
		 */
		{
			u8 hold = VCPU_RUNNING;

			if (!try_cmpxchg(&pn->state, &hold, VCPU_HALTED)) {
				if (hold == VCPU_SKIPPED) {
					this_cpu_inc(ivh_evict_halt_averted);
					return PV_WAIT_REQUEUE;
				}
				/*
				 * Defensive: VCPU_HASHED is argued impossible
				 * above. Re-enter the spin loop rather than
				 * halt; it will observe ->locked and return
				 * PV_WAIT_OK.
				 */
				continue;
			}
		}

		if (!READ_ONCE(node->locked)) {
			lockevent_inc(pv_wait_node);
			lockevent_cond_inc(pv_wait_early, wait_early);
			this_cpu_inc(ivh_halt_from_node);
			halt_tsc = ivh_raw_tsc();
			pv_wait(&pn->state, VCPU_HALTED);
			ivh_node_halt_record(cause, ivh_raw_tsc() - halt_tsc);
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
	 *
	 * Unreachable: the for(;;) above is exited only by the two returns in
	 * the inner loop. Kept so the compiler sees a value on every path.
	 */
	return PV_WAIT_OK;
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
	this_cpu_inc(ivh_hash_ins_kick);
	(void)pv_hash(lock, pn, 0);	/* site 0 = pv_kick_node */

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
 * ============================================================================
 * pv_handoff_rotate() -- handoff-time rotation
 * ============================================================================
 *
 * Called by the thread that has JUST ACQUIRED the lock (set_locked() at
 * kernel/locking/qspinlock.c:447), immediately before it promotes its
 * successor via arch_mcs_spin_unlock_contended(&next->locked).
 *
 * Why this call site: at that point the caller (a) holds the lock, so it has
 * mutual exclusion, and (b) has not yet run __this_cpu_dec(qnodes[0].mcs.count),
 * so it still owns its qnode slot and is still a queue member. That is exactly
 * the position CNA's cna_order_queue() splices from, and it is what makes a
 * forward walk of ->next safe here in a way it would not be from the unlock
 * path. See tools/bpf/docs/ivh_handoff_rotation_feasibility_2026-09-13.md.
 *
 * TWO KNOBS, AND THE DISTINCTION IS THE WHOLE SAFETY POSTURE:
 *
 *   ivh_pv_rot_probe  -- PHASE 0, DETECT ONLY. Walks the queue and counts.
 *       The only thing it writes is the promoted successor's own rot_flags
 *       byte, which the successor reads back in ivh_rot_ack(); no ->next
 *       pointer and no *nextp is ever touched, so the queue is byte-for-byte
 *       what upstream would have built.
 *         ivh_rot_preempted / ivh_rot_handoffs  -- how often is the promotion
 *             target preempted, i.e. how often would promoting it create a
 *             "dead head"?
 *         ivh_rot_depth_hist[]  -- when it is, how far back is the first live
 *             waiter? Bucket 0 = successor was live (common case). Bucket
 *             IVH_ROT_HOP_CAP = no live node among the first HOP_CAP nodes.
 *         ivh_rot_splice_ok  -- and of those, how many were actually LEGAL to
 *             splice. This is the real addressable number; the histogram
 *             above overstates it, because it does not check the live node's
 *             own ->next.
 *
 *   ivh_pv_rot_enable -- PHASE 1. The only setting under which any ->next
 *       pointer is rewritten. See <asm/ivh_tsc_beat.h> for the full safety
 *       argument (the two facts about ->next that make the splice legal, why
 *       every node behind us is frozen, why no barrier beyond the caller's
 *       existing smp_store_release is needed, and how starvation is bounded).
 *       Each individual store is justified again at its own site below.
 *
 * BOTH REQUIRE ivh_pv_preempt_src == 2. At any other value the liveness test
 * degrades to vcpu_is_preempted(), hardwired false on a host with no real
 * steal-time page (this one), so every node reads live, the probe correctly
 * reports "never preempted", and rotation is self-disabling -- it can never
 * fire on a signal it does not have.
 */
#define IVH_ROT_LIVE		0
#define IVH_ROT_PREEMPTED	1
#define IVH_ROT_HALTED		2

static __always_inline int ivh_rot_class(struct mcs_spinlock *n, unsigned long src,
					 u64 thr, u64 now)
{
	struct pv_node *pn = (struct pv_node *)n;

	/*
	 * G-LOCK-31: state FIRST, then the heartbeat. A waiter that halted on
	 * purpose (anything but VCPU_RUNNING) is HALTED regardless of its beat --
	 * a tier-1 halt lands within 256 iterations of a publish, so its beat is
	 * often still fresh. Only a waiter that is VCPU_RUNNING by its own account
	 * and silent is PREEMPTED. Only the node's own CPU writes its state, and
	 * walked nodes are never HASHED (pv_kick_node() only touches the node
	 * being promoted).
	 */
	if (READ_ONCE(pn->state) != VCPU_RUNNING)
		return IVH_ROT_HALTED;

	/*
	 * Deliberately NOT is_wait_preempted(): that function has mandatory
	 * counter side effects on both paths (tier2 == true bumps
	 * ivh_beat_tier2_checked/_fired and ivh_beat_age_hist_raw; tier2 ==
	 * false bumps the ivh_tier1_confirm_* set, and either writes
	 * ivh_beat_min_age). This probe is a THIRD question -- "is the
	 * promotion target preempted" -- and folding it into either population
	 * would corrupt that population's own fire-rate measurement, exactly
	 * the hazard is_wait_preempted()'s own comment warns about.
	 *
	 * The src test mirrors is_wait_preempted() EXACTLY, including that
	 * src == 1 falls back to the KVM bit rather than the heartbeat: at
	 * src == 1 the kernel acts on vcpu_is_preempted(), so a probe that
	 * reported the heartbeat instead would count rotations that Phase 1
	 * would never actually perform.
	 */
	if (src != 2)
		return vcpu_is_preempted(pn->cpu) ? IVH_ROT_PREEMPTED : IVH_ROT_LIVE;

	return (s64)(now - READ_ONCE(per_cpu(ivh_tsc_beat, pn->cpu).stamp)) > (s64)thr ?
		IVH_ROT_PREEMPTED : IVH_ROT_LIVE;
}

/*
 * ============================================================================
 * G-LOCK-33: pv_evict_walk() -- the shared eviction logic
 * ============================================================================
 *
 * ONE function, used by BOTH promotion points, so the two cannot drift:
 *
 *   ivh_pv_skip_point == 0  pv_handoff_rotate(), called by the thread that has
 *                           just acquired the lock, before its critical
 *                           section. Classification is pre-CS state.
 *   ivh_pv_skip_point == 1  pv_deferred_handoff(), called from the unlock
 *                           slowpath after pv_unhash(). Classification is
 *                           current state, which is the entire point of
 *                           G-LOCK-32.
 *
 * Both callers are in the same structural position -- they hold, or have just
 * held, the lock, and every node reachable from @succ is FROZEN: a queued
 * waiter cannot leave until somebody sets its ->locked, and only the holder
 * does that. So ->next reads are stable and cannot fault.
 *
 * Returns the node to promote. The caller promotes it exactly as it would have
 * promoted @succ -- no ->next pointer is rewritten here, because the evicted
 * nodes are simply not promoted and the walker is leaving the queue anyway.
 *
 * Every exit promotes the node currently under examination. There is no
 * backtracking, and that is load-bearing: because an eviction is a removal
 * rather than a reordering, the queue is well formed after every single
 * iteration, so committing incrementally is safe. The consequence worth
 * stating is that the node whose predecessor got evicted is ALWAYS the node
 * that gets promoted, so no waiter is ever left spinning behind an orphan.
 *
 * ONE rdtsc for the whole walk, and src/threshold/cap read once, matching
 * ivh_rot_probe_walk()'s discipline: a single timestamp is a consistent
 * snapshot of the queue rather than one smeared across up to HOP_CAP readings.
 */
static noinline struct mcs_spinlock *pv_evict_walk(struct mcs_spinlock *succ)
{
	unsigned long src = READ_ONCE(ivh_pv_preempt_src);
	u64 thr = READ_ONCE(ivh_pv_beat_threshold);
	u64 now = rdtsc();
	unsigned long cap = READ_ONCE(ivh_pv_requeue_max);
	unsigned long hopcap = READ_ONCE(ivh_pv_evict_hop_cap);
	struct mcs_spinlock *cand = succ;
	int hop, cls;

	if (cap > IVH_REQUEUE_MAX_CAP)
		cap = IVH_REQUEUE_MAX_CAP;
	/*
	 * Default 1. Letting one handoff evict up to IVH_ROT_HOP_CAP waiters is
	 * a storm amplifier under heavy steal, where MOST waiters read
	 * preempted: the queue is torn down and rebuilt, and every evicted
	 * waiter pays an xchg_tail on the single hottest cacheline in the
	 * system -- reintroducing exactly the contention the MCS queue exists
	 * to remove. One eviction per handoff still covers the common shape
	 * (successor preempted, the one behind it live) at bounded cost.
	 */
	if (hopcap > IVH_ROT_HOP_CAP)
		hopcap = IVH_ROT_HOP_CAP;

	this_cpu_inc(ivh_evict_walks);

	for (hop = 0; hop < (int)hopcap; hop++) {
		struct pv_node *pn = (struct pv_node *)cand;
		struct mcs_spinlock *after;
		u8 old = VCPU_RUNNING;

		/*
		 * LIVE  -- the node we came for.
		 * HALTED -- G-LOCK-31's fairness rule: a waiter that halted on
		 *           purpose is never skipped. Promote it; pv_kick_node()
		 *           (skip_point 0) or the F1 promote in
		 *           pv_deferred_handoff() (skip_point 1) wakes it.
		 * Either way: stop and promote @cand.
		 */
		cls = ivh_rot_class(cand, src, thr, now);
		if (cls != IVH_ROT_PREEMPTED) {
			/*
			 * Count only the HALTED stop: that is the fairness rule
			 * firing (we found a waiter that halted on purpose and
			 * refused to pass it). A LIVE stop is the ordinary and
			 * overwhelmingly common outcome -- the successor could
			 * use the lock -- and folding the two together would
			 * make the fairness rule look like it fires on nearly
			 * every handoff.
			 */
			if (cls == IVH_ROT_HALTED)
				this_cpu_inc(ivh_evict_stop_halted);
			goto out;
		}

		/*
		 * THE ONLY STRUCTURAL RULE LEFT: never evict a node whose
		 * ->next reads NULL.
		 *
		 * Under rotation this was a REFUSED opportunity -- the chosen
		 * node had to be moved to the front, and moving the tail would
		 * detach it from the lock word (or race an enqueuer that has
		 * done xchg_tail() but not yet stored into prev->next). Here it
		 * is not a refusal at all: a NULL ->next means there is nobody
		 * further along to promote instead, so evicting @cand could
		 * only leave the lock with no successor. Promote it.
		 *
		 * That is why eviction has no tail problem. It never writes the
		 * promoted node's ->next, so the promoted node is allowed to BE
		 * the tail -- which is what makes a two-waiter queue
		 * (holder -> cand -> after) actionable here and not under
		 * rotation.
		 */
		after = READ_ONCE(cand->next);
		if (!after) {
			this_cpu_inc(ivh_evict_tail_stop);
			goto out;
		}

		/*
		 * STARVATION BOUND. ->requeues counts evictions within this
		 * tenure and is cleared only by pv_init_node(), so a waiter
		 * reaches the front at most ivh_pv_requeue_max + 1 times before
		 * it is served regardless of how preempted it looks. cap == 0
		 * disables eviction entirely.
		 */
		if (READ_ONCE(pn->requeues) >= cap) {
			this_cpu_inc(ivh_evict_cap_refused);
			goto out;
		}

		/*
		 * THE COMMIT POINT, and the halt-race guard in one operation.
		 *
		 * Success: @cand was VCPU_RUNNING by its own account. A
		 * host-preempted vCPU still reads VCPU_RUNNING -- that is tier
		 * 2's whole premise -- so @cand is either spinning (and will
		 * see this on its next iteration) or off-CPU (and will see it
		 * the moment the host schedules it). No kick, no hypercall, no
		 * IPI: the eviction is free on this path, which is the one
		 * holding the lock.
		 *
		 * Failure: @cand reached VCPU_HALTED between the classification
		 * above and here, so by the fairness rule it must NOT be
		 * evicted. Promote it. VCPU_HASHED cannot occur -- only the
		 * queue head is ever hashed and the head is never reachable
		 * from a holder's ->next -- and VCPU_SKIPPED cannot occur
		 * because an already-evicted node is not in the queue.
		 *
		 * Full try_cmpxchg (not _relaxed): it must order the ->next and
		 * ->state reads above against the caller's subsequent promotion
		 * store, and it is taken at most once per evicted waiter.
		 */
		if (!try_cmpxchg(&pn->state, &old, VCPU_SKIPPED)) {
			this_cpu_inc(ivh_evict_halt_race);
			goto out;
		}

		this_cpu_inc(ivh_evict_marked);
		cand = after;
	}
	this_cpu_inc(ivh_evict_hop_cap);

out:
	if (cand != succ) {
		this_cpu_inc(ivh_evict_walks_acted);
		/*
		 * The promoted node is LIVE (or HALTED-and-about-to-be-woken)
		 * as of the decision, so its Phase 0b class is 0 -- not the
		 * evicted nodes' class. Depositing anything else here would
		 * label a healthy new head as stale and corrupt
		 * ivh_rot_idle_hist[]. Same store, same reason, as
		 * pv_handoff_rotate()'s "Store 5".
		 */
		WRITE_ONCE(((struct pv_node *)cand)->rot_flags, 0);
	}
	return cand;
}

/*
 * What the forward walk found, so pv_handoff_rotate() does not have to walk
 * the chain a second time to rediscover it.
 *
 * @live is the first non-stale node behind the immediate successor, and @prev
 * is the node immediately in front of it. @prev is exactly the node whose
 * ->next we read to reach @live, so @prev->next is KNOWN non-NULL -- that is
 * what makes it legal to overwrite (see rule (1) in <asm/ivh_tsc_beat.h>), and
 * it is why the walk hands back the predecessor rather than making the caller
 * re-derive it.
 */
struct ivh_rot_pick {
	struct mcs_spinlock	*prev;
	struct mcs_spinlock	*live;
};

/*
 * noinline, and every loop invariant hoisted: this runs between set_locked()
 * and the promotion store, i.e. inside the lock hold with preemption off.
 *
 * ONE rdtsc for the whole walk, not one per hop -- matching the discipline
 * is_wait_preempted() states for itself ("ONE rdtsc, not two"). A single
 * timestamp is also more correct: it yields a consistent snapshot of the
 * queue rather than one smeared across up to HOP_CAP readings. src and the
 * threshold are likewise read once, not re-loaded per hop.
 *
 * noinline keeps the PV slowpath's hot path down to the gate's own load +
 * test + not-taken branch, instead of inlining the whole walk into an already
 * very large function and perturbing its register allocation.
 *
 * Phase 1 changed the signature but NOT the counters: @probe carries
 * ivh_pv_rot_probe down so every pre-existing Phase 0 counter keeps its
 * original meaning of "counted iff the probe is on", even though the walk now
 * also runs when only ivh_pv_rot_enable is set. Mixing enable-only traffic
 * into ivh_rot_handoffs would silently change that denominator under every
 * Phase 0 measurement already taken.
 */
static noinline u8 ivh_rot_probe_walk(struct mcs_spinlock *next, bool probe,
				      struct ivh_rot_pick *pick)
{
	unsigned long src = READ_ONCE(ivh_pv_preempt_src);
	u64 thr = READ_ONCE(ivh_pv_beat_threshold);
	u64 now = rdtsc();
	struct mcs_spinlock *prev = next;
	struct mcs_spinlock *n = next;
	int hop, cls;

	pick->prev = NULL;
	pick->live = NULL;

	if (probe)
		this_cpu_inc(ivh_rot_handoffs);

	/*
	 * G-LOCK-31: a HALTED next-in-line is promoted normally (pv_kick_node()
	 * hashes it so the unlock wakes it), same as a live one.
	 */
	if (ivh_rot_class(n, src, thr, now) != IVH_ROT_PREEMPTED) {
		if (probe)
			this_cpu_inc(ivh_rot_depth_hist[0]);
		return 0;
	}
	if (probe)
		this_cpu_inc(ivh_rot_preempted);

	/*
	 * Forward scan, READ-ONLY, hard hop cap. Cannot fault: every ->next is
	 * NULL or a qnodes[] slot belonging to a CPU currently queued behind us,
	 * and qspinlock's MCS queue is strictly FIFO with no abort path, so no
	 * node behind us can be released before we store next->locked. The cap
	 * is defensive rather than strictly required (that same FIFO property
	 * rules out a cycle), but it is cheap and this code runs with the lock
	 * held -- an unbounded walk here would be a hard hang.
	 *
	 * Note hop < HOP_CAP, not <=: a live waiter at depth exactly HOP_CAP is
	 * reported in the "none found" bucket. Bucket HOP_CAP therefore means
	 * "no live node among the first HOP_CAP nodes", not "none within reach".
	 */
	for (hop = 1; hop < IVH_ROT_HOP_CAP; hop++) {
		struct mcs_spinlock *nn = READ_ONCE(n->next);

		if (!nn) {
			/*
			 * Reads as the tail. Counted separately so the analysis
			 * can distinguish "the queue was only this deep" from
			 * "HOP_CAP consecutive preempted waiters" -- completely
			 * different findings for Phase 1's economics, and the
			 * histogram alone cannot separate them.
			 *
			 * Not a reliable tail test: an enqueuer that has done
			 * xchg_tail() but not yet WRITE_ONCE(prev->next, node)
			 * leaves prev->next transiently NULL, so this over-counts
			 * genuine tails. Harmless -- and for Phase 1 the
			 * conservative "never splice a node whose next reads
			 * NULL" rule is correct regardless of the reason.
			 */
			if (probe)
				this_cpu_inc(ivh_rot_tail_stop);
			goto none_found;
		}
		/*
		 * @prev trails @n by one hop. We have just read nn out of
		 * n->next and found it non-NULL, so once we step forward
		 * prev->next is a pointer we are allowed to overwrite.
		 */
		prev = n;
		n = nn;
		cls = ivh_rot_class(n, src, thr, now);
		if (cls == IVH_ROT_HALTED) {
			/*
			 * G-LOCK-31, fairness: never skip past a waiter that
			 * halted on purpose. Stop the search; the caller
			 * promotes the original next-in-line.
			 */
			this_cpu_inc(ivh_rot_stop_halted);
			return IVH_ROT_F_STALE;
		}
		if (cls == IVH_ROT_LIVE) {
			if (probe)
				this_cpu_inc(ivh_rot_depth_hist[hop]);
			/*
			 * Stale target AND a live node behind it: the only
			 * shape Phase 1 could actually act on.
			 */
			pick->prev = prev;
			pick->live = n;
			return IVH_ROT_F_STALE | IVH_ROT_F_SKIPPABLE;
		}
	}

none_found:
	if (probe) {
		this_cpu_inc(ivh_rot_no_live);
		this_cpu_inc(ivh_rot_depth_hist[IVH_ROT_HOP_CAP]);
	}
	return IVH_ROT_F_STALE;
}

static __always_inline void pv_handoff_rotate(struct qspinlock *lock,
					      struct mcs_spinlock *node,
					      struct mcs_spinlock **nextp)
{
	unsigned long probe = READ_ONCE(ivh_pv_rot_probe);
	unsigned long enable = READ_ONCE(ivh_pv_rot_enable);
	struct mcs_spinlock *succ, *after;
	struct ivh_rot_pick pick;
	unsigned long cap;
	unsigned int skips;
	u8 flags, sf;

	/* lock/node unused: rotation needs neither, by design. */
	(void)lock;
	(void)node;

	/*
	 * G-LOCK-33, PROMOTION POINT 0 (pre-CS). Eviction takes precedence over
	 * rotation: they are two answers to the same question and running both
	 * would splice a queue we are also removing nodes from. Checked before
	 * the probe|enable gate so eviction is reachable with rotation entirely
	 * off, which is how the A/B is run -- one kernel, three modes
	 * (off / rotate / evict), selected live.
	 */
	if (unlikely(READ_ONCE(ivh_pv_evict_enable))) {
		succ = *nextp;
		if (succ)
			*nextp = pv_evict_walk(succ);
		return;
	}

	/*
	 * Both knobs off == upstream, to the instruction: one load of each
	 * read-mostly global, an or, and a not-taken branch.
	 */
	if (likely(!(probe | enable)))
		return;

	/*
	 * *nextp is provably non-NULL here: the call site spins on
	 * smp_cond_load_relaxed(&node->next, (VAL)) until it is. Defensive
	 * only -- but it is also what lets everything below dereference @succ
	 * unconditionally.
	 */
	succ = *nextp;
	if (!succ)
		return;

	flags = ivh_rot_probe_walk(succ, probe, &pick);

	if (!pick.live)
		goto promote_succ;

	/*
	 * THE SAFETY GATE. We are about to write pick.prev->next and
	 * pick.live->next. pick.prev->next is already known non-NULL (the walk
	 * read pick.live out of it). pick.live->next has not been looked at
	 * yet, so read it here, once, and refuse the whole rotation if it is
	 * NULL.
	 *
	 * A NULL here means pick.live is the queue tail, or that an enqueuer
	 * has already claimed pick.live's tail code via xchg_tail() and is
	 * about to store itself into pick.live->next (qspinlock.c:380). In the
	 * first case splicing would detach the tail from the lock word; in the
	 * second our store and the enqueuer's would race, and whichever lost
	 * would leave a node in the queue that no predecessor will ever set
	 * ->locked on -- a permanently stuck queue. There is no way to
	 * distinguish the two cases and no need to: refuse both.
	 *
	 * Conversely, non-NULL is conclusive. Exactly one waiter ever obtains a
	 * given tail code, so a non-NULL ->next has already taken its one and
	 * only in-queue write and has no second writer pending; and because
	 * neither spliced node is the tail, and the tail code only ever moves
	 * on to later arrivals, no future enqueue can target them either.
	 */
	after = READ_ONCE(pick.live->next);
	if (!after) {
		this_cpu_inc(ivh_rot_splice_blocked_tail);
		goto promote_succ;
	}
	this_cpu_inc(ivh_rot_splice_ok);

	if (!enable)
		goto promote_succ;

	/*
	 * Starvation bound. bits 2-7 of the successor's rot_flags count how
	 * many times it has already been rotated past during THIS tenure
	 * (pv_init_node() zeroes rot_flags on every queue entry). At the cap we
	 * promote it regardless, which is what makes the whole scheme
	 * starvation-free -- see <asm/ivh_tsc_beat.h> for the bound.
	 *
	 * Clamped rather than trusted: the sysctl is an unbounded unsigned
	 * long, and a value above IVH_ROT_SKIP_MAX would let skips + 1 overflow
	 * bit 7 and corrupt the IVH_ROT_F_* class bits in bits 0-1.
	 */
	cap = READ_ONCE(ivh_pv_rot_skip_max);
	if (cap > IVH_ROT_SKIP_MAX)
		cap = IVH_ROT_SKIP_MAX;

	sf = READ_ONCE(((struct pv_node *)succ)->rot_flags);
	skips = sf >> IVH_ROT_SKIP_SHIFT;
	if (skips >= cap) {
		this_cpu_inc(ivh_rot_splice_blocked_starve);
		goto promote_succ;
	}

	/*
	 * ------------------------------------------------------------------
	 * THE SPLICE.  us -> succ -> ... -> prev -> live -> after -> ... -> T
	 *         becomes  us -> live -> succ -> ... -> prev -> after -> T
	 * ------------------------------------------------------------------
	 *
	 * Store 1, pick.prev->next = after: pick.prev->next was read as
	 * pick.live (non-NULL) inside the walk, so by rule (1) it has no
	 * pending second writer and no future enqueue can target it. pick.prev
	 * is frozen -- it is queued behind us and cannot leave its tenure until
	 * we set its ->locked, which only its own (new) predecessor ever does.
	 * In the depth-1 case pick.prev == succ and this is exactly
	 * "A->next = C".
	 *
	 * Store 2, pick.live->next = succ: pick.live->next was just read as
	 * @after, non-NULL, so the same argument applies verbatim. pick.live is
	 * likewise frozen.
	 *
	 * Neither store touches a node whose ->next is NULL, neither touches
	 * the tail, and neither touches the lock word's tail field. The queue
	 * remains one simple acyclic list ending at the same tail node T, so
	 * the next xchg_tail() enqueue links onto T exactly as before.
	 *
	 * No barrier between or after these two stores is needed or added: the
	 * caller's very next statement is
	 * arch_mcs_spin_unlock_contended(&next->locked), an smp_store_release,
	 * which orders both of them before any waiter can observe ->locked == 1
	 * through the matching smp_cond_load_acquire. The release/acquire chain
	 * carries them transitively onward -- @succ sees its rewritten ->next
	 * when pick.live later releases it.
	 */
	WRITE_ONCE(pick.prev->next, after);
	WRITE_ONCE(pick.live->next, succ);

	/*
	 * Store 3, the skipped successor's flags. Same freeze argument: @succ
	 * cannot end its tenure before we set its ->locked, and nothing else
	 * writes rot_flags -- pv_init_node() only runs at the start of a
	 * tenure, and only the holder of THIS lock ever runs pv_handoff_rotate()
	 * against a node queued on THIS lock. rot_flags is a distinct byte from
	 * ->state, which @succ's own CPU may be storing to concurrently; byte
	 * stores do not tear into each other on x86, and this header is x86-only
	 * (see the <asm/ivh_tsc_beat.h> include at the top).
	 *
	 * Bumping the skip count here and nowhere else is what bounds
	 * starvation; the class bits record that @succ was found stale and
	 * skippable, and are overwritten with fresh ones the moment @succ is
	 * finally promoted.
	 */
	WRITE_ONCE(((struct pv_node *)succ)->rot_flags,
		   (u8)(IVH_ROT_F_STALE | IVH_ROT_F_SKIPPABLE |
			((skips + 1) << IVH_ROT_SKIP_SHIFT)));

	/*
	 * Store 4, the promotion target. @nextp is the caller's own on-stack
	 * `next`, not shared state; the caller reads it for both
	 * arch_mcs_spin_unlock_contended() and pv_kick_node(), so this single
	 * store redirects the MCS baton AND the PV kick/hash bookkeeping to
	 * pick.live together. They must not diverge: hashing one node and
	 * releasing another would put a node into pv_hash() that nobody will
	 * ever unhash.
	 */
	*nextp = pick.live;
	this_cpu_inc(ivh_rot_splice_done);

	/*
	 * Store 5, the promoted node's class. pick.live was LIVE at the moment
	 * the decision was taken, so its Phase 0b class is 0 -- NOT the walk's
	 * @flags, which describe @succ. Depositing @flags here would label a
	 * healthy new head as stale and corrupt ivh_rot_idle_hist[]. This also
	 * resets pick.live's own skip counter, which is correct: it is being
	 * promoted.
	 */
	WRITE_ONCE(((struct pv_node *)pick.live)->rot_flags, 0);
	return;

promote_succ:
	/*
	 * No rotation. @succ is promoted, so deposit the walk's verdict about
	 * @succ in @succ -- unchanged Phase 0b behaviour, and it zeroes @succ's
	 * skip counter, which is the "reset on promotion" half of the
	 * starvation bound. Free: arch_mcs_spin_unlock_contended() is about to
	 * store ->locked in this same cacheline and pv_kick_node() RMWs ->state
	 * in it immediately after, so the line is taken exclusive here either
	 * way.
	 */
	WRITE_ONCE(((struct pv_node *)succ)->rot_flags, flags);
}

/*
 * Phase 0b -- LOCK IDLE TIME: how long @lock sits released-but-unclaimed
 * because the waiter it was handed to is not running.
 *
 * Stamps the slot of the TARGET (@node, the waiter being released to), not of
 * the releasing CPU. Keying by releaser would prove only "the last hashed
 * release that CPU performed was on a lock at this address", which is a
 * different claim: a lock is released many times, and every release taking
 * the asm fast path leaves the previous stamp standing, so a match can span
 * an unbounded number of intervening tenures. Keying by target makes the
 * pairing unambiguous by construction -- __pv_queued_spin_unlock_slowpath()
 * has already done pv_unhash(lock) and so holds the exact pv_node it is about
 * to wake -- and lets the reader consume its OWN local slot, with no remote
 * load on the acquisition path at all.
 *
 * COVERAGE LIMIT, stated rather than papered over: on x86-64
 * __pv_queued_spin_unlock() is hand-written assembly (PV_UNLOCK_ASM in
 * <asm/qspinlock_paravirt.h>), so only the hashed _Q_SLOW_VAL release path is
 * reachable from C at all. A release is hashed only when pv_kick_node()'s
 * cmpxchg(&pn->state, VCPU_HALTED, VCPU_HASHED) succeeded -- that is, only
 * when the successor had ALREADY halted. Every sample here is therefore a
 * halted head. A head the host descheduled mid-spin never halts, is never
 * hashed, and is invisible to this measurement. See the ivh_rot_idle_hist[]
 * note in <asm/ivh_tsc_beat.h> for what this does and does not license.
 */
static __always_inline void ivh_rot_stamp_release(struct qspinlock *lock,
						  struct pv_node *node)
{
	struct ivh_rot_rel *r;

	if (likely(!READ_ONCE(ivh_pv_rot_probe)))
		return;

	r = &per_cpu(ivh_rot_rel, node->cpu);
	r->tsc = rdtsc();
	/*
	 * ->tsc must be visible before ->lock. ->lock is the validity flag the
	 * target tests, and the pair is read on a different CPU; without this
	 * the reader can pair a matching ->lock with a stale ->tsc and report a
	 * wildly wrong -- or negative -- interval.
	 */
	smp_wmb();
	WRITE_ONCE(r->lock, lock);
}

/*
 * Called by a queue head the instant it has observed @lock free and is about
 * to claim it. @prev is the node that handed us the MCS baton, or NULL if we
 * were the first node queued and so have nothing to attribute an interval to.
 *
 * idle = now - (moment our predecessor released the lock), bucketed by class
 * so that class 0 -- our promoter did NOT consider us stale -- is the
 * baseline. An absolute idle time means nothing on its own; only the excess
 * of the stale classes over class 0 is the time rotation could recover.
 */
static noinline void ivh_rot_ack_slow(struct qspinlock *lock,
				      struct mcs_spinlock *node)
{
	/*
	 * FIRST statement, before any load: this runs ahead of set_locked(), so
	 * with the probe on the lock really is still free for the whole of this
	 * function, and anything charged after a cold miss here lands in the
	 * interval being reported.
	 */
	u64 now = rdtsc();
	struct pv_node *pn = (struct pv_node *)node;
	struct ivh_rot_rel *r = this_cpu_ptr(&ivh_rot_rel);
	u64 idle, cap;
	u8 cls;
	int bucket;

	if (READ_ONCE(r->lock) != lock) {
		this_cpu_inc(ivh_rot_idle_unknown);
		return;
	}
	smp_rmb();			/* pairs with smp_wmb() in stamp_release */
	idle = now - r->tsc;

	/*
	 * Single-use. Leaving the stamp set would let a later acquisition of
	 * the same lock match a long-dead release and report an interval
	 * spanning every tenure in between.
	 */
	WRITE_ONCE(r->lock, NULL);

	cls = READ_ONCE(pn->rot_flags) & (IVH_ROT_F_STALE | IVH_ROT_F_SKIPPABLE);

	if ((s64)idle < 0) {
		/*
		 * Backwards, and this is real rather than hypothetical: the
		 * stamp is written just after smp_store_release(&lock->locked,
		 * 0), and a head woken by something other than that kick can
		 * acquire inside the window. Counted, never silently dropped --
		 * the discards are preferentially the SHORT intervals, so
		 * dropping them quietly would shift every mean upward.
		 */
		this_cpu_inc(ivh_rot_idle_backward);
		return;
	}

	/*
	 * A qspinlock lives in memory that can be freed and reallocated --
	 * __pv_queued_spin_unlock_slowpath() says so itself. A new lock at a
	 * recycled address can match a surviving stamp and yield an interval of
	 * milliseconds or seconds. ivh_rot_idle_cycles[] is a SUM, so a single
	 * such artifact swamps a million genuine samples: cap it rather than
	 * trust it, and count what was capped.
	 */
	cap = READ_ONCE(ivh_pv_beat_threshold) * 100;
	if (cap && idle > cap) {
		this_cpu_inc(ivh_rot_idle_capped);
		return;
	}

	bucket = idle ? ilog2(idle) : 0;
	if (bucket >= IVH_BEAT_AGE_HIST_BUCKETS)
		bucket = IVH_BEAT_AGE_HIST_BUCKETS - 1;

	this_cpu_inc(ivh_rot_idle_hist[cls][bucket]);
	this_cpu_add(ivh_rot_idle_cycles[cls], idle);
	this_cpu_inc(ivh_rot_idle_events[cls]);
}

/*
 * Gate only. The body above is deliberately noinline: it is several loads, a
 * remote per-CPU read and an ilog2, and inlining all of that into
 * queued_spin_lock_slowpath() would grow the PV slowpath's I-cache footprint
 * for every acquisition even when the probe is off. With the probe off this
 * costs one predictable load and a not-taken branch.
 */
static __always_inline void ivh_rot_ack(struct qspinlock *lock,
					struct mcs_spinlock *node)
{
	if (likely(!READ_ONCE(ivh_pv_rot_probe)))
		return;

	ivh_rot_ack_slow(lock, node);
}

/*
 * Phase 0b hook, mirroring pv_handoff_rotate()'s shape so the two stay
 * symmetric: rotate() runs at the promotion end of a handoff, ack() at the
 * acquisition end of the same handoff, one queue position later.
 */
static __always_inline void pv_handoff_ack(struct qspinlock *lock,
					   struct mcs_spinlock *node)
{
	ivh_rot_ack(lock, node);
}

/*
 * Wait for l->locked to become clear and acquire the lock;
 * halt the vcpu after a short spin.
 * __pv_queued_spin_unlock() will wake us.
 *
 * The current value of the lock will be returned for additional processing.
 */
static u32
pv_wait_head_or_lock(struct qspinlock *lock, struct mcs_spinlock *node,
		     struct mcs_spinlock *prev)
{
	struct pv_node *pn = (struct pv_node *)node;
	struct pv_node *pp = (struct pv_node *)prev;	/* may be NULL */
	struct qspinlock **lp = NULL;
	int waitcnt = 0;
	unsigned long loop;
	unsigned long threshold;
	/*
	 * ALL episode state is in locals. struct pv_node stays exactly 32
	 * bytes with head_ctl at offset 24 and rot_flags in the 3-byte hole at
	 * 21; nothing here touches it. That is not incidental -- the head is a
	 * single thread running a single loop, so its detection state has no
	 * reason to be visible to anyone else.
	 */
	u64 ep_acq = 0, ep_start = 0, tenure_start = 0;
	bool ep_any = false, probe, entered_hashed = false;
#define IVH_HEAD_FOREIGN_MAX 4
	u8 cs_gate = IVH_CS_GATE_OK;
	/*
	 * Stage B: `bail` is the only thing that turns a fired detection into
	 * a control-flow change, read once per tenure alongside `probe`; `cause`
	 * says which exit led to the head halt below, for the per-cause halt
	 * accounting. Both are re-initialised at the top of every tenure.
	 */
	bool bail = false;
	int cause = IVH_CS_HALT_EXHAUST;
	u64 halt_tsc;

	/*
	 * If pv_kick_node() already advanced our state, we don't need to
	 * insert ourselves into the hash table anymore.
	 */
	if (READ_ONCE(pn->state) == VCPU_HASHED)
	{
		lp = (struct qspinlock **)1;
		/* sec 1.2: was halted at handoff; prev may already have
		 * released before we could set pending. */
		entered_hashed = true;
	}

	/*
	 * Tracking # of slowpath locking operations
	 */
	lockevent_inc(lock_slowpath);

	for (;; waitcnt++) {
		/*
		 * Per-tenure, NOT per-function. Declared outside this loop it
		 * accumulated across the head's entire stay, so four separate
		 * tenures that each saw one foreign entry -- and each resolved
		 * correctly -- would still trip the forced-duplicate path. The
		 * bound is meant to be "four consecutive observations of the
		 * SAME stuck entry".
		 */
		unsigned int foreign_rounds = 0;
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
		 * Read the gate ONCE per tenure, not per iteration -- the same
		 * rule the G-LOCK-21-spin comments impose on
		 * ivh_pv_spin_threshold two lines below, and for the same
		 * reason: a live sysctl flip must not take effect mid-spin, or
		 * the per-tenure accounting stops being internally consistent.
		 */
		probe = READ_ONCE(ivh_cs_head_probe);
		bail = probe && READ_ONCE(ivh_cs_head_bail) &&
		       READ_ONCE(ivh_adaptive_mode) == IVH_MODE_ADAPTIVE;
		cause = IVH_CS_HALT_EXHAUST;
		ep_acq = 0;
		ep_any = false;
		if (probe)
			tenure_start = rdtsc();

		/*
		 * Set the pending bit in the active lock spinning loop to
		 * disable lock stealing before attempting to acquire the lock.
		 */
		set_pending(lock);
		/*
		 * Soundness gate, once per tenure, BEFORE the first sampled
		 * check: commits the pending store and decides whether `prev`
		 * is provably still the holder (build plan sec 1.2). Behaviour-
		 * neutral: no control flow depends on cs_gate outside the probe.
		 */
		if (unlikely(probe))
			cs_gate = ivh_cs_tenure_gate(lock, pp, waitcnt,
						     entered_hashed);
		/* G-LOCK-21-spin: read once per attempt, see pv_wait_node(). */
		threshold = READ_ONCE(ivh_pv_spin_threshold);
		for (loop = threshold; loop; loop--) {
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
			/*
			 * IVH head adaptive check. With ivh_cs_head_bail == 0
			 * (STAGE A, the default) this is DETECT-ONLY: `bail`
			 * is false for the whole tenure, so the one break
			 * below is unreachable, nothing else here stores
			 * outside this_cpu counters and stack locals, and the
			 * loop's trip count is bit-identical to before. That is
			 * the behaviour-neutrality proof, and it is checkable:
			 * ivh_head_spin_iters_sum / ivh_head_spin_attempts must
			 * still equal ivh_pv_spin_threshold exactly.
			 *
			 * STAGE B (ivh_cs_head_bail == 1): a fired detection
			 * breaks out into the existing, already-audited
			 * clear_pending() -> pv_hash() -> xchg(_Q_SLOW_VAL) ->
			 * pv_wait() sequence, exactly as exhaustion does. No
			 * new mechanism, no new state, no new failure mode.
			 *
			 * Sampled on PV_PREV_CHECK_MASK, the same cadence as
			 * pv_wait_early()'s tier 2, for the same cacheline
			 * reason -- every evaluation pulls a remote line
			 * (prev's ivh_cs_owner, then prev's ivh_tsc_beat).
			 *
			 * At the default ivh_cs_head_probe == 0 this is one
			 * already-loaded register test and one predicted
			 * not-taken branch.
			 */
			if (unlikely(probe) &&
			    (loop & PV_PREV_CHECK_MASK) == 0) {
				bool hit = ivh_cs_head_probe_one(lock, pp,
						cs_gate, &ep_acq, &ep_start,
						&ep_any);

				if (hit && bail) {
					/*
					 * G-LOCK-31: already hashed but SLOW_VAL is
					 * gone means no unlock will kick us; halting
					 * would return at once and churn. Keep spinning.
					 */
					if (lp && READ_ONCE(lock->locked) != _Q_SLOW_VAL) {
						this_cpu_inc(ivh_cs_bail_suppressed);
					} else {
						this_cpu_inc(ivh_cs_head_bailed);
						cause = IVH_CS_HALT_CS;
						break;
					}
				}
			}
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
		/*
		 * Stage B introduces the first early exit this loop has ever
		 * had, which falsifies the "loop == 0 here" invariant this
		 * block was built on. Split rather than blended: the
		 * exhaustion accumulators keep meaning exactly what they meant
		 * (and keep averaging exactly SPIN_THRESHOLD, which is still
		 * the accounting sanity check), and the bail population gets
		 * its own pair. Behaviour-identical when ivh_cs_head_bail == 0,
		 * because loop is then always 0 here.
		 */
		if (loop) {
			this_cpu_add(ivh_head_spin_iters_bail_sum, threshold - loop);
			this_cpu_inc(ivh_head_spin_bail_attempts);
		} else {
			this_cpu_add(ivh_head_spin_iters_sum, threshold - loop);
			this_cpu_inc(ivh_head_spin_attempts);
		}
		if (unlikely(probe)) {
			u64 now = rdtsc();

			ivh_cs_ep_close(&ep_acq, ep_start, now, IVH_CS_EP_EXHAUST);
			ivh_cs_tenure_record(tenure_start, now, ep_any);
		}

		/*
		 * G-LOCK-33 LEAK FIX. The guard above ("am I VCPU_HASHED?") asks
		 * whether pv_kick_node() hashed THIS node. Under stock PV that is
		 * sufficient, because the node pv_kick_node() hashes for is always
		 * the node that becomes the head -- entry owner and head are the
		 * same node by construction.
		 *
		 * EVICTION BREAKS THAT IDENTITY. The holder promotes `after` and
		 * pv_kick_node() hashes for it, while the node we EVICTED requeues
		 * and can reach the head role on the same lock. The head is then A
		 * while the outstanding entry belongs to B; A's own state reads
		 * VCPU_RUNNING (it was never kicked), the guard passes, and A
		 * hashes too. Two entries for one lock; pv_unhash() frees one and
		 * the other leaks forever. Measured: ivh_hash_dup_head 126 vs
		 * ivh_hash_dup_kick 3, and the live gauge never returns to zero.
		 *
		 * So ask about the LOCK, not about ourselves. And when the lock is
		 * already hashed by somebody else we must ALSO NOT HALT: the entry
		 * names B, so the unlock would pv_unhash() B and kick B, and
		 * nothing would ever wake us. Spin another round instead. The
		 * foreign entry is released by the very next slowpath unlock (it
		 * is what keeps lock->locked at _Q_SLOW_VAL), so this resolves
		 * immediately and cannot loop indefinitely.
		 *
		 * Rare by construction: 129 events against 31,428 evictions.
		 */
		if (!lp && unlikely(pv_lock_is_hashed(lock))) {
			this_cpu_inc(ivh_head_foreign_hash);
			/*
			 * BOUNDED. The reasoning above assumes the foreign entry
			 * is LIVE -- that it is what holds lock->locked at
			 * _Q_SLOW_VAL, so the next slowpath unlock releases it.
			 * That is true of an entry created by pv_kick_node() or by
			 * another head. It is NOT true of an ORPHAN: a previously
			 * leaked entry whose lock is no longer _Q_SLOW_VAL. No
			 * unlock will ever unhash an orphan, so spinning on one
			 * would spin forever.
			 *
			 * We cannot reliably tell the two apart from here, so bound
			 * the wait instead of asserting. After a few rounds, fall
			 * through and hash anyway: that risks one duplicate entry,
			 * which costs a leaked slot, whereas spinning forever costs
			 * a wedged vCPU. Liveness beats tidiness.
			 */
			if (++foreign_rounds < IVH_HEAD_FOREIGN_MAX)
				continue;
			this_cpu_inc(ivh_head_foreign_forced);
		}

		if (!lp) { /* ONCE */
			this_cpu_inc(ivh_hash_ins_head);
			lp = pv_hash(lock, pn, 1);	/* site 1 = head */

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
				this_cpu_inc(ivh_hash_rel_lp);
				/*
				 * G-LOCK-33 gauge: this clears the entry WITHOUT
				 * going through pv_unhash(), so it must decrement
				 * here too. Missing this made the gauge climb
				 * monotonically during entirely healthy operation.
				 */
				atomic_dec(&ivh_pv_hash_live);
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
		/*
		 * Stage B: measure the halt, do not assume it helped. Bracketed
		 * exactly as pv_wait_node() brackets its own pv_wait(), and
		 * recorded by cause, so a CS-caused halt that is systematically
		 * LONGER than an exhaustion halt (the head slept past the
		 * release; the wake path is the problem, not the detector) is
		 * visible. Records only; nothing reads these back.
		 */
		halt_tsc = ivh_raw_tsc();
		pv_wait(&lock->locked, _Q_SLOW_VAL);
		{
			u64 d = ivh_raw_tsc() - halt_tsc;

			this_cpu_add(ivh_head_halt_cycles[cause], d);
			this_cpu_inc(ivh_head_halt_events[cause]);
			this_cpu_inc(ivh_head_halt_hist[cause][ivh_cs_bucket(d)]);
		}
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
	if (unlikely(probe)) {
		u64 now = rdtsc();

		/*
		 * The predecessor's hold has just ended -- we are taking the
		 * lock it released. If its stamp is still readable, this is a
		 * free, population-correct sample of how long a CONTENDED hold
		 * actually lasts on this workload, which is exactly the
		 * distribution the false-positive audit needs and the only one
		 * this predicate ever judges. Costs nothing on the release path.
		 */
		/*
		 * Only when the tenure gate PASSED and the clear is off. A
		 * failed gate means prev may have released long ago, so
		 * now - a would span a stealer's hold too. With the clear on,
		 * the tag is already NULL here and the sample is taken
		 * holder-side in __ivh_cs_owner_clear() instead.
		 */
		if (pp && cs_gate == IVH_CS_GATE_OK &&
		    !READ_ONCE(ivh_cs_owner_clear) &&
		    !READ_ONCE(ivh_pv_rot_enable)) {
			struct ivh_cs_owner *o = &per_cpu(ivh_cs_owner, pp->cpu);
			u64 a = READ_ONCE(o->tsc);
			s64 h = (s64)(now - a);

			if (READ_ONCE(o->lock) == (void *)lock && h > 0)
				this_cpu_inc(ivh_cs_prev_hold_hist[ivh_cs_bucket(h)]);
		}
		ivh_cs_ep_close(&ep_acq, ep_start, now, IVH_CS_EP_ACQUIRED);
		ivh_cs_tenure_record(tenure_start, now, ep_any);
	}
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
 * G-LOCK-32: defer the choice of the next waiter to unlock time.
 *
 * Instead of promoting @next now (before our critical section), record it in the
 * PV hash so our unlock can find it, mark the lock _Q_SLOW_VAL so the unlock
 * takes the slow path, and set the pending bit as a reservation so arrivals
 * enqueue instead of camping in pv_hybrid_queued_unfair_trylock(). @next cannot
 * leave the queue while we hold the lock, so the recorded pointer stays valid
 * for the whole CS -- that is what lets us give our qnode back as usual.
 *
 * Returns true when the promotion was deferred; the caller then does NOT
 * promote or kick.
 */
static bool pv_defer_promote(struct qspinlock *lock, struct mcs_spinlock *node,
			     struct mcs_spinlock *next)
{
	struct pv_node *pn = (struct pv_node *)next;

	if (likely(!READ_ONCE(ivh_pv_skip_point)))
		return false;
	if (unlikely(!next)) {
		this_cpu_inc(ivh_defer_no_successor);
		return false;
	}

	/*
	 * Hash before the _Q_SLOW_VAL store, the same ordering pv_kick_node() and
	 * the head path use, so an unlock that observes SLOW_VAL is guaranteed to
	 * find the entry. Review F4: the table cannot fill -- every entry is
	 * charged to a distinct queued qnode, and a lock never has both a
	 * deferred entry and a halted-head entry.
	 */
	(void)pv_hash(lock, pn, 2);
	WRITE_ONCE(lock->locked, _Q_SLOW_VAL);
	set_pending(lock);

	/*
	 * Reserve policy 0: if the first waiter is already halted there is no
	 * spinner for the reservation to protect, so drop it immediately and let
	 * arrivals steal, which is what stock PV does once a head halts.
	 */
	if (READ_ONCE(ivh_pv_unlock_reserve) == 0) {
		smp_mb();
		if (READ_ONCE(pn->state) != VCPU_RUNNING)
			clear_pending(lock);
	}

	((struct pv_node *)next)->deferred = 1;
	this_cpu_inc(ivh_defer_acquires);
	return true;
}

/*
 * G-LOCK-32: the unlock-side half. @first is the waiter the holder deferred.
 * Walk forward skipping PREEMPTED waiters (VCPU_RUNNING but silent), stop at a
 * HALTED one (fairness: never skip a waiter that halted on purpose), splice the
 * chosen one to the front, then release and promote.
 */
static void pv_deferred_handoff(struct qspinlock *lock, struct pv_node *first)
{
	struct mcs_spinlock *pick = &first->mcs;
	struct pv_node *pp;
	u8 old = VCPU_HALTED;

	this_cpu_inc(ivh_defer_handoffs);

	/*
	 * G-LOCK-33, PROMOTION POINT 1 (unlock). Same walk, same rules, same
	 * function as the pre-CS path -- only the timing of the classification
	 * differs, which is the entire reason skip_point=1 exists. Takes
	 * precedence over the splice below for the same reason as in
	 * pv_handoff_rotate().
	 */
	if (unlikely(READ_ONCE(ivh_pv_evict_enable)))
		pick = pv_evict_walk(pick);
	else if (READ_ONCE(ivh_pv_rot_enable)) {
		unsigned long src = READ_ONCE(ivh_pv_preempt_src);
		u64 thr = READ_ONCE(ivh_pv_beat_threshold);
		u64 now = rdtsc();
		struct mcs_spinlock *prev = NULL, *n = pick, *nn, *after;
		bool found = false;
		int hop, cls;

		for (hop = 0; hop < IVH_ROT_HOP_CAP; hop++) {
			cls = ivh_rot_class(n, src, thr, now);
			if (cls == IVH_ROT_LIVE) {
				found = true;
				break;
			}
			if (cls == IVH_ROT_HALTED) {
				if (hop)
					this_cpu_inc(ivh_defer_stop_halted);
				break;
			}
			nn = READ_ONCE(n->next);	/* PREEMPTED: try to move past */
			if (!nn)
				break;		/* tail: nothing to skip to */
			prev = n;
			n = nn;
		}
		if (found && prev) {
			after = READ_ONCE(n->next);
			if (after) {		/* never splice through a NULL ->next */
				WRITE_ONCE(prev->next, after);
				WRITE_ONCE(n->next, pick);
				pick = n;
				this_cpu_inc(ivh_defer_skips);
			}
		}
	}

	/* Review F2: pending -> release -> promote, in that order. */
	if (READ_ONCE(ivh_pv_unlock_reserve) != 2)
		clear_pending(lock);
	smp_store_release(&lock->locked, 0);

	/*
	 * Review F1 (mandatory): a halted node waiter sleeps on
	 * pv_wait(&pn->state, VCPU_HALTED). Only a STATE CHANGE ends that wait --
	 * the kick alone does not latch in the no-PV-unhalt spin path, the IRQ-off
	 * spin path, or IPI mode. Set ->locked first so a waiter that is still
	 * spinning simply observes it, then flip HALTED->RUNNING and kick only if
	 * the flip won. Never pv_hash()/pv_kick_node() the pick here: that would
	 * leave an orphan entry and hang a later deferral.
	 */
	WRITE_ONCE(pick->locked, 1);
	smp_mb__before_atomic();
	pp = (struct pv_node *)pick;
	if (try_cmpxchg_relaxed(&pp->state, &old, VCPU_RUNNING)) {
		this_cpu_inc(ivh_defer_kicks);
		pv_kick(pp->cpu);
	}
}

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
	 * G-LOCK-32: dispatch on the NODE, not on ivh_pv_skip_point, so a lock
	 * acquired in either mode always releases correctly even if the knob is
	 * flipped mid-flight. A promoted head always has mcs.locked == 1; a
	 * deferred first waiter has never been promoted, so it reads 0.
	 */
	/*
	 * Dispatch on an EXPLICIT marker, not on mcs.locked == 0. See the
	 * ->deferred comment in struct pv_node: unpromoted heads also have
	 * locked == 0, and routing them here fed the head itself to
	 * pv_evict_walk() as candidate 0.
	 */
	if (READ_ONCE(((struct pv_node *)node)->deferred)) {
		node->deferred = 0;
		pv_deferred_handoff(lock, node);
		return;
	}

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
	ivh_rot_stamp_release(lock, node);

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
	 * No Phase 0b stamp here, and none is possible: nothing was hashed, so
	 * there is no target node to key one to. This path is also dead on
	 * x86-64, where <asm/qspinlock_paravirt.h> defines
	 * __pv_queued_spin_unlock and the whole function is #ifndef'd out in
	 * favour of PV_UNLOCK_ASM.
	 */
	if (try_cmpxchg_release(&lock->locked, &locked, 0))
		return;

	__pv_queued_spin_unlock_slowpath(lock, locked);
}
#endif /* __pv_queued_spin_unlock */
