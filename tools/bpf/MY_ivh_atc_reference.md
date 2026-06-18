# MY_ivh_atc — Reference

## Purpose

IVH implementation for the vSched 6.17 port. Hooks into the scheduler tick
path to detect lockholder tasks that are candidates for migration to a less
preempted CPU.

---

## curr_lockholder: how it is computed

All computation is in `running_migration()` at `kernel/sched/fair.c:12940-12952`,
before the BPF hook is called.

### Kernel side

```c
int curr_lock_depth = rq->curr->lock_depth;
int curr_kernel_lockholder = (curr_lock_depth > 0);
```

`lock_depth` is a per-task counter incremented by every `raw_spin_lock`
acquired in process context and decremented on release. A value > 0 means
the task is actively holding one or more kernel spinlocks.

**Invariant:** `lock_depth` is never negative in normal execution. The
rq-lock transfer across `context_switch` (rq locked on old task, unlocked
on new task) required a two-part correction in `prepare_task_switch` and
`finish_lock_switch`; without it, `lock_depth` goes to -1 on the new task.
That fix is already applied.

Tracking sites in `kernel/locking/spinlock.c`:

| Function | Line | Direction |
|----------|------|-----------|
| `_raw_spin_trylock` | 141 | `++` (on success only) |
| `_raw_spin_trylock_bh` | 153 | `++` (on success only) |
| `_raw_spin_lock` | 164 | `++` |
| `_raw_spin_lock_irqsave` | 176 | `++` |
| `_raw_spin_lock_irq` | 187 | `++` |
| `_raw_spin_lock_bh` | 198 | `++` |
| `_raw_spin_unlock` | 207 | `--` before unlock |
| `_raw_spin_unlock_irqrestore` | 217 | `--` before unlock |
| `_raw_spin_unlock_irq` | 227 | `--` before unlock |
| `_raw_spin_unlock_bh` | 238 | `--` **after** unlock |

rwlock variants have no `lock_depth` tracking.

### Userspace side

```c
int curr_user_lockholder = 0;
#ifdef CONFIG_RSEQ
if (rq->curr->rseq && rq->curr->rseq_len >= 32) {
    u32 cr;
    if (!copy_from_user_nofault(&cr, &rq->curr->rseq->cr_counter, sizeof(cr)))
        curr_user_lockholder = (cr & 0xFFFFFFFEu) != 0;
}
#endif
```

`rseq->cr_counter` is a 32-bit field in the userspace-mapped `struct rseq`
(defined in `include/uapi/linux/rseq.h`). Bit layout:

| Bit | Name | Set by |
|-----|------|--------|
| 0 | `RSEQ_CR_FLAG_IN_CRITICAL_SECTION_BIT` | userspace — bare entry marker |
| 1 | `RSEQ_CR_FLAG_KERNEL_REQUEST_SCHED_BIT` | kernel — request task to yield |
| 2–31 | nesting / lock depth | userspace |

The mask `0xFFFFFFFEu` = `~1u` clears bit 0. The check `(cr & 0xFFFFFFFEu) != 0`
is true when any bits 1–31 are set — i.e., when userspace has incremented the
nesting depth beyond the bare entry flag. Bit 0 alone (bare entry) does not
qualify the task as a lockholder.

`copy_from_user_nofault` is used so that a faulting or absent mapping returns
an error rather than sleeping or crashing in the tick context.

### Combined gate

```c
int curr_lockholder = curr_kernel_lockholder || curr_user_lockholder;
```

Passed directly to the BPF hook as the `curr_lockholder` argument.

---

## BPF hook: `sched/cfs_sched_tick_end`

```c
SEC("sched/cfs_sched_tick_end")
int BPF_PROG(test, struct rq *rq, u64 now, unsigned int idle_cpus,
             int curr_lock_depth, int curr_kernel_lockholder,
             int curr_user_lockholder, int curr_lockholder)
```

The BPF_PROG macro is required. Without it, context args are NOT_INIT in the
BPF register file and the verifier rejects field dereferences on `rq`.

### Gate ordering in `test()`

1. `preempt_migrate_locked == 1` — IVH already pending, skip
2. `(h_nr_runnable - h_nr_idle) > 1` — source CPU is busy, skip
3. `curr == rq->idle` — idle task, skip
4. `!curr_lockholder` — not a lockholder, skip ← **lockholder gate (checked once, before shadow block)**
── shadow candidate block fires here ───────────────────────────────
5. `cpu_capacity > 900` — unthrottled CPU, skip
6. `last_preemption == 0` — no steal time ever recorded, skip
7. `idle_cpus == 0` — nowhere to migrate, skip
8. `get_task_runtime(now, rq) < 1ms` — not running long enough, skip
9. `util_percent < 60` — not CPU-intensive, skip
10. `!movable` — task is pinned, cannot migrate, skip ← **movability gate**

Returns 1 only when all gates pass.

---

## CPU selection hook: `sched/cfs_select_run_cpu_spin`

```c
SEC("sched/cfs_select_run_cpu_spin")
int BPF_PROG(test3, struct rq *rq, struct task_struct *curr, u64 now_time,
             int average_capacity, int total_cpus)
```

Iterates CPUs via `bpf_loop`. Preference order:
1. SCHED_IDLE CPUs that are not preempted and have been active longer than source
2. Any CPU with capacity > average or capacity > 500

Target is marked with `PRMPT_HELD_MASK` (BIT(2) of `prmpt_flags`) under
`my_spinlock` to prevent double-selection across concurrent IVH firings.

---

## Kernel-side lockholder classification (debugfs snapshot)

In `running_migration()` before the BPF call, the kernel also classifies
the task into `lhp_class` and writes it to `lhp_last_class` per-CPU:

```
curr_lockholder=0                 → LHP_NOT_LOCKHOLDER
curr_user_lockholder, movable     → LHP_USER_MOVABLE
curr_user_lockholder, !movable    → LHP_USER_NONMOVABLE
curr_kernel_lockholder, movable   → LHP_KERNEL_MOVABLE
curr_kernel_lockholder, !movable  → LHP_KERNEL_NONMOVABLE
```

Movability: `cpumask_weight(rq->curr->cpus_ptr) > 1` (same criterion as lhp_movable).

---

## Shadow-mode candidate rule

Runs inside `test()` **before** the IVH capacity/utilization gates (gates 4–8),
immediately after the idle check (gate 3).  Does **not** trigger migration.

```
curr_lockholder && movable
    → shadow candidate (counted, traced, not migrated)
IVH gates (cpu_capacity, last_preemption, idle_cpus, runtime, util) run after.
```

The condition intentionally has **no runtime threshold**.  The goal is to count
every lockholder tick on every CPU, independent of cpu_capacity or util_avg.
Runtime-threshold sweeps are a later step (see experiment_plan.md).

### Gate position

```
Gate 1:  preempt_migrate_locked        (skip if IVH already pending)
Gate 2:  h_nr_runnable − h_nr_idle > 1 (skip if CPU is busy)
Gate 3:  curr == rq->idle              (skip idle task)
Gate 4:  !curr_lockholder              (skip if not a lockholder)
── shadow candidate fires here ────────────────────────────────────
Gate 5:  cpu_capacity > 900            (IVH: skip unthrottled CPUs)
Gate 6:  last_preemption == 0          (IVH: skip if no steal time)
Gate 7:  idle_cpus == 0                (IVH: skip if nowhere to go)
Gate 8:  task_runtime < 1 ms          (IVH: task not running long)
Gate 9:  util_percent < 60             (IVH: not CPU-intensive)
Gate 10: !movable                      (IVH: task is pinned)
→ return 1 (trigger migration)
```

### Movability check

```c
unsigned long cbits = *(curr->cpus_ptr->bits);
int movable = cbits && (cbits & (cbits - 1));
```

`cbits & (cbits-1)` clears the lowest set bit; non-zero means more than one
allowed CPU.  Safe for systems with ≤ 64 CPUs (single-word cpumask).

### BPF maps

| Map | Type | Entries | Purpose |
|-----|------|---------|---------|
| `sched_in_map` | PERCPU_ARRAY | 1 | reserved for future runtime tracking |
| `lhp_counters` | PERCPU_ARRAY | 9 | class tick counts + candidate counts |
| `last_candidate_pid` | PERCPU_ARRAY | 1 | one-shot dedup: last candidate pid per CPU |

Counter indices in `lhp_counters`:

| Index | Constant | Meaning |
|-------|----------|---------|
| 0 | CTR_USER_MOVABLE | ticks seen: user lockholder, movable |
| 1 | CTR_USER_NONMOVABLE | ticks seen: user lockholder, pinned |
| 2 | CTR_KERNEL_MOVABLE | ticks seen: kernel lockholder, movable |
| 3 | CTR_KERNEL_NONMOVABLE | ticks seen: kernel lockholder, pinned |
| 4 | CTR_CANDIDATE_TOTAL | one-shot candidate events (total) |
| 5 | CTR_CAND_USER_MOV | one-shot: user movable candidates |
| 6 | CTR_CAND_USER_NMON | one-shot: user non-movable candidates |
| 7 | CTR_CAND_KERN_MOV | one-shot: kernel movable candidates |
| 8 | CTR_CAND_KERN_NMON | one-shot: kernel non-movable candidates |

Read with:
```bash
sudo bpftool map dump name lhp_counters
```
Sum per-CPU values to get system totals.

### Trace output

When a new candidate is detected (one-shot per PID per CPU epoch):
```
SHADOW_CAND cpu=N pid=P
```
Read from `/sys/kernel/debug/tracing/trace_pipe`.

### Threshold

No threshold in the current shadow candidate rule.  The `sched_in_map` is
retained for future use when a runtime-threshold sweep is added (see
experiment_plan.md).  To add a threshold back: restore `CANDIDATE_THRESHOLD_NS`
and the `sched_in_map` stamp-tracking code from git history, then rebuild with
`make MY_ivh_atc`.  No kernel change or reboot required.

---

## Edward's ivh_atc (reference)

`tools/bpf/ivh_atc.bpf.c` is the 6.1-era reference implementation.

Key differences from MY_ivh_atc:
- Does not dereference context args directly; obtains rq via `bpf_this_cpu_ptr(&runqueues)`.
- Predates the BPF_PROG macro requirement (6.17 context model change).
- Hook signatures and gate logic may differ; treat as reference, not ground truth.
