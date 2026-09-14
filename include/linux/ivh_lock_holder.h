/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_IVH_LOCK_HOLDER_H
#define _LINUX_IVH_LOCK_HOLDER_H

#include <linux/compiler.h>
#include <linux/types.h>

struct qspinlock;

/*
 * ---------------------------------------------------------------------------
 * IVH lock-holder identity (Build 1, tools/bpf/docs/
 * ivh_tsc_full_redesign_build_plan_2026-07-29.md sec 3.3)
 * ---------------------------------------------------------------------------
 *
 * WHAT THIS ANSWERS.  A waiter spinning on the lock BYTE -- the queue head in
 * pv_wait_head_or_lock() -- is waiting on an owner it has no name for.  The
 * MCS-node waiters do not have this problem: for an MCS node `prev->cpu` is
 * genuinely "the thing we are waiting on", which is why pv_wait_early()
 * already has everything it needs and is deliberately left alone.  The queue
 * head has nothing.  is_cs_preempted() cannot be asked "is the holder
 * preempted?" until something first records WHO the holder is, and that is
 * the entire job of this file.
 *
 * WHY NOT cs_enter()/cs_exit() (kernel/locking/spinlock.c), which already
 * bracket every outermost hold.  Four disqualifiers, all re-verified against
 * this tree in sec 3.3.1 of the build plan, and every one of them is fatal
 * for IDENTITY even though only the last is fatal for the CS STAMP:
 *
 *   1. cs_enter() runs its body only at current->lock_depth == 1.  The holder
 *      of an INNER lock -- exactly the holder a waiter on that inner lock
 *      needs to name -- is never recorded.
 *   2. All ten wrapper call sites are gated on !in_interrupt().  A lock held
 *      from hardirq or softirq context has no holder record at all.
 *   3. _raw_spin_unlock_bh() runs its cs_exit() AFTER the real unlock (there
 *      is an in-tree comment saying so).  A holder-clear there can wipe a
 *      NEW holder's identity after a fast handoff.  That is a live WRONG
 *      ANSWER, not a stale one, and it cannot be repaired by the
 *      pointer-match trick that makes the CS stamp survivable.
 *   4. Both are gated on ivh_universal_eligible && !ivh_exclude.  The holder
 *      that needs identifying is very often ineligible code -- a kernel
 *      thread, an excluded process, anything IVH does not manage.
 *
 * And a fifth, positive argument rather than the absence of an objection:
 * those wrappers are only reached through raw_spin_lock() and friends, so
 * every direct user of arch_spin_lock()/queued_spin_lock() -- including the
 * scheduler internals and the qspinlock machinery itself -- bypasses them.
 * A stamp at the qspinlock layer catches EVERY qspinlock acquisition in the
 * kernel.
 *
 * WHY THIS IS ITS OWN HEADER, and not <asm/ivh_tsc_beat.h> where the build
 * plan's sec 3.1 table nominally puts it.  The stamp/clear call sites are
 * include/asm-generic/qspinlock.h and arch/x86/include/asm/qspinlock.h, and
 * <asm/qspinlock.h> CANNOT include <asm/ivh_tsc_beat.h>: that header needs
 * rdtsc() -> <asm/tsc.h> -> <asm/msr.h> -> <linux/percpu.h> ->
 * <linux/sched.h>, and reaching <linux/sched.h> from inside <asm/qspinlock.h>
 * defines the generic vcpu_is_preempted(int) fallback before the x86
 * vcpu_is_preempted(long) below it is finished, and the build dies.  There is
 * already a standing "do not tidy the heartbeat back into this file" comment
 * in <asm/qspinlock.h> recording that.  So the API lives here, in a header
 * whose entire dependency set is <linux/compiler.h> plus <linux/types.h>,
 * which is includable from anywhere; <asm/ivh_tsc_beat.h> includes this file
 * so that everything reached through the heartbeat header still sees the API
 * exactly as the build plan describes.
 *
 * WHY THE WORKERS ARE OUT OF LINE, with only the GATE inlined.  Same reason:
 * the body needs hash_ptr() and raw_smp_processor_id(), and pulling those in
 * here would drag <linux/percpu.h> back into <asm/qspinlock.h> and reopen the
 * build break above.  Splitting it this way keeps the property that actually
 * matters for the default configuration -- at ivh_lock_holder_enabled == 0
 * the cost at every acquire and release is ONE READ_ONCE of a read-mostly
 * global plus one perfectly-predicted branch, the identical posture
 * ivh_beat_publish_in_spin() already has and the identical posture that makes
 * that one safe to leave compiled in permanently.  The honest caveat, stated
 * here so nobody has to rediscover it while reading throughput numbers: at
 * enabled == 1 the measured cost includes an out-of-line call that an
 * eventually-shipped version could inline away, so the A/B of sec 3.9 item 7
 * is a PESSIMISTIC bound on the store's cost, not a tight one.  Failing in
 * the pessimistic direction is the right way round for a go/no-go decision.
 *
 * CONFIG GATING.  The storage, the counters and the table all live in
 * arch/x86/kernel/kvm.c inside its #ifdef CONFIG_PARAVIRT_SPINLOCKS block,
 * which is the same home and the same guard the TSC heartbeat's counters
 * already use, and the same guard /proc/ivh_debug already reads them under.
 * Everywhere else -- every other architecture, and an x86 build without KVM
 * guest support or without PV spinlocks -- gets the no-op stubs below, so
 * include/asm-generic/qspinlock.h stays buildable for arm64/riscv/etc.
 */

#if defined(CONFIG_X86) && defined(CONFIG_KVM_GUEST) && \
    defined(CONFIG_PARAVIRT_SPINLOCKS)

/*
 * Table geometry.  The table is allocated ONCE at its maximum size and the
 * EFFECTIVE index width is the sysctl ivh_holder_bits, clamped into
 * [IVH_HOLDER_MIN_BITS, IVH_HOLDER_MAX_BITS].  That single decision is what
 * makes the sizing question answerable inside ONE boot: sweep the sysctl from
 * small to maximum under a fixed workload, read
 * ivh_holder_unknown_collision / ivh_holder_stamps at each setting, and the
 * whole size-vs-accuracy curve falls out of one kernel.  Without it every
 * point on that curve costs a rebuild and a reboot, which is the specific
 * failure this build is designed to avoid.
 *
 * 16 bits => 65536 slots x 64 B = 4 MB, trivially affordable on this research
 * guest.  6 bits (64 slots) is the low end, chosen to be small enough that
 * collisions are guaranteed and the curve therefore has a visible bad end to
 * anchor against -- a sweep whose worst point is already good tells you
 * nothing about where the knee is.
 */
#define IVH_HOLDER_MAX_BITS	16
#define IVH_HOLDER_MIN_BITS	6

extern unsigned long ivh_lock_holder_enabled;
extern unsigned long ivh_holder_bits;

void __ivh_lock_set_holder(struct qspinlock *lock);
void __ivh_lock_clear_holder(struct qspinlock *lock);

/*
 * Returns the CPU currently recorded as owning @lock, or -1 for "unknown".
 * "Unknown" is the SAFE direction by construction and every caller must treat
 * it as "do not act": the two causes are counted separately
 * (ivh_holder_unknown_empty for the genuine handoff window, which is
 * irreducible physics, and ivh_holder_unknown_collision for table geometry,
 * which the ivh_holder_bits sweep drives toward zero) because a single lumped
 * "unknown" counter could not distinguish "too small a table" from "nothing
 * to be done about it".
 */
int  ivh_lock_holder_cpu(struct qspinlock *lock);

static __always_inline void ivh_lock_set_holder(struct qspinlock *lock)
{
	if (likely(!READ_ONCE(ivh_lock_holder_enabled)))
		return;
	__ivh_lock_set_holder(lock);
}

static __always_inline void ivh_lock_clear_holder(struct qspinlock *lock)
{
	if (likely(!READ_ONCE(ivh_lock_holder_enabled)))
		return;
	__ivh_lock_clear_holder(lock);
}

#else /* !(x86 && KVM guest && PV spinlocks) */

static inline void ivh_lock_set_holder(struct qspinlock *lock) { }
static inline void ivh_lock_clear_holder(struct qspinlock *lock) { }
static inline int  ivh_lock_holder_cpu(struct qspinlock *lock) { return -1; }

#endif

#endif /* _LINUX_IVH_LOCK_HOLDER_H */
