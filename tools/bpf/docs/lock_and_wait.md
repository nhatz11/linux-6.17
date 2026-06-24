  ---
  Coverage audit: spinlock tracking in your 6.17-rseqport build

  CONFIG_PREEMPT_RT=n, CONFIG_DEBUG_LOCK_ALLOC=n, CONFIG_DEBUG_SPINLOCK=n — those conditions matter
  for which paths are active.

  ---
  lock_depth — lockholder tracking

  Covered

  ┌─────────────────────────────┬───────────────────────────────┬─────────────────────────────────┐
  │          Function           │             File              │              Note               │
  ├─────────────────────────────┼───────────────────────────────┼─────────────────────────────────┤
  │ _raw_spin_trylock           │ kernel/locking/spinlock.c:163 │ conditional on return value     │
  ├─────────────────────────────┼───────────────────────────────┼─────────────────────────────────┤
  │ _raw_spin_trylock_bh        │ kernel/locking/spinlock.c:176 │ conditional on return value     │
  ├─────────────────────────────┼───────────────────────────────┼─────────────────────────────────┤
  │ _raw_spin_lock              │ kernel/locking/spinlock.c:190 │                                 │
  ├─────────────────────────────┼───────────────────────────────┼─────────────────────────────────┤
  │ _raw_spin_lock_irqsave      │ kernel/locking/spinlock.c:202 │                                 │
  ├─────────────────────────────┼───────────────────────────────┼─────────────────────────────────┤
  │ _raw_spin_lock_irq          │ kernel/locking/spinlock.c:217 │                                 │
  ├─────────────────────────────┼───────────────────────────────┼─────────────────────────────────┤
  │ _raw_spin_lock_bh           │ kernel/locking/spinlock.c:229 │                                 │
  ├─────────────────────────────┼───────────────────────────────┼─────────────────────────────────┤
  │ _raw_spin_unlock            │ kernel/locking/spinlock.c:242 │                                 │
  ├─────────────────────────────┼───────────────────────────────┼─────────────────────────────────┤
  │ _raw_spin_unlock_irqrestore │ kernel/locking/spinlock.c:254 │                                 │
  ├─────────────────────────────┼───────────────────────────────┼─────────────────────────────────┤
  │ _raw_spin_unlock_irq        │ kernel/locking/spinlock.c:266 │                                 │
  ├─────────────────────────────┼───────────────────────────────┼─────────────────────────────────┤
  │ _raw_spin_unlock_bh         │ kernel/locking/spinlock.c:278 │                                 │
  ├─────────────────────────────┼───────────────────────────────┼─────────────────────────────────┤
  │                             │                               │ inline; calls spin_lock() →     │
  │ write_seqlock* family       │ include/linux/seqlock.h:874+  │ raw_spin_lock() →               │
  │                             │                               │ _raw_spin_lock — covered        │
  │                             │                               │ implicitly                      │
  ├─────────────────────────────┼───────────────────────────────┼─────────────────────────────────┤
  │ raw_spin_trylock_irqsave    │                               │ macro; calls raw_spin_trylock() │
  │ macro                       │ include/linux/spinlock.h:296  │  → _raw_spin_trylock — covered  │
  │                             │                               │ implicitly                      │
  └─────────────────────────────┴───────────────────────────────┴─────────────────────────────────┘

  Not covered (lock_depth)

  Mechanism: bit_spin_lock / bit_spin_trylock / bit_spin_unlock
  File: include/linux/bit_spinlock.h:16,41,57
  Purpose: Inline bit-flag spinlock. Callers: mm/slub.c:678 (per-slab lock),
    include/linux/list_bl.h:148 (dcache hlist_bl), include/linux/rhashtable.h:332,344 (RCU hash
    tables), include/linux/jbd2.h:343 (journal head), include/linux/rmap.h:180 (folio reverse-map).
    Completely __always_inline — not interceptable at the _raw_spin_lock level.
  ────────────────────────────────────────
  Mechanism: _raw_spin_lock_nested
  File: kernel/locking/spinlock.c:450
  Purpose: Lockdep subclass variant. Only compiled under CONFIG_DEBUG_LOCK_ALLOC — currently dead code

    in your build, not a real gap right now.
  ────────────────────────────────────────
  Mechanism: _raw_spin_lock_irqsave_nested
  File: kernel/locking/spinlock.c:458
  Purpose: Same condition, same conclusion.
  ────────────────────────────────────────
  Mechanism: _raw_spin_lock_nest_lock
  File: kernel/locking/spinlock.c:471
  Purpose: Same condition, same conclusion.
  ────────────────────────────────────────
  Mechanism: _raw_read_lock* / _raw_write_lock*
  File: kernel/locking/spinlock.c:293–443
  Purpose: rwlock_t read/write variants. Deliberate exclusion per your design — these are not raw
    spinlocks for LHP purposes.
  ────────────────────────────────────────
  Mechanism: rt_spin_lock*
  File: kernel/locking/spinlock_rt.c:54+
  Purpose: PREEMPT_RT spinlock API — dead code in this build since CONFIG_PREEMPT_RT=n.

  ---
  wait_depth — spinner/waiter tracking

  Covered

  ┌────────────────┬───────────────────────────────────────────────┬──────────────────────────────┐
  │   Mechanism    │                     File                      │    Spinning phase tracked    │
  ├────────────────┼───────────────────────────────────────────────┼──────────────────────────────┤
  │                │                                               │ MCS-based optimistic spin    │
  │ osq_lock /     │ kernel/locking/osq_lock.c:119,156,185,208,224 │ queue; entered by mutex and  │
  │ osq_unlock     │                                               │ rwsem before owner-watching  │
  │                │                                               │ phase                        │
  ├────────────────┼───────────────────────────────────────────────┼──────────────────────────────┤
  │ qspinlock      │                                               │ PENDING bit busy-wait        │
  │ pending-bit    │ kernel/locking/qspinlock.c:200,204            │ (first-waiter fast path)     │
  │ spin           │                                               │                              │
  ├────────────────┼───────────────────────────────────────────────┼──────────────────────────────┤
  │ qspinlock MCS  │ kernel/locking/qspinlock.c:231,394            │ Full MCS node spin in the    │
  │ spin           │                                               │ slowpath queue               │
  └────────────────┴───────────────────────────────────────────────┴──────────────────────────────┘

  Not covered (wait_depth)

  Mechanism: mutex_spin_on_owner
  File: kernel/locking/mutex.c:340
  Spinning phase missed: After winning the OSQ, the task spins watching owner->on_cpu until the holder

    reschedules or hands off the mutex. This phase is between osq_lock() success and actual mutex
    acquisition — wait_depth drops to 0 mid-spin.
  ────────────────────────────────────────
  Mechanism: ww_mutex_spin_on_owner
  File: kernel/locking/mutex.c:292
  Spinning phase missed: WW-mutex variant of the above; same gap.
  ────────────────────────────────────────
  Mechanism: rwsem_spin_on_owner
  File: kernel/locking/rwsem.c:744
  Spinning phase missed: Same pattern for rwsem writer acquisition: after OSQ win, spins on
    owner->on_cpu.
  ────────────────────────────────────────
  Mechanism: rtmutex_spin_on_owner
  File: kernel/locking/rtmutex.c:1481
  Spinning phase missed: RT-mutex optimistic spin loop on SMP. RT-mutex is not OSQ-based — it has its
    own owner-watching loop with no OSQ involvement, so there is no osq_lock call to catch it.

  ---
  Summary

  For lock_depth: the only real gap is bit_spin_lock. It's inline, used in
  slub/dcache/rhashtable/jbd2, and you'd need to either intercept it via kprobes/fprobe or accept that
  those subsystems won't contribute to the lockholder signal. In practice, slab and dcache hold these
  locks for microseconds under non-overlapping access patterns, so the LHP impact is low — but they
  can show up in production. The _raw_spin_lock_nested family and rt_spin_lock are not active in your
  build.

  For wait_depth: the spin_on_owner phase (mutex/rwsem/rtmutex) is a genuine gap. The OSQ entry is
  tracked, but the owner-watching loop after OSQ success runs with wait_depth==0. Whether this matters
  depends on how long that phase is — typically it's a few cpu_relax() iterations before the owner
  releases, so it's short. If you want to close it, mutex_spin_on_owner and rwsem_spin_on_owner are
  the two noinline functions you'd add wait_depth++/-- around.

✻ Baked for 3m 43s

※ recap: Auditing spinlock coverage for lock_depth and wait_depth tracking in a Linux 6.17 LHP port.
  The one gap that matters is bit_spin_lock — inline, untracked, used in slub/dcache/rhashtable/jbd2 —
  decide whether to instrument it or accept the omission. (disable recaps in /config)

──────────────────────────────────────────────────────────────────────────────────────────────────────
❯ 
──────────────────────────────────────────────────────────────────────────────────────────────────────
  ? for shortcuts · ← for agents

