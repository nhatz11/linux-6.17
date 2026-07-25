/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_X86_QSPINLOCK_H
#define _ASM_X86_QSPINLOCK_H

#include <linux/jump_label.h>
#include <asm/cpufeature.h>
#include <asm-generic/qspinlock_types.h>
#include <asm/paravirt.h>
#include <asm/rmwcc.h>

#define _Q_PENDING_LOOPS	(1 << 9)

#define queued_fetch_set_pending_acquire queued_fetch_set_pending_acquire
static __always_inline u32 queued_fetch_set_pending_acquire(struct qspinlock *lock)
{
	u32 val;

	/*
	 * We can't use GEN_BINARY_RMWcc() inside an if() stmt because asm goto
	 * and CONFIG_PROFILE_ALL_BRANCHES=y results in a label inside a
	 * statement expression, which GCC doesn't like.
	 */
	val = GEN_BINARY_RMWcc(LOCK_PREFIX "btsl", lock->val.counter, c,
			       "I", _Q_PENDING_OFFSET) * _Q_PENDING_VAL;
	val |= atomic_read(&lock->val) & ~_Q_PENDING_MASK;

	return val;
}

#ifdef CONFIG_PARAVIRT_SPINLOCKS
extern void native_queued_spin_lock_slowpath(struct qspinlock *lock, u32 val);
extern void __pv_init_lock_hash(void);
extern void __pv_queued_spin_lock_slowpath(struct qspinlock *lock, u32 val);
extern void __raw_callee_save___pv_queued_spin_unlock(struct qspinlock *lock);
extern bool nopvspin;

#define	queued_spin_unlock queued_spin_unlock
/**
 * queued_spin_unlock - release a queued spinlock
 * @lock : Pointer to queued spinlock structure
 *
 * A smp_store_release() on the least-significant byte.
 */
static inline void native_queued_spin_unlock(struct qspinlock *lock)
{
	smp_store_release(&lock->locked, 0);
}

static inline void queued_spin_lock_slowpath(struct qspinlock *lock, u32 val)
{
	pv_queued_spin_lock_slowpath(lock, val);
}

static inline void queued_spin_unlock(struct qspinlock *lock)
{
	kcsan_release();
	pv_queued_spin_unlock(lock);
}

#define vcpu_is_preempted vcpu_is_preempted
static inline bool vcpu_is_preempted(long cpu)
{
	return pv_vcpu_is_preempted(cpu);
}

/*
 * IVH pv_wait/pv_kick mechanism toggle, default 0 (OFF/safe).
 *
 *   0 - "as-close-to-stock-as-possible" fallback: real host-cooperative
 *       halt + KVM_HC_KICK_CPU hypercall wake if the host advertises
 *       KVM_FEATURE_PV_UNHALT (byte-for-byte the pre-IVH kvm_wait()/
 *       kvm_kick_cpu() behavior), else a plain bounded cpu_relax() busy
 *       loop with no TPAUSE and no steal-bit logic — the least-surprising,
 *       lowest-risk degenerate case when the host offers nothing to
 *       cooperate with.
 *   1 - IVH's own non-hypervisor-cooperative substitute: TPAUSE-based
 *       backoff (never halts, never issues a wake hypercall) plus the
 *       steal-bit-gated early bailout in pv_wait_early().
 *   2 - "scoped halt + IPI wake": a real, host-visible HLT yield woken by
 *       smp_send_reschedule(), entered only when pv_wait_early() says the
 *       thing we are queued behind is genuinely not running.  NOTE: a
 *       maskable IPI cannot un-halt a HLT taken with RFLAGS.IF=0, so this
 *       mode halts ONLY via safe_halt() on the IRQs-were-enabled path; a
 *       waiter that arrives with IRQs already off degrades to mechanism 1's
 *       bounded poll instead.  See ivh_pv_wait() in arch/x86/kernel/kvm.c —
 *       do not "restore symmetry" with mechanism 0's bare halt() there.
 *
 * This only ever branches inside the already-registered, permanently
 * installed ivh_pv_wait()/ivh_pv_kick() callbacks (arch/x86/kernel/kvm.c)
 * and pv_wait_early() (kernel/locking/qspinlock_paravirt.h) — it never
 * touches virt_spin_lock_key or pv_ops.lock.* registration, both of which
 * are decided exactly once at boot in kvm_spinlock_init(), same as
 * upstream. See kvm_spinlock_init()'s comment for why that boot-time
 * decision is deliberately NOT made runtime-toggleable.
 *   echo 1 > /proc/sys/kernel/ivh_pv_wait_mechanism
 */
extern unsigned long ivh_pv_wait_mechanism;
#endif

#ifdef CONFIG_PARAVIRT
/*
 * virt_spin_lock_key - disables by default the virt_spin_lock() hijack.
 *
 * Native (and PV wanting native due to vCPU pinning) should keep this key
 * disabled. Native does not touch the key.
 *
 * When in a guest then native_pv_lock_init() enables the key first and
 * KVM/XEN might conditionally disable it later in the boot process again.
 */
DECLARE_STATIC_KEY_FALSE(virt_spin_lock_key);

/*
 * Shortcut for the queued_spin_lock_slowpath() function that allows
 * virt to hijack it.
 *
 * Returns:
 *   true - lock has been negotiated, all done;
 *   false - queued_spin_lock_slowpath() will do its thing.
 */
#define virt_spin_lock virt_spin_lock
static inline bool virt_spin_lock(struct qspinlock *lock)
{
	int val;

	if (!static_branch_likely(&virt_spin_lock_key))
		return false;

	/*
	 * On hypervisors without PARAVIRT_SPINLOCKS support we fall
	 * back to a Test-and-Set spinlock, because fair locks have
	 * horrible lock 'holder' preemption issues.
	 */

 __retry:
	val = atomic_read(&lock->val);

	if (val || !atomic_try_cmpxchg(&lock->val, &val, _Q_LOCKED_VAL)) {
		cpu_relax();
		goto __retry;
	}

	return true;
}

#endif /* CONFIG_PARAVIRT */

#include <asm-generic/qspinlock.h>

#endif /* _ASM_X86_QSPINLOCK_H */
