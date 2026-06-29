# IVH State of the Art — Linux 6.17-rseqport (2026-06-26)

## Overview

IVH (Interruptible Virtual Host) is a synchronous pre-lock migration system.
When a task is about to acquire a raw spinlock on a throttled vCPU, IVH
migrates the task *before* it touches the lock, so the subsequent lock
acquisition happens on a healthy vCPU.

This document supersedes `experiment_plan.md` and `MY_ivh_atc_reference.md`,
which contain outdated designs and known mistakes.

---

## Problem Statement

On over-committed virtual machines, the hypervisor time-slices vCPUs.  A task
can begin a spinlock critical section on a vCPU, then get preempted by the
hypervisor mid-CS.  All other tasks spinning on the same lock burn CPU cycles
waiting for a vCPU that is not scheduled — the "vCPU preemption" problem
(addressed by PLE and PV spinlock in other kernels).

The prior IVH design fired AFTER the task acquired the lock
(`bpf_sched_lock_acquire()` → `running_migration()` → async IPI).  This was
fundamentally broken:

1. **Wrong task moved**: `migrate_task_to_async_fair` iterates `rq->cfs_tasks`
   (the runqueue list).  The *running* lockholder is dequeued while running, so
   a random runnable task was moved, not the lockholder.
2. **MCS corruption risk**: qspinlock slowpath allocates a per-CPU MCS node at
   line 232 (`this_cpu_ptr(&qnodes[0].mcs)`).  Moving the task after this point
   means `__this_cpu_dec(qnodes[0].mcs.count)` at line 398 fires on a different
   CPU, corrupting that CPU's per-CPU node count.

---

## Current Architecture: Synchronous Pre-Lock Migration

### Kernel entry point: `ivh_pre_lock()` (kernel/locking/spinlock.c:46)

```c
static __always_inline void ivh_pre_lock(void)
{
    if (!bpf_sched_enabled())       /* static key — zero cost when IVH off */
        return;
    if (!in_task() || !preemptible() || current->lock_depth > 0)
        return;
    bpf_sched_pre_lock_migrate();
}
```

Called before `__raw_spin_lock*()` in 4 blocking variants:
- `_raw_spin_lock` (spinlock.c:214)
- `_raw_spin_lock_irqsave` (spinlock.c:229)
- `_raw_spin_lock_irq` (spinlock.c:243)
- `_raw_spin_lock_bh` (spinlock.c:257)

NOT called for trylocks (`_raw_spin_trylock*`) — they are speculative and
non-blocking.

`lock_depth > 0` prevents re-entry for nested locks.  The outer lock already
holds the CPU context; scheduling would deadlock.

### Migration function: `bpf_sched_pre_lock_migrate()` (kernel/sched/fair.c)

Gates (in order, cheapest first):

1. **Capacity**: `rq->cpu_capacity > IVH_CAPACITY_THRESHOLD (900)` → skip.
   vCPU is healthy; no reason to migrate.
2. **Time left**: if burst time remaining ≥ `ivh_time_left_threshold_ns`
   (default 500000 ns) → skip.  vCPU is early in its burst; will likely finish
   the CS before preemption.
3. **Movability**: `cpumask_weight(current->cpus_ptr) <= 1` → skip.  Task is
   pinned to this CPU.
4. **Lock pending**: `rq->preempt_migrate_locked` → skip.  Migration already
   in progress on this CPU.

If all gates pass:

```c
current->lock_depth++;          /* recursion guard — prevents re-entry from
                                 * raw_spin_lock_irqsave(&my_spinlock) below */

raw_spin_lock_irqsave(&my_spinlock, flags);
target_cpu = bpf_sched_cfs_select_run_cpu_spin(...);   /* calls test3() */
if (target_cpu != -1)
    atomic_fetch_or(PRMPT_HELD_MASK, prmpt_flags(target_cpu));
raw_spin_unlock_irqrestore(&my_spinlock, flags);

/* Synchronous self-migration */
cpumask_copy(saved_mask, &current->cpus_mask);
if (set_cpus_allowed_ptr(current, cpumask_of(target_cpu)) == 0)
    schedule();                 /* task wakes on target_cpu */
set_cpus_allowed_ptr(current, saved_mask);
free_cpumask_var(saved_mask);
atomic_fetch_andnot(PRMPT_HELD_MASK, prmpt_flags(target_cpu));
current->lock_depth--;
```

**Critical recursion guard**: `raw_spin_lock_irqsave(&my_spinlock)` goes
through `_raw_spin_lock_irqsave()` → `ivh_pre_lock()`.  At that point
`lock_depth` is still 0 unless the `lock_depth++` guard is in place.  Without
it, `ivh_pre_lock()` re-enters `bpf_sched_pre_lock_migrate()` → stack
overflow.  The `++` before the first spinlock prevents this.

**Heap-allocated cpumask**: uses `alloc_cpumask_var(&saved_mask, GFP_KERNEL)`
to avoid a 1024-byte stack frame from a on-stack `cpumask_t`.

### Why synchronous self-migration is safe here

- `lock_depth == 0` (first lock of this context) → preempt_count is clean.
- `preemptible()` passes → IRQs enabled, no outer spinlock held.
- `in_task()` → process context; `current` is meaningful.
- `set_cpus_allowed_ptr` + `schedule()` is the standard kthread binding pattern.
- MCS node is allocated in `queued_spin_lock_slowpath()` which runs AFTER
  `ivh_pre_lock()` returns.  The node is created on the correct CPU.

---

## BPF CPU Selection: `test3()` / `process_cpu()`

File: `tools/bpf/MY_ivh_atc.bpf.c`

Hook: `sched/cfs_select_run_cpu_spin`

### JIT worker banlist (CRITICAL — crash fix 2026-06-25)

Added at the top of `test3()` to mirror the identical check in `test()`:

```c
u32 tgid = curr->tgid;
if (is_jit_worker(curr)) {
    /* seed jit_tgids so all sibling threads sharing the same mm
     * are also blocked; BPF_NOEXIST avoids redundant map writes */
    if (!bpf_map_lookup_elem(&jit_tgids, &tgid)) {
        u8 one = 1;
        bpf_map_update_elem(&jit_tgids, &tgid, &one, BPF_NOEXIST);
    }
    return -1;
}
if (bpf_map_lookup_elem(&jit_tgids, &tgid))
    return -1;
```

**Why this matters**: JIT runtimes (Bun, JavaScriptCore, Mesa llvmpipe)
interleave spinlock-protected work with `mprotect(PROT_EXEC)` calls for JIT
code emission.  When IVH migrates a JIT thread, `set_cpus_allowed_ptr` expands
`mm->cpu_bitmap` to include the destination CPU.  Future `flush_tlb_mm()` calls
then send IPIs to all CPUs in the bitmap.  If any of those CPUs are in an
IRQ-disabled spinlock critical section, they cannot handle the IPI — the TLB
flush caller stalls → soft lockup → kernel panic.

**The 2026-06-25 overnight crash**: The `test()` hook (tick path) had the JIT
banlist, but `test3()` (CPU selection hook, called directly from
`bpf_sched_pre_lock_migrate()`) did not.  HeapHelper (JSC GC thread) was
migrated across all 16 CPUs via the pre-lock path overnight, expanding
`mm->cpu_bitmap` to `0xffff`.  The final log entries before crash showed every
CPU reporting `mm_bits=0xffff` and HeapHelper as the last migration.

### 2-tier CPU preference

`process_cpu()` searches candidate CPUs with this preference order:

**Tier 1 — Active workers** (`!idle_cpu && lock_depth==0 && wait_depth==0`):
Task is computing with no lock involvement.  No hypervisor vCPU wake-up cost
(CPU is already scheduled).  Stop loop immediately (`return 1`).

**Tier 2 — Idle vCPUs** (`idle_cpu`): Safe fallback; requires a hypervisor
vCPU wake-up.  Keep searching for Tier 1 but save as fallback.

Rationale for NOT targeting spinners (`wait_depth > 0`):
- If the lock is free: the spinner already chose this vCPU as safe for itself.
  Migrating another task here risks thrashing.
- If the lock is held: the migrated task joins the MCS queue behind the existing
  waiter, giving it lower priority — the "waiter preemption problem."

Rationale for NOT targeting lockholders (`lock_depth > 0`):
- Adding a spinlock waiter behind a lockholder on a potentially throttled vCPU
  is counterproductive.

### Remaining per-CPU health checks

Applied after the tier check, in `process_cpu()`:

- `prmpt_flags & PRMPT_HELD_MASK`: CPU is already claimed by another concurrent
  IVH migration.  Skip.
- `cpu_capacity <= 500 && <= average_capacity`: CPU is throttled below half AND
  below the system average.  Skip.
- `is_cpu_preempted(rq, now) == 0`: Heartbeat (`clock_preempt`) is stale by
  < 300µs.  The vCPU may not be running.  Skip.
- `last_preemption <= source_rq->last_preemption`: Target vCPU started its
  active burst earlier than ours — it has less remaining burst time.  Skip.
- Budget exhaustion: `target_active >= ewma_act_ns` (target has used its typical
  active window).  Migrating there puts the task on a vCPU about to be yanked.

---

## Userspace Path: `sys_ivh_cs_enter` (syscall 470)

File: `kernel/sched/bpf_sched.c`

```c
SYSCALL_DEFINE0(ivh_cs_enter)
{
    if (!bpf_sched_enabled())
        return 0;
    bpf_sched_pre_lock_migrate();
    return 0;
}
```

Called from:
- `NHextend.c`: `ivh_cs_enter()` BEFORE `start_wait = get_time()` and before
  `wait_enter()`.  Migration time is excluded from wait-time measurements.
- `pthread_spin_lock.c`: `__spin_ivh_cs_enter()` before the first CAS, guarded
  by `__spin_depth == 0` (no migration during nested spinning).

`bpf_sched_lock_acquire()` has been **deleted** — it was the old post-lock
migration entry point that was architecturally broken.

---

## Dead Code

### `running_migration()` (kernel/sched/fair.c)

Has no callers.  The only remaining logic:
```c
if (rq->preempt_migrate_locked == 1) return 0;
```
All other gates (lockholder check, async IPI path) have been removed.
Can be deleted in a cleanup pass.

---

## lock_depth / wait_depth Coverage

See `lock_and_wait.md` for the full coverage audit.  Summary:

**lock_depth** tracks raw spinlock hold depth.
- Covered: all `_raw_spin_lock*` / `_raw_spin_unlock*` blocking variants.
- Not covered: `bit_spin_lock` (inline, used in slub/dcache/jbd2).  Low impact
  in practice — bit_spin_lock critical sections are microsecond-range and
  non-overlapping.

**wait_depth** tracks MCS queue membership.
- Covered: qspinlock pending-bit spin, qspinlock MCS spin, OSQ lock.
- Gap: `mutex_spin_on_owner` / `rwsem_spin_on_owner` phase (after OSQ win,
  before actual mutex acquisition) — `wait_depth` drops to 0 mid-spin.
  Typically short enough to be acceptable.

---

## Files Changed from Stock 6.17

| File | Change |
|------|--------|
| `kernel/locking/spinlock.c` | Added `ivh_pre_lock()`, inserted before `__raw_spin_lock*()` in 4 blocking variants; removed `bpf_sched_lock_acquire()` from `cs_enter()` |
| `kernel/sched/fair.c` | Added `bpf_sched_pre_lock_migrate()` with recursion guard; simplified `running_migration()` (now dead code) |
| `kernel/sched/bpf_sched.c` | `sys_ivh_cs_enter` calls `bpf_sched_pre_lock_migrate()` directly; deleted `bpf_sched_lock_acquire()` |
| `include/linux/bpf_sched.h` | Added `bpf_sched_pre_lock_migrate()` declaration; removed `bpf_sched_lock_acquire()` declaration |
| `tools/bpf/MY_ivh_atc.bpf.c` | 2-tier CPU selection in `process_cpu()`; JIT banlist in `test3()` (crash fix); regenerated `vmlinux.h` for `ewma_act_ns` |

---

## Experiment Results (2026-06-26)

- VM stable with IVH loaded under hackbench + co-VM sysbench contention.
- Migrations observed: rcu_preempt, kworker, sshd-session, hackbench pool-0,
  systemd, dbus-daemon at 5–15 per 5-second window per CPU.
- **No JIT worker migrations**: HeapHelper, Bun Pool, JITWorker absent from log.
  JIT banlist fix confirmed working.
- Migration direction: always from throttled source CPUs to Tier 1 (active
  worker) or Tier 2 (idle) target CPUs.
- All migrations are synchronous: `running_migration()` has no callers in kernel
  source (confirmed via grep); all migration decisions go through
  `bpf_sched_pre_lock_migrate()`.

---

## Known Issues / Future Work

1. **`running_migration()` is dead code**: safe to delete.
2. **`bit_spin_lock` gap**: slub/dcache/jbd2 lockholders not tracked by
   `lock_depth`.  Instrument via kprobes or accept the omission.
3. **mutex `spin_on_owner` gap**: `wait_depth` drops to 0 after OSQ win but
   before mutex acquisition.  Rarely long enough to matter.
4. **`sshd-session` mm_bits=0xffff**: SSH process shares mm across all CPUs
   after IVH migrations.  Lower-risk than JIT (no frequent mprotect), but worth
   monitoring.
