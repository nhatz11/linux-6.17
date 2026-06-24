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

## glibc Pthread Spinlock Patch — Userspace cr_counter Coverage

### What was patched

The kernel reads `cr_counter` from the userspace rseq struct at tick time to determine whether the running task is holding a pthread spinlock.  Without instrumentation in glibc, `cr_counter` is always zero for pthread spinlock holders and the `curr_user_lockholder` gate never fires for pthread workloads.

The following files in `/home/nick/glibc-build-src/glibc-2.41/` were patched so that every `pthread_spin_lock` acquisition increments `cr_counter` (via `__spin_extend()`) and every `pthread_spin_unlock` decrements it (via `__spin_unextend()`), plus writes timing fields to the rseq struct.

### Coverage: pthread spinlock functions

| Function | Coverage | Notes |
|----------|----------|-------|
| `pthread_spin_lock` | Fully instrumented | wait timing, extend, CS timestamps |
| `pthread_spin_unlock` | Fully instrumented | unextend, writes last_cs_overall/active/wait_ns |
| `pthread_spin_trylock` | Fully instrumented | `.S` asm deleted; C version patched with same `acquired:` block as lock.c |
| `pthread_spin_init` | Replaced with correct C (0=unlocked) | `.S` asm deleted |
| `pthread_spin_destroy` | No-op; no instrumentation needed | |

### trylock asm was doubly broken

`sysdeps/x86_64/nptl/pthread_spin_trylock.S` used the **old 1=unlocked** convention (`xorl %ecx,%ecx; xchgl %ecx,(%rdi); cmpl $1,%ecx`) and was never deleted when lock.S and unlock.S were deleted.  After the C implementations switched to 0=unlocked, the asm trylock would:
- Succeed when the lock was held (saw old value 0 = C-unlocked, but compared it as 1 = asm-unlocked → mismatch)
- Fail when the lock was free (saw old value 1 = C-locked, compared as 0 → mismatch)
- Also bypass all rseq instrumentation regardless

Fix: deleted the .S file so the C fallback (`nptl/pthread_spin_trylock.c`) takes over, then patched the C file to add the `__spin_extend()` / timestamp / depth tracking on the success path.

### Timing fields written to rseq struct

| Field | Offset | Writer | Meaning |
|-------|--------|--------|---------|
| `cr_counter` | 28 | lock/trylock (bits 2–31 = depth), kernel (bit 1) | lockholder depth + resched request |
| `wait_counter` | 32 | lock/trylock (enter/exit bracket) | nonzero while spinning; observable from another CPU via BPF |
| `last_cs_overall_ns` | 40 | unlock | wall time the lock was held (CLOCK_MONOTONIC delta) |
| `last_cs_active_ns` | 48 | unlock | CPU time the lock was held (CLOCK_THREAD_CPUTIME_ID delta) |
| `last_wait_overall_ns` | 56 | lock/trylock | wall time the caller spun before acquiring |

All fields are in the extended rseq area (`offsetof(struct rseq, cr_counter)` through `offsetof(struct rseq, end)`).  The kernel validates `rseq_len >= 64` for full-feature registrations.

### Verification

`/home/nick/pthread_extend_probe.c` reads these fields directly via `__rseq_offset` and prints per-run timing.  `/home/nick/verify_extend.sh` sweeps `rseq_sched_extend_usec` from 0 to 1000 and produces the following table (obtained 2026-06-18):

```
extend_usec      runs   avg_wait   max_wait   avg_cs   active  overhead  extended
-----------      ----   --------   --------   ------   ------  --------  --------
0               71073       720us     16027us      69us      61us        8us         0
50              78597       621us      8766us      63us      61us        2us       739
200             80839       597us      8215us      61us      61us        0us      1500
500             80669       596us     11112us      61us      61us        0us      1526
1000            80184       601us      8538us      61us      61us        0us      1533
```

`overhead` (= cs_overall − cs_active = preemption penalty) drops from 8 µs to 0 µs; `avg_wait` drops 17%; `extended > 0` only when `extend_usec > 0`.  All three signals confirm that `cr_counter` is being written by the patched glibc and read correctly by the kernel.

---

## Lock-acquisition-driven migration trigger

### Why lock-acquisition vs tick

The tick fires every 4 ms. A task that acquires a spinlock 3 ms into its slice and holds it for 1 ms is invisible to the tick — the tick has already fired cleanly. Moving the trigger to lock acquisition catches the CS at the moment it begins, when the remaining vCPU time estimate is freshest.

### bpf_sched_lock_acquire() — kernel/sched/bpf_sched.c:37

Called from `cs_enter()` in `kernel/locking/spinlock.c:44` when `lock_depth` transitions from 0 to 1 (outermost kernel spinlock acquired). The tick-path call to `running_migration()` in `sched_balance_trigger()` has been removed; only this path remains.

```c
void bpf_sched_lock_acquire(void)
{
    struct rq *rq;
    u64 now, elapsed, last_act, time_left, last_cs;

    if (!bpf_sched_enabled())          // Gate 1: static key — zero cost when IVH unloaded
        return;

    rq = this_rq();

    if (unlikely(rq->last_preemption == 0))   // Gate 2: no steal history
        return;

    last_cs = current->last_cs_ns;
    if (last_cs == 0)                  // Gate 3: no completed CS yet (boot)
        return;

    now      = sched_clock();
    elapsed  = now - rq->last_preemption;
    last_act = rq->last_active_time;
    if (elapsed >= last_act)           // Gate 4: model window expired — no evidence of imminent steal
        return;

    time_left = last_act - elapsed;
    if (time_left >= last_cs + MIGRATION_THRESHOLD_NS)   // Gate 5: safe zone
        return;

    running_migration(rq);             // danger zone: migrate now
}
```

### last_preemption and last_active_time — kernel/sched/cputime.c:268–275

Updated in `steal_account_process_time()` whenever paravirt steal > 1 ms is detected:

```c
if (steal > 1000000) {
    if (rq->last_preemption > rq->last_idle_tp)
        rq->last_active_time = now - rq->last_preemption - steal;
    else
        rq->last_active_time = now - rq->last_idle_tp - steal;
    rq->last_preemption = now;
}
```

`last_preemption`: timestamp (sched_clock) of the most recent hypervisor preemption.  
`last_active_time`: duration the vCPU was continuously active before that preemption.  
Together they model: "the vCPU ran for `last_active_time` ns, then got stolen at `last_preemption`." Time remaining = `last_active_time - (now - last_preemption)`.

### The elapsed fix (2026-06-18)

Original code: `time_left = (elapsed < last_act) ? (last_act - elapsed) : 0;`

When `elapsed >= last_act`, `time_left = 0`, so `0 < last_cs + 500µs` is always true → `running_migration()` fired on every lock acquisition once any steal history existed. Measured ~60–100% call rate under hackbench with IVH loaded.

Fix: `if (elapsed >= last_act) return;` added before the time_left calculation. The window-expired case gets an early exit because the model cannot predict imminent preemption in a new (potentially long) vCPU slice. Verified: call rate dropped to ~0.26% under the same workload.

### CS timing fields for last_cs_ns — include/linux/sched.h:1432–1447

`last_cs_ns` is a per-task field holding the wall-clock duration of the most recently completed outermost kernel spinlock CS. It provides the "how long will this task need the lock" estimate in `bpf_sched_lock_acquire()`.

Two timestamps cooperate to compute it:

| Field | Context-switch behavior | Purpose |
|-------|------------------------|---------|
| `cs_start_ts` | Reset by `prepare_task_switch`, resumed by `finish_task_switch` | On-CPU accumulation into `cumulative_cs_time` |
| `cs_wall_start_ts` | Never touched by context-switch handlers | Wall-clock anchor; survives preemption mid-CS |

At outermost acquire (`cs_enter()`, `spinlock.c:41–45`):
```c
current->cs_start_ts     = sched_clock();
current->cs_wall_start_ts = current->cs_start_ts;
bpf_sched_lock_acquire();
```

At outermost release (`cs_exit()`, `spinlock.c:48–56`):
```c
u64 now = sched_clock();
current->last_cs_ns        = now - current->cs_wall_start_ts;   // wall-clock
current->cumulative_cs_time += now - current->cs_start_ts;      // on-CPU only
current->cs_start_ts      = 0;
current->cs_wall_start_ts = 0;
```

`last_cs_ns` mirrors `rseq->last_cs_overall_ns` semantics (wall-clock) for kernel spinlocks. `cumulative_cs_time` mirrors `rseq->last_cs_active_ns` semantics (on-CPU only).

### Verification (2026-06-18)

- `bpf_sched_lock_acquire` symbol live in `/proc/kallsyms` ✓
- ftrace function_graph confirms: `bpf_sched_lock_acquire() { running_migration() { } }` nesting ✓
- `sched_balance_trigger` function_graph shows only `nohz_balance_exit_idle` + `raise_softirq` inside — zero `running_migration` calls from tick path ✓
- Under hackbench -g 32 with CPU saturation: 14 `ivh: final cpu=X target_cpu=Y` migrations completed ✓
- Elapsed fix: call ratio dropped from ~100% → 0.26% of lock acquisitions ✓

---

## Edward's ivh_atc (reference)

`tools/bpf/ivh_atc.bpf.c` is the 6.1-era reference implementation.

Key differences from MY_ivh_atc:
- Does not dereference context args directly; obtains rq via `bpf_this_cpu_ptr(&runqueues)`.
- Predates the BPF_PROG macro requirement (6.17 context model change).
- Hook signatures and gate logic may differ; treat as reference, not ground truth.
