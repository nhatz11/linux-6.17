/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Queued spinlock
 *
 * A 'generic' spinlock implementation that is based on MCS locks. For an
 * architecture that's looking for a 'generic' spinlock, please first consider
 * ticket-lock.h and only come looking here when you've considered all the
 * constraints below and can show your hardware does actually perform better
 * with qspinlock.
 *
 * qspinlock relies on atomic_*_release()/atomic_*_acquire() to be RCsc (or no
 * weaker than RCtso if you're power), where regular code only expects atomic_t
 * to be RCpc.
 *
 * qspinlock relies on a far greater (compared to asm-generic/spinlock.h) set
 * of atomic operations to behave well together, please audit them carefully to
 * ensure they all have forward progress. Many atomic operations may default to
 * cmpxchg() loops which will not have good forward progress properties on
 * LL/SC architectures.
 *
 * One notable example is atomic_fetch_or_acquire(), which x86 cannot (cheaply)
 * do. Carefully read the patches that introduced
 * queued_fetch_set_pending_acquire().
 *
 * qspinlock also heavily relies on mixed size atomic operations, in specific
 * it requires architectures to have xchg16; something which many LL/SC
 * architectures need to implement as a 32bit and+or in order to satisfy the
 * forward progress guarantees mentioned above.
 *
 * Further reading on mixed size atomics that might be relevant:
 *
 *   http://www.cl.cam.ac.uk/~pes20/popl17/mixed-size.pdf
 *
 * (C) Copyright 2013-2015 Hewlett-Packard Development Company, L.P.
 * (C) Copyright 2015 Hewlett-Packard Enterprise Development LP
 *
 * Authors: Waiman Long <waiman.long@hpe.com>
 */
#ifndef __ASM_GENERIC_QSPINLOCK_H
#define __ASM_GENERIC_QSPINLOCK_H

#include <asm-generic/qspinlock_types.h>
#include <linux/atomic.h>
/*
 * IVH lock-holder identity.  Deliberately a standalone header whose entire
 * dependency set is <linux/compiler.h> + <linux/types.h>, and deliberately
 * NOT <asm/ivh_tsc_beat.h> where the rest of the IVH lock instrumentation
 * lives -- see the include-order writeup in <linux/ivh_lock_holder.h>.  Every
 * ivh_lock_*_holder() below compiles to nothing at all off x86/KVM/PV, and to
 * a single predicted branch on a read-mostly global when
 * ivh_lock_holder_enabled is 0, which is the default.
 */
#include <linux/ivh_lock_holder.h>

#ifndef queued_spin_is_locked
/**
 * queued_spin_is_locked - is the spinlock locked?
 * @lock: Pointer to queued spinlock structure
 * Return: 1 if it is locked, 0 otherwise
 */
static __always_inline int queued_spin_is_locked(struct qspinlock *lock)
{
	/*
	 * Any !0 state indicates it is locked, even if _Q_LOCKED_VAL
	 * isn't immediately observable.
	 */
	return atomic_read(&lock->val);
}
#endif

/**
 * queued_spin_value_unlocked - is the spinlock structure unlocked?
 * @lock: queued spinlock structure
 * Return: 1 if it is unlocked, 0 otherwise
 *
 * N.B. Whenever there are tasks waiting for the lock, it is considered
 *      locked wrt the lockref code to avoid lock stealing by the lockref
 *      code and change things underneath the lock. This also allows some
 *      optimizations to be applied without conflict with lockref.
 */
static __always_inline int queued_spin_value_unlocked(struct qspinlock lock)
{
	return !lock.val.counter;
}

/**
 * queued_spin_is_contended - check if the lock is contended
 * @lock : Pointer to queued spinlock structure
 * Return: 1 if lock contended, 0 otherwise
 */
static __always_inline int queued_spin_is_contended(struct qspinlock *lock)
{
	return atomic_read(&lock->val) & ~_Q_LOCKED_MASK;
}
/**
 * queued_spin_trylock - try to acquire the queued spinlock
 * @lock : Pointer to queued spinlock structure
 * Return: 1 if lock acquired, 0 if failed
 */
static __always_inline int queued_spin_trylock(struct qspinlock *lock)
{
	int val = atomic_read(&lock->val);

	if (unlikely(val))
		return 0;

	if (likely(atomic_try_cmpxchg_acquire(&lock->val, &val, _Q_LOCKED_VAL))) {
		/*
		 * IVH ownership-transfer site A2 (build plan sec 3.3.4).
		 * Distinct from A6: inside kernel/locking/qspinlock.c this name
		 * is #define'd to pv_hybrid_queued_unfair_trylock()
		 * (kernel/locking/qspinlock_paravirt.h), so the PV slowpath's
		 * trylocks never reach this function and are stamped there
		 * instead.  This one covers every OTHER caller of
		 * queued_spin_trylock() in the kernel.
		 *
		 * Strictly AFTER the acquiring cmpxchg; see
		 * __ivh_lock_set_holder() for why the ordering argument is
		 * store->store under x86-TSO and not "the locked RMW fences it".
		 */
		ivh_lock_set_holder(lock);
		return 1;
	}

	return 0;
}

extern void queued_spin_lock_slowpath(struct qspinlock *lock, u32 val);

#ifndef queued_spin_lock
/**
 * queued_spin_lock - acquire a queued spinlock
 * @lock: Pointer to queued spinlock structure
 */
static __always_inline void queued_spin_lock(struct qspinlock *lock)
{
	int val = 0;

	if (likely(atomic_try_cmpxchg_acquire(&lock->val, &val, _Q_LOCKED_VAL))) {
		/*
		 * IVH ownership-transfer site A1 (build plan sec 3.3.4): the
		 * uncontended fast path, and THE cost that decides whether any
		 * of this can ship.  This is the hottest lock site in the
		 * kernel -- a single LOCK CMPXCHG today -- so the whole
		 * "is a gated store on the ownership path affordable" question
		 * (build plan sec 1.1) is really a question about this line.
		 * It is answered by A/B'ing ivh_lock_holder_enabled 0 vs 1 on
		 * the same workload inside one boot, which is why the gate is a
		 * sysctl and not a Kconfig.
		 */
		ivh_lock_set_holder(lock);
		return;
	}

	queued_spin_lock_slowpath(lock, val);
}
#endif

#ifndef queued_spin_unlock
/**
 * queued_spin_unlock - release a queued spinlock
 * @lock : Pointer to queued spinlock structure
 */
static __always_inline void queued_spin_unlock(struct qspinlock *lock)
{
	/*
	 * IVH release site R1 (build plan sec 3.3.4), strictly BEFORE the
	 * releasing store.  smp_store_release() orders every prior store ahead
	 * of itself, so the plain WRITE_ONCE inside cannot be moved past the
	 * release and no extra barrier is needed -- but it must be here rather
	 * than after, because the moment the lock byte clears another CPU may
	 * already own this lock and a clear placed after would wipe ITS stamp.
	 * That is the same failure that disqualifies _raw_spin_unlock_bh()'s
	 * cs_exit() as a holder-identity site (see <linux/ivh_lock_holder.h>).
	 *
	 * Note this generic body is NOT the live one on x86 with
	 * CONFIG_PARAVIRT_SPINLOCKS: <asm/qspinlock.h> overrides
	 * queued_spin_unlock and routes to R2/R3/R4 instead.
	 */
	ivh_lock_clear_holder(lock);
	/*
	 * unlock() needs release semantics:
	 */
	smp_store_release(&lock->locked, 0);
}
#endif

#ifndef virt_spin_lock
static __always_inline bool virt_spin_lock(struct qspinlock *lock)
{
	return false;
}
#endif

#ifndef __no_arch_spinlock_redefine
/*
 * Remapping spinlock architecture specific functions to the corresponding
 * queued spinlock functions.
 */
#define arch_spin_is_locked(l)		queued_spin_is_locked(l)
#define arch_spin_is_contended(l)	queued_spin_is_contended(l)
#define arch_spin_value_unlocked(l)	queued_spin_value_unlocked(l)
#define arch_spin_lock(l)		queued_spin_lock(l)
#define arch_spin_trylock(l)		queued_spin_trylock(l)
#define arch_spin_unlock(l)		queued_spin_unlock(l)
#endif

#endif /* __ASM_GENERIC_QSPINLOCK_H */
