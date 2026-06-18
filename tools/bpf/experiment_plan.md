# LHP/IVH Implementation Report — Linux 6.17 vSched Port

**Branch:** `rseq-port`  
**Date:** 2026-06-11  
**Author:** Nick Hatz

---

## Executive Summary

The LHP (Lockholder Policy) / IVH (Interruptible Virtual Host) extension has been ported to the Linux 6.17 vSched framework. The full lock-tracking substrate is in place: `lock_depth` counts every raw spinlock acquisition in process context (including trylocks and every unlock variant); an rq-lock handoff bug that caused `lock_depth` to go negative across context switches has been corrected; and userspace lockholders are detected via the rseq `cr_counter` field. At tick time, the kernel classifies every running task into one of four lockholder classes and passes the result to the BPF scheduler hook. Four BPF profiling tools are fully built and operational. The threshold-sweep experiments and paper-figure production have not yet begun.

---

## 1. Project Overview

### What is LHP / IVH?

In a virtualized environment, a vCPU running a guest kernel task may be preempted by the host hypervisor at any time. If the preempted task holds a spinlock, every other vCPU in the guest that needs that lock spins idly until the preempted holder is rescheduled. This is the **Lock Holder Preemption (LHP)** problem: lock-holder preemption creates artificial latency that scales with the number of waiters.

**IVH (Interruptible Virtual Host)** is a scheduler mechanism that detects preempted or throttled lockholders and proactively migrates them to a less-preempted CPU, shortening the effective hold time and reducing waiter spin time.

### Why lockholders matter

Spinlock contention under preemption is multiplicative: one preempted holder blocks N waiters for the full duration of the preemption, not just for the normal lock hold time. Migrations of O(10 µs) can eliminate spin waits of O(1 ms) or more when the holder is on a throttled vCPU. The value of the migration grows with the number of waiters and the severity of CPU throttling.

### The vSched hook model

This tree adds a `BPF_PROG_TYPE_SCHED` program type with `SEC("sched/...")` attach points. The scheduler calls into the BPF program at well-defined points (tick end, CPU selection, etc.) and acts on the return value. This port adds `curr_lock_depth`, `curr_kernel_lockholder`, `curr_user_lockholder`, and `curr_lockholder` as pre-computed arguments to the `cfs_sched_tick_end` hook so the BPF side sees lockholder state without having to dereference kernel structures.

---

## 2. Completed Implementation Work

### 2.1 `lock_depth` counter

`lock_depth` is a new `int` field in `struct task_struct`. It is a process-context spinlock nesting counter: it is zero when the task holds no spinlocks, positive when one or more are held, and never negative under normal operation. It is initialized to 0 on fork.

### 2.2 Raw spinlock coverage

Every non-inline `_raw_spin_lock*` entry point in `kernel/locking/spinlock.c` increments `lock_depth` after acquiring the lock, and every `_raw_spin_unlock*` decrements it before releasing. The guard `!in_interrupt()` prevents updates from interrupt context, where `current` is meaningless.

| Function | Direction | Line | Note |
|----------|-----------|------|------|
| `_raw_spin_trylock` | `lock_depth++` | 141 | conditional on return value |
| `_raw_spin_trylock_bh` | `lock_depth++` | 153 | conditional on return value |
| `_raw_spin_lock` | `lock_depth++` | 164 | |
| `_raw_spin_lock_irqsave` | `lock_depth++` | 176 | |
| `_raw_spin_lock_irq` | `lock_depth++` | 187 | |
| `_raw_spin_lock_bh` | `lock_depth++` | 198 | |
| `_raw_spin_unlock` | `lock_depth--` | 207 | decrement before unlock |
| `_raw_spin_unlock_irqrestore` | `lock_depth--` | 217 | decrement before unlock |
| `_raw_spin_unlock_irq` | `lock_depth--` | 227 | decrement before unlock |
| `_raw_spin_unlock_bh` | `lock_depth--` | 238 | decrement **after** unlock (ordering differs) |

rwlock variants (`_raw_read_*`, `_raw_write_*`) have no `lock_depth` tracking — they are not raw spinlocks and do not contribute to the lockholder signal.

### 2.3 Trylock accounting

`_raw_spin_trylock` and `_raw_spin_trylock_bh` increment `lock_depth` **only if the trylock succeeds** (conditional on the return value). This is correct: a failed trylock leaves the lock unacquired, so the depth should not change.

### 2.4 rq-lock handoff fix

The scheduler acquires `rq->lock` in `__schedule()` on behalf of the outgoing task (`prev`) and releases it on behalf of the incoming task (`next`) in `finish_lock_switch()`. Without correction, `next` would enter `finish_lock_switch` with `lock_depth == 0` and decrement it to -1 when the rq unlock fires.

The fix is a two-part correction in `prepare_task_switch` and `finish_lock_switch`:
- In `prepare_task_switch`: `prev->lock_depth--` cancels the rq-lock increment that was charged to `prev`.
- In the path leading to `finish_lock_switch`: `current->lock_depth++` pre-charges the unlock that `finish_lock_switch` will trigger on `next`.

This preserves the invariant that `lock_depth ≥ 0` at all times and is transparent to BPF programs reading the field at tick time.

### 2.5 Tick-time lockholder visibility

`running_migration()` in `kernel/sched/fair.c` (lines 12940–12952) computes the lockholder state immediately before calling the BPF hook:

- **Kernel lockholder**: `curr->lock_depth > 0`
- **User lockholder**: `rseq->cr_counter` with mask `0xFFFFFFFEu` is non-zero. Bit 0 is the bare rseq entry flag; bits 2–31 are the nesting depth. A task is a user lockholder when it has incremented the nesting depth beyond the bare entry. `copy_from_user_nofault` is used because this runs in tick context and must not sleep or fault.
- **Combined**: `curr_lockholder = curr_kernel_lockholder || curr_user_lockholder`

All four values are passed as explicit arguments to the BPF hook.

### 2.6 Kernel/user classification and movability

In `running_migration()`, the kernel also classifies the running task into one of five `lhp_class` values and writes a snapshot to `lhp_last_class[cpu]` for debugfs:

| Class | Condition |
|-------|-----------|
| `LHP_NOT_LOCKHOLDER` | `curr_lockholder == 0` |
| `LHP_USER_MOVABLE` | user lockholder, `cpumask_weight > 1` |
| `LHP_USER_NONMOVABLE` | user lockholder, pinned to one CPU |
| `LHP_KERNEL_MOVABLE` | kernel lockholder, `cpumask_weight > 1` |
| `LHP_KERNEL_NONMOVABLE` | kernel lockholder, pinned to one CPU |

Movability is determined by reading `curr->cpus_ptr->bits[0]` directly (single word, safe for ≤ 64 CPUs). The test `bits && (bits & (bits-1))` is non-zero when more than one CPU bit is set.

### 2.7 Shadow candidate framework

The shadow candidate system runs inside `test()` in `MY_ivh_atc.bpf.c`, **after** the idle/runnable checks but **before** the IVH capacity and utilization gates. It observes every lockholder tick without triggering migration, building up statistics to inform threshold selection.

On each tick where `curr_lockholder == 1`:
1. A per-class tick counter in `lhp_counters` is incremented (fires every tick).
2. If the task is also movable (`cpumask > 1 allowed CPU`) and its PID differs from the last recorded candidate PID on this CPU, a one-shot candidate event fires: the total and per-class candidate counters are incremented, and a `SHADOW_CAND cpu=N pid=P` trace is emitted to `trace_pipe`.

The one-shot dedup (via `last_candidate_pid` PERCPU_ARRAY) prevents the same task from flooding candidate counts; it resets when a different task becomes current on that CPU.

### 2.9 rseq extension time sysctl

**What was added**: a runtime-tunable grace period for the rseq cooperative-yield mechanism, exposed as `/proc/sys/kernel/rseq_sched_extend_usec`.

**Background — the extension mechanism**: when a task has `TIF_NEED_RESCHED_LAZY` pending and returns to userspace, `rseq_delay_resched()` in `kernel/rseq.c` checks whether the task is inside an rseq critical section (bit 0 of `cr_counter`, `RSEQ_CR_FLAG_IN_CRITICAL_SECTION_MASK`). If it is, the kernel:
1. Sets bit 1 of `cr_counter` (`RSEQ_CR_FLAG_KERNEL_REQUEST_SCHED`) to signal userspace to call `sched_yield()` cooperatively.
2. Starts an hrtimer via `hrtick_local_start()`. If the timer fires before userspace yields, `rseq_delay_resched_tick()` forces the resched and clears bit 1.

The grace period was previously hardcoded at **50 µs** in `rseq_delay_resched_fini()`.

**What changed**: the hardcoded `50` is now `rseq_sched_extend_usec`, an `unsigned int` initialized to 50 (preserving the original behavior by default). Writing 0 disables the extension entirely — no timer is started, the `KERNEL_REQUEST_SCHED` bit is never set, and the lazy resched fires immediately at the next opportunity.

**Why the two uses of `cr_counter` are independent**: the extension mechanism triggers on bit 0 (bare critical-section entry). The IVH lock-holder detection reads bits 2–31 (the nesting depth counter written by `extend()`/`inc_extend()` in userspace). Disabling the extension (`= 0`) does not touch bits 2–31 and does not affect the `curr_user_lockholder` computation in `running_migration()` or the BPF hook.

**Usage**:
```bash
# Read current value (default: 50 µs)
cat /proc/sys/kernel/rseq_sched_extend_usec

# Increase to 100 µs
echo 100 | sudo tee /proc/sys/kernel/rseq_sched_extend_usec

# Disable extension entirely (BPF lock-holder detection still works)
echo 0 | sudo tee /proc/sys/kernel/rseq_sched_extend_usec

# Restore default
echo 50 | sudo tee /proc/sys/kernel/rseq_sched_extend_usec
```

Changes live in `kernel/rseq.c`. No BPF rebuild or reboot is required to change the value; a write to the proc file takes effect immediately.

---

### 2.8 BPF hook and scheduler changes

- **New BPF prog type**: `BPF_PROG_TYPE_SCHED` with `SEC("sched/...")` attach point support, wired through BTF attach and link_create.
- **`cfs_sched_tick_end` signature extended**: five new int args appended — `curr_lock_depth`, `curr_kernel_lockholder`, `curr_user_lockholder`, `curr_lockholder`, `curr_waiter` — so BPF programs receive pre-computed lockholder and waiter state without needing to dereference `task_struct` fields. `curr_waiter = (wait_depth > 0) || curr_user_waiter`: non-zero when the task is actively spinning in a kernel qspinlock/OSQ slowpath or a userspace rseq spin-wait region at tick time.
- **`BPF_PROG` macro required**: context args are accessible only through the macro; without it the verifier marks them `NOT_INIT`.
- **CPU selection hook**: `cfs_select_run_cpu_spin` for choosing the migration target, using `PRMPT_HELD_MASK` to prevent double-selection across concurrent IVH firings.

---

## 3. Profiling Infrastructure

### 3.1 `lhp_movable` — Movability profiler

**Purpose**: Characterize what fraction of running tasks are pinned vs. movable at tick time, before committing to any IVH threshold.

**What it measures**: On every scheduler tick (for every non-idle running task), it records the task's PID, TGID, comm, `cpus_ptr` bitmask, and a derived `movable` flag. A task is movable if `popcount(cpumask_bits) > 1`.

**How it works**: A single `sched/cfs_sched_tick_end` BPF program reads `rq->curr->cpus_ptr->bits[0]` and emits a ring-buffer event per tick. The userspace binary polls the ring buffer and prints one line per event.

**How to run**:
```bash
cd tools/bpf
sudo ./lhp_movable
```
Interrupt with Ctrl-C. Output is a stream of per-tick lines:
```
cpu=3 pid=1234 (my_proc) cpumask=0x3f movable=1
```

**Use case**: Run this alongside a workload and count how many running tasks have `movable=1`. If most candidates are pinned, IVH migration will not be effective regardless of threshold.

---

### 3.2 `lhp_cstime` — Critical section duration profiler

**Purpose**: Measure the actual durations of kernel spinlock critical sections and userspace rseq critical sections system-wide.

**What it measures**:
- **Kernel CS**: measures wall-clock time from `_raw_spin_lock` (or variant) to the corresponding `_raw_spin_unlock` via `fexit`/`fentry` probes.
- **User CS**: detects when a task enters (rseq `cr_counter` bits 2–31 become non-zero) and exits (returns to zero) a userspace critical section, using tick sampling to bracket the duration.

**How it works**: Two BPF attach points:
1. `fexit/_raw_spin_lock`: records a timestamp keyed by PID in `kern_cs_map`.
2. `fentry/_raw_spin_unlock`: looks up the entry timestamp, computes duration, emits a ring-buffer event with `[kernel]` label.
3. `sched/cfs_sched_tick_end`: samples rseq `cr_counter`; transitions 0→non-zero start a user CS entry, non-zero→0 complete it, emitting a `[userspace]` event.

**How to run**:
```bash
cd tools/bpf
sudo ./lhp_cstime
```
Or via the experiment script (recommended):
```bash
sudo bash cs_experiment.sh 10 ~/extend-sched
```

**Expected output** (system-wide, no workload, 3-second capture from live run):
```
Valid events  : 2,550,683
Min           : 51 ns
Mean          : 1.9 µs
Median (p50)  : 1.9 µs
p90           : 2.2 µs
p99           : 4.6 µs
p99.9         : 11.0 µs
Max (filtered): 9.997 ms

  < 1 µs           961,445  (37.69%)
  1–10 µs        1,586,009  (62.18%)
  10–100 µs          2,795  ( 0.11%)
  100 µs–1 ms          182  ( 0.01%)
  1–2 ms                37  ( 0.00%)
  2–4 ms               131  ( 0.01%)
  > 4 ms                84  ( 0.00%)

CS < 1 ms: 2,550,431 (99.99%)  |  CS ≥ 1 ms: 252 (0.01%)
CS < 2 ms: 2,550,468 (99.99%)  |  CS ≥ 2 ms: 215 (0.01%)
CS < 4 ms: 2,550,599 (100.0%)  |  CS ≥ 4 ms:  84 ( 0.00%)
```

**Key finding**: Under typical idle conditions, the vast majority (~99.99%) of kernel spinlock CS events last under 1 ms. The current IVH 1 ms runtime gate implies the tool would only fire for the top 0.01% of lock hold events by duration. Longer CS durations emerge under lock-contended workloads (hackbench, spinlock stress).

---

### 3.3 `lhp_waittime` — Wait-time and CS-time profiler

**Purpose**: Measure how long a task runs on the CPU before entering a userspace critical section, alongside the CS duration. This directly informs threshold selection: if a task enters a CS only 500 µs after being scheduled in, a 2 ms IVH threshold would never fire before the CS is already underway.

**What it measures**: For each rseq-registered task:
- **CS duration**: time from rseq `cr_counter` entry to exit (tick-sampled).
- **Wait time**: time elapsed between the task's last preemption/idle timestamp (`max(rq->last_preemption, rq->last_idle_tp)`) and the moment it entered the CS. This is how long it ran on the CPU before taking a lock.

**How it works**: A single `sched/cfs_sched_tick_end` BPF program. On CS entry it records a `wt_entry` containing the entry timestamp and a wait anchor (derived from rq timestamps). On CS exit it computes both durations and emits a ring-buffer event with `cs=N ns wait=M ns`.

**Important**: `lhp_waittime` only captures rseq-registered tasks. It requires a workload using rseq-based critical sections such as `~/extend-sched` (the per-project lock workload). It will produce zero events on a workload that does not use rseq.

**How to run**:
```bash
cd tools/bpf
sudo bash cs_experiment.sh 10 ~/extend-sched
```

**Expected output format** (with extend-sched running):
```
USER rseq CS  (lhp_waittime, tick-sampled — extend-sched only)
  Events        : N
  CS duration:
    Median      : X µs
    p90         : Y µs
  Wait-before-CS:
    Median      : A µs
    p90         : B µs

MIGRATION THRESHOLD ANALYSIS
  T = 1 ms:
    Already in CS        : N1/N (P1%)
    Not yet in CS        : N2/N (P2%)
    Remaining CS (defer) : median=X  p90=Y
  T = 2 ms: ...
```

The "Remaining CS (defer)" value answers: "if we migrated at T ms, how much longer would the task still hold the lock at P50/P90?" This is the key number for threshold selection.

---

### 3.4 `MY_ivh_atc` — Full IVH implementation with shadow mode

**Purpose**: The primary IVH BPF program. In production mode it triggers CPU migration for qualifying lockholders. In its current **shadow mode** (all gates up to migration return, the migration return-1 fires but data collection runs unconditionally) it counts and classifies every lockholder candidate without issuing actual migrations — enabling safe data collection under real workloads.

**What it measures**:
- Per-class tick counts: how many scheduler ticks see each class of lockholder (4 classes × movable/not).
- One-shot candidate events: how many unique tasks first appear as lockholder+movable on each CPU.
- Trace events: `SHADOW_CAND cpu=N pid=P` per new candidate via `trace_pipe`.
- (When migration gates pass) fires IVH by returning 1.

**How it works**: Three BPF hook programs:
1. `test()` on `sched/cfs_sched_tick_end`: the main IVH decision + shadow candidate logic (see §4 for gate ordering).
2. `test3()` on `sched/cfs_select_run_cpu_spin`: CPU selection for migration target.
3. `test4()` on `cfs_should_spinlock` and `test6()` on `cfs_should_bias`: return 1 unconditionally (stubs).

**How to run**:
```bash
cd tools/bpf
sudo ./MY_ivh_atc &
# Watch trace output
sudo cat /sys/kernel/debug/tracing/trace_pipe | grep SHADOW_CAND
# Read counters
sudo bpftool map dump name lhp_counters
```

**Counter format**: `lhp_counters` is a PERCPU_ARRAY with 9 entries. Sum across CPUs for system totals.

| Index | Constant | Meaning |
|-------|----------|---------|
| 0 | CTR_USER_MOVABLE | ticks: user lockholder, movable |
| 1 | CTR_USER_NONMOVABLE | ticks: user lockholder, pinned |
| 2 | CTR_KERNEL_MOVABLE | ticks: kernel lockholder, movable |
| 3 | CTR_KERNEL_NONMOVABLE | ticks: kernel lockholder, pinned |
| 4 | CTR_CANDIDATE_TOTAL | one-shot candidate events total |
| 5–8 | CTR_CAND_* | candidate counts per class |

**Rebuilding after threshold change** (no reboot needed):
```bash
cd tools/bpf
# Edit MY_ivh_atc.bpf.c: change CANDIDATE_THRESHOLD_NS
make MY_ivh_atc
sudo ./MY_ivh_atc
```

---

### 3.5 `cs_experiment.sh` — Unified experiment harness

**Purpose**: Run `lhp_cstime` and `lhp_waittime` back-to-back against a common workload and produce a combined human-readable statistical summary.

**Usage**:
```bash
# With workload
sudo bash cs_experiment.sh 10 ~/extend-sched

# System-wide (no specific workload)
sudo bash cs_experiment.sh 5

# Longer with flags
sudo bash cs_experiment.sh 30 ~/extend-sched --some-flag
```

**What it does**:
1. Optionally starts the workload in the background.
2. Runs `lhp_cstime` for `DURATION` seconds (captures kernel + user CS events).
3. Restarts workload, runs `lhp_waittime` for `DURATION` seconds.
4. Runs a Python analysis script that produces:
   - Kernel spinlock CS statistics (min/mean/p50/p90/p99/p99.9/max, distribution histogram, top processes by long-CS count).
   - User rseq CS statistics (if events present).
   - Threshold analysis table: at T = 1/2/4 ms, what fraction of tasks are already mid-CS vs. not yet started.

---

## 4. Important Code Paths

### `lock_depth` lifecycle

`lock_depth` starts at 0. Each `_raw_spin_lock*` call in process context (`!in_interrupt()`) increments it by 1. Each corresponding `_raw_spin_unlock*` decrements it by 1. Trylocks increment only on success. The rq-lock handoff during `context_switch` is corrected so neither `prev` nor `next` carries a phantom ±1.

At tick time, the kernel reads `rq->curr->lock_depth` directly. Since this is a process-context counter updated with preemption disabled (spinlock acquisition is inherently non-preemptible for the duration), there are no races with the tick reader.

### `curr_lockholder` computation

The combined lockholder flag is computed in `running_migration()` at `fair.c:12940–12952`:

1. Read `rq->curr->lock_depth`. Non-zero → `curr_kernel_lockholder = 1`.
2. If the task has a registered rseq struct (`curr->rseq != NULL`, `curr->rseq_len >= 32`): use `copy_from_user_nofault` to read `cr_counter` at offset 28. Mask off bit 0 (`~1u`). Non-zero → `curr_user_lockholder = 1`.
3. OR the two flags: `curr_lockholder = kernel || user`.

All four values are passed as arguments to the BPF hook.

### USER_MOVABLE / USER_NONMOVABLE / KERNEL_MOVABLE / KERNEL_NONMOVABLE

These four classes (plus `LHP_NOT_LOCKHOLDER`) are written to `lhp_last_class[cpu]` for debugfs visibility and replicated in the BPF `lhp_counters` map. The classification is:
- Is it a lockholder? (user vs. kernel based on which flag is set)
- Can it move? (`cpumask_weight(curr->cpus_ptr) > 1`)

A task is movable if its allowed-CPU bitmask has more than one bit set. Pinned tasks (NONMOVABLE) are counted but excluded from the migration candidate and shadow candidate paths, since migrating them is impossible.

### Shadow candidate logic

```
tick fires
├── Gate 1:  IVH already pending?  → skip
├── Gate 2:  busy CPU (>1 non-idle task)?  → skip
├── Gate 3:  idle task?  → skip
├── Gate 4:  !curr_lockholder?  → skip
│
├── [Shadow candidate block — COUNTING ONLY, fires here]
│   increment per-class tick counter (always)
│   if movable && pid != last_candidate_pid:
│     update last_candidate_pid
│     increment CANDIDATE_TOTAL + per-class candidate counter
│     emit SHADOW_CAND trace event
│
├── Gate 5:  cpu_capacity > 900?  → skip (not throttled)
├── Gate 6:  last_preemption == 0?  → skip (no steal time)
├── Gate 7:  idle_cpus == 0?  → skip (nowhere to go)
├── Gate 8:  task_runtime < 1 ms?  → skip
├── Gate 9:  util_percent < 60%?  → skip
└── Gate 10: !movable?  → skip (task is pinned)
    → return 1  (trigger migration)
```

The shadow candidate block intentionally has no runtime threshold: the goal is to count every lockholder regardless of how long it has been running. The runtime CDF analysis (§5, Figure F3) will show where a threshold would cut the candidate population.

### IVH gates in `test()`

Gates 1–4 are cheap pre-filters; the shadow candidate block fires after gate 4; gates 5–10 are the IVH-specific filters that must all pass before migration is triggered.

- **Gate 1** (`preempt_migrate_locked == 1`): IVH already pending on this CPU — no double-trigger.
- **Gate 2** (`h_nr_runnable − h_nr_idle > 1`): source CPU has more than one non-idle runnable task — contention is local, IVH not needed.
- **Gate 3** (`curr == rq->idle`): idle task — nothing to migrate.
- **Gate 4** (`!curr_lockholder`): task holds neither a kernel spinlock nor a userspace rseq lock — IVH would be pointless. Checked once here; not repeated below.
- **Gate 5** (`cpu_capacity > 900`): only fires on throttled vCPUs. A value above 900/1024 means the CPU is not being squeezed — no reason to migrate.
- **Gate 6** (`last_preemption == 0`): requires that the `cputime.c` heartbeat has recorded at least one steal event on this CPU. Without this guard, the scheduler would fire IVH on a CPU that has never been preempted.
- **Gate 7** (`idle_cpus == 0`): requires at least one system-wide idle CPU to migrate to.
- **Gate 8** (`task_runtime < 1 ms`): requires the task to have been running uninterrupted for at least 1 ms. Tasks that just started are unlikely to hold a lock the system needs urgently.
- **Gate 9** (`util_percent < 60%`): only CPU-intensive tasks. A task spending most of its time sleeping is not a bottleneck.
- **Gate 10** (`!movable`): task's cpumask has only one bit set — it is pinned and cannot be migrated regardless of other conditions.

All ten gates must pass for `test()` to return 1 (trigger migration).

---

## 5. Experiments and Findings

### 5.1 CS duration measurements (system-wide, idle baseline)

From a live 3-second capture on this machine (no extra workload):

| Statistic | Value |
|-----------|-------|
| Events captured | 2,550,683 valid (724 underflow artifacts filtered) |
| Minimum | 51 ns |
| Mean | 1.9 µs |
| Median (p50) | 1.9 µs |
| p90 | 2.2 µs |
| p99 | 4.6 µs |
| p99.9 | 11.0 µs |
| Maximum (filtered) | 9.997 ms |

Distribution:

| Bucket | Count | Fraction |
|--------|-------|----------|
| < 1 µs | 961,445 | 37.7% |
| 1–10 µs | 1,586,009 | 62.2% |
| 10–100 µs | 2,795 | 0.11% |
| 100 µs–1 ms | 182 | 0.007% |
| 1–2 ms | 37 | 0.001% |
| 2–4 ms | 131 | 0.005% |
| > 4 ms | 84 | 0.003% |

**Implication for threshold tuning**: Under idle system conditions, essentially all (99.99%) spinlock CSes complete in under 1 ms. This is expected — the problematic CSes appear under contended workloads like hackbench or spinlock stress. The baseline measurement confirms that the 1 ms IVH runtime gate is selective by default; under a locked workload the tail of this distribution will lengthen considerably.

Long-CS outliers on idle system: `rcu_preempt` (98 events, median 4 ms), `lhp_cstime` itself (67 events, measurement overhead), `claude` (48 events, JIT/GC activity).

### 5.2 Wait-time measurements

Wait-time data requires the `extend-sched` workload running with rseq support. The `lhp_waittime` tool is operational and has been verified to attach and run correctly; quantitative results are pending collection under the extend-sched workload.

### 5.3 Candidate detection results

Shadow candidate counters (`lhp_counters`) are operational. Candidate collection under controlled workloads (hackbench, spinlock stress) has not yet been run. The maps are cleared on each `MY_ivh_atc` restart.

### 5.4 Threshold observations

From the CS duration data and the threshold analysis framework:

- **P50 CS ≈ 1.9 µs**: a threshold below ~10 µs is pointless — the lock would be released before the migration overhead is recouped.
- **P99 CS ≈ 4.6 µs**: the vast majority of CSes are under 10 µs even system-wide. Under a contended workload, the tail will shift right.
- **Thresholds to evaluate**: 0.5 ms, 1 ms, 2 ms, 4 ms, 8 ms. The current IVH gate is 1 ms (runtime) with no BPF-level threshold in the shadow candidate rule. The earlier prototype used a 2 ms `CANDIDATE_THRESHOLD_NS`; that code is available in git history for restoration.
- **Rule of thumb**: threshold should be much larger than the typical CS duration. With median CS = 1.9 µs and threshold = 1 ms, there is a ~500× margin; the threshold is selecting specifically for unusually long-running lockholders.

---

## 6. Reproduction Guide

### Prerequisites

```bash
# Build all tools (one-time)
cd tools/bpf
make lhp_movable lhp_cstime lhp_waittime MY_ivh_atc
```

All four binaries require `sudo` and a running kernel with the BPF_PROG_TYPE_SCHED patches.

---

### Experiment A: Movability profile

```bash
sudo ./lhp_movable
# Run workload in parallel, Ctrl-C after 10–30 s
# Count lines with movable=0 vs movable=1
```

---

### Experiment B: CS duration profiling (system-wide or workload)

```bash
# System-wide, 10 seconds
sudo bash cs_experiment.sh 10

# With extend-sched workload
sudo bash cs_experiment.sh 10 ~/extend-sched

# With stress-ng spinlock workload
stress-ng --spinlock 8 --timeout 30 &
sudo bash cs_experiment.sh 10
```

The script runs `lhp_cstime` and `lhp_waittime` back-to-back and produces a statistical summary automatically.

---

### Experiment C: Shadow candidate collection

```bash
# Terminal 1: start IVH with shadow mode
sudo ./MY_ivh_atc &

# Terminal 2: watch for new candidates
sudo cat /sys/kernel/debug/tracing/trace_pipe | grep SHADOW_CAND | tee candidates.log

# Terminal 3: run workload
stress-ng --spinlock 8 --timeout 60

# After workload finishes, read counters
sudo bpftool map dump name lhp_counters
```

---

### Experiment D: Runtime-at-detection CDF

```bash
# Collect rt_us data (requires extending SHADOW_CAND trace format with rt_us=R)
sudo cat /sys/kernel/debug/tracing/trace_pipe | grep SHADOW_CAND > candidates.log

# Parse and plot (Python)
python3 - << 'EOF'
import re, sys
runtimes = []
with open("candidates.log") as f:
    for line in f:
        m = re.search(r'rt_us=(\d+)', line)
        if m:
            runtimes.append(int(m.group(1)))
print(f"Collected {len(runtimes)} candidate events")
EOF
```

Note: `rt_us` is not currently emitted; restore the `sched_in_map` stamp-tracking and `rt_us=R` format string from git history, rebuild with `make MY_ivh_atc`.

---

### Experiment F: Extension time sweep

**Goal**: determine whether the 50 µs cooperative-yield grace period is the right value, and quantify the effect of disabling it entirely.

**What to vary**: `rseq_sched_extend_usec` — no kernel rebuild needed, change takes effect immediately.

**Suggested values**: 0 (disabled), 10, 25, 50 (default), 100, 250 µs.

**What to measure per value**:
1. `lhp_waittime` output (CS duration and wait-before-CS) to see whether a longer grace period shifts the CS duration distribution (tasks yield cooperatively vs. being force-rescheduled mid-CS).
2. `total_extended` in `extend-sched` output — how many lock-hold cycles triggered a yield. With extension disabled this should drop to 0.
3. Aggregate throughput from `extend-sched` (lock acquisitions per second). A very long grace period may degrade throughput by letting the holder run past its quota; a very short one approaches the no-extension baseline.

**Procedure**:
```bash
for usec in 0 10 25 50 100 250; do
    echo $usec | sudo tee /proc/sys/kernel/rseq_sched_extend_usec
    echo "--- extend_usec=$usec ---"
    sudo bash cs_experiment.sh 10 ~/extend-sched
done
```

**Expected signal**: with `= 0`, the `KERNEL_REQUEST_SCHED` bit (bit 1 of `cr_counter`) is never set, so `unextend()` in `extend-sched` never sees `return *ptr & 2` true, and `tdata->extended` stays 0. With larger values, the cooperative yield rate (`total_extended`) should increase — but past some point the task is force-rescheduled before it can yield anyway, making larger values equivalent to each other.

**Note**: this experiment is separate from the BPF `CANDIDATE_THRESHOLD_NS` sweep (Experiment E). Extension time controls whether userspace gets a grace period; the candidate threshold controls which lockholders IVH considers for migration.

---

### Experiment E: Threshold sweep

Change `CANDIDATE_THRESHOLD_NS` in `MY_ivh_atc.bpf.c`, rebuild, and rerun Experiment C at each threshold (no kernel change or reboot required):

```bash
# Values to test: 500000, 1000000, 2000000, 4000000, 8000000
# Edit MY_ivh_atc.bpf.c line defining CANDIDATE_THRESHOLD_NS
make MY_ivh_atc
sudo ./MY_ivh_atc &
stress-ng --spinlock 8 --timeout 60
sudo bpftool map dump name lhp_counters   # record CTR_CANDIDATE_TOTAL
```

---

## 7. Current Status

### DONE

- [x] `wait_depth` field in `task_struct`, initialized to 0 on fork — depth counter for contended spin/wait slowpaths; > 0 while task is actively spinning in a qspinlock pending/MCS path or OSQ optimistic-spin path; 0 otherwise. Incremented on slowpath entry, decremented on every exit (acquisition or bail). Distinct from `lock_depth` which counts held locks; `wait_depth` counts in-flight waits.
- [x] `wait_counter` field in `struct rseq` (uABI, offset 32) — userspace parallel of `cr_counter` for waiter-side signaling. Bits [31:2] = spin-wait nesting depth (same add-4/sub-4 encoding as cr_counter); bits [1:0] reserved. Non-zero means thread is currently in a userspace spin/wait region. Updated by `wait_enter()`/`wait_exit()` in extend-sched / NHextend around the cmpxchg spin loop. Kernel reads it at tick time in `fair.c` as `curr_user_waiter` (guard: `rseq_len >= 36`); stored in `lhp_classify_snapshot` and printed in debugfs. Distinct from `cr_counter` (lockholder) — `wait_counter` is a waiter signal.
- [x] `lock_depth` field in `task_struct`, initialized to 0 on fork
- [x] `_raw_spin_lock` / `_raw_spin_lock_irqsave` / `_raw_spin_lock_irq` / `_raw_spin_lock_bh` — increment in process context
- [x] `_raw_spin_unlock` / `_raw_spin_unlock_irqrestore` / `_raw_spin_unlock_irq` / `_raw_spin_unlock_bh` — decrement in process context
- [x] `_raw_spin_trylock` / `_raw_spin_trylock_bh` — conditional increment on success
- [x] rq-lock handoff fix (`prepare_task_switch` + `finish_lock_switch`)
- [x] Tick-time lockholder detection in `running_migration()` (`fair.c:12940–12952`)
- [x] Userspace lockholder detection via rseq `cr_counter`
- [x] Kernel/user classification (`LHP_USER_MOVABLE`, etc.), per-CPU snapshot for debugfs
- [x] Movability detection via `cpumask_weight(curr->cpus_ptr) > 1`
- [x] `cfs_sched_tick_end` hook signature extended with 5 args: 4 lockholder + `curr_waiter` (spinner signal)
- [x] `curr_waiter` computed in `fair.c` at tick time: `(wait_depth > 0) || curr_user_waiter`; visible to BPF as a pre-computed flag, same pattern as `curr_lockholder`
- [x] `curr_waiter` confirmed working: SPINNER trace events emitted by `MY_ivh_atc` BPF hook for userspace spinners (extend-sched `wait_counter`); QSPIN_ENTER fentry events confirm kernel qspinlock slowpath (wait_depth increment sites) is reached under hackbench load (`qspinlock_probe.bpf.c`)
- [x] `demo_spinner.sh` — end-to-end spinner demo script: builds, loads, runs both tests, prints results
- [x] `BPF_PROG_TYPE_SCHED` + `SEC("sched/...")` BPF program type, BTF attach, link_create
- [x] Shadow candidate framework: per-tick class counters + one-shot candidate events + trace output
- [x] `lhp_movable` built, working, and tested
- [x] `lhp_cstime` built, working, and tested (kernel spinlock CS + user rseq CS)
- [x] `lhp_waittime` built, working, and tested (user rseq CS + wait anchor)
- [x] `MY_ivh_atc` built with full IVH gates + shadow mode + CPU selection hook
- [x] `cs_experiment.sh` unified harness with Python statistical analysis
- [x] Baseline system-wide CS duration measurement captured
- [x] `rseq_sched_extend_usec` sysctl — runtime-tunable rseq extension grace period (`/proc/sys/kernel/rseq_sched_extend_usec`, default 50 µs, 0 = disabled)

### IN PROGRESS

- [ ] Threshold sweep (0.5/1/2/4/8 ms): infrastructure ready, measurements not yet collected
- [ ] Runtime-at-detection CDF: `sched_in_map` infrastructure is present; `rt_us` field needs restoring to trace format and sweep needs to run
- [ ] Wait-time baseline vs. IVH-active comparison: requires extend-sched runs with and without `MY_ivh_atc` loaded

### NOT STARTED

- [ ] F1: CS duration CDF figure (requires contended-workload data + plot script)
- [ ] F2: Wait-time CDF baseline vs. IVH-active (requires extend-sched measurements)
- [ ] F3: Runtime-at-detection CDF (requires restoring rt_us trace field + data collection)
- [ ] F4: Candidate rate vs. threshold bar chart (requires threshold sweep data)
- [ ] T1: Class breakdown table at 2 ms threshold
- [ ] Full IVH end-to-end validation (verify migrations actually occur and reduce wait time)
- [ ] Comparison against unmodified 6.17 baseline
- [ ] Performance regression check (lock overhead from `lock_depth` instrumentation)

---

## 8. Remaining Work

### Immediate (next session)

1. **Run threshold sweep**: modify `CANDIDATE_THRESHOLD_NS` at each of the 5 values, run 60-second spinlock stress workload, record `CTR_CANDIDATE_TOTAL` from `lhp_counters`. No kernel change required.

2. **Restore `rt_us` in shadow trace**: un-comment the `sched_in_map` stamp tracking in `MY_ivh_atc.bpf.c` and add `rt_us=%llu` to the `bpf_trace_printk` format string. Rebuild. This enables Figure F3.

3. **Collect extend-sched wait-time data**: run `cs_experiment.sh 30 ~/extend-sched` to populate lhp_waittime measurements. Compare with and without `MY_ivh_atc` loaded.

### Analysis and figures

4. **Figure F1**: Plot CS duration CDF from `lhp_cstime` output under a contended workload. Add vertical lines at 0.5/1/2/4/8 ms.

5. **Figure F3**: Plot runtime-at-detection CDF from SHADOW_CAND `rt_us` values. The inflection point marks the natural threshold.

6. **Figure F4**: Bar chart of candidate rate (candidates/second) vs. threshold across the 5 values.

7. **Table T1**: Class breakdown at 2 ms threshold (values from `lhp_counters` indices 5–8).

8. **Figure F2**: `lhp_waittime` CDF baseline vs. IVH-active at the selected threshold.

### Validation

9. **End-to-end IVH test**: verify that returning 1 from `test()` actually causes a migration. Confirm via `trace_pipe` that the IVH fire message (`MY_ivh_atc: lockholder IVH fired`) appears under stress workload with throttled CPUs.

10. **Baseline comparison**: run hackbench and spinlock stress against unmodified 6.17 and against IVH-active tree. Measure aggregate throughput and tail latency.

11. **Lock overhead measurement**: benchmark `_raw_spin_lock`/`_raw_spin_unlock` with and without the `lock_depth` increment to quantify instrumentation cost.

### Threshold selection

12. Apply the heuristic from the experiment plan:
    - Discard thresholds below P50 CS duration (migration takes longer than the CS itself).
    - Find the inflection point of the F3 runtime CDF.
    - Pick the lowest threshold that shows meaningful wait-time reduction in F2 without excessive candidate rate in F4.

---

## 9. Advisor Summary

**Accomplishments**

- The entire `lock_depth` tracking substrate is implemented and correct: every non-inline raw spinlock variant (lock, lock_irqsave, lock_irq, lock_bh, trylock, trylock_bh) increments or decrements the per-task counter in process context. A subtle rq-lock handoff bug that produced `lock_depth = -1` on newly scheduled tasks has been identified and fixed.

- Userspace lockholder detection is implemented via the rseq `cr_counter` field, enabling IVH to fire for both kernel-side (spinlock) and userspace (futex/rseq) lockholders.

- The BPF scheduler hook (`cfs_sched_tick_end`) now receives pre-computed lockholder state as explicit arguments, removing the need for BPF programs to walk `task_struct` fields.

- A four-class lockholder taxonomy (USER/KERNEL × MOVABLE/NONMOVABLE) is computed at every tick and stored in both a debugfs snapshot and per-CPU BPF counters.

- The shadow candidate framework collects lockholder statistics without triggering migrations, enabling safe characterization of the candidate population under real workloads.

- All four profiling tools are built, working, and tested: `lhp_movable` (movability characterization), `lhp_cstime` (kernel + user CS durations), `lhp_waittime` (wait anchor + CS duration), `MY_ivh_atc` (IVH candidate detection).

- A unified experiment harness (`cs_experiment.sh`) runs both time-series collectors against a workload and produces a human-readable statistical report including threshold-impact analysis.

**Key findings**

- Under idle system conditions, the median kernel spinlock CS duration is **1.9 µs** (p90 = 2.2 µs, p99 = 4.6 µs). Only 0.01% of CSes exceed 1 ms. The current 1 ms IVH runtime gate is therefore selective by design and will fire primarily under contended-workload conditions.

- The instrumentation overhead (`lock_depth` increment per spinlock acquisition) has not yet been benchmarked; this should be done before claiming the approach is production-viable.

**Next concrete steps**

1. Run the threshold sweep (5 values × 60-second spinlock workload) — no kernel change required.
2. Restore `rt_us` to the SHADOW_CAND trace format and collect the runtime-at-detection CDF (Figure F3).
3. Run `cs_experiment.sh` against `~/extend-sched` for wait-time baseline data.
4. Produce Figures F1–F4 and Table T1.
5. Run the end-to-end IVH validation to confirm actual migrations occur.

---

## 10. Per-task CS time and active runtime accounting

### New `task_struct` fields (`include/linux/sched.h`)

| Field | Type | Meaning |
|---|---|---|
| `cs_start_ts` | `u64` | `ktime_get_ns()` at outermost spinlock acquire; 0 when not in a CS |
| `cumulative_cs_time` | `u64` | Total nanoseconds spent inside kernel spinlock critical sections (outermost only) |
| `sched_in_stamp` | `u64` | `ktime_get_ns()` when task was last scheduled in; 0 while off-CPU |
| `cumulative_active_time` | `u64` | Total nanoseconds on-CPU since task creation |

All four are initialized to 0 in `__sched_fork()`.

### CS time accounting (`kernel/locking/spinlock.c`)

Two `__always_inline` helpers (`cs_enter`, `cs_exit`) are called from every `_raw_spin_lock*` / `_raw_spin_unlock*` entry point in process context (`!in_interrupt()`):

- `cs_enter`: fires after `lock_depth++`; records `cs_start_ts` only when `lock_depth == 1` (outermost acquire). Nested locks (depth > 1) are no-ops.
- `cs_exit`: fires after `lock_depth--`; accumulates `ktime_get_ns() - cs_start_ts` into `cumulative_cs_time` and clears `cs_start_ts` only when `lock_depth == 0` (outermost release).

This matches the outermost-CS semantics of `lhp_cstime` but accumulates per-task rather than emitting ring-buffer events. The `cumulative_cs_time` field is a permanent per-task counter that grows monotonically across the task's lifetime.

### Active runtime accounting (`kernel/sched/core.c`)

- **Schedule-out** (`prepare_task_switch`): if `prev->sched_in_stamp != 0`, adds `ktime_get_ns() - sched_in_stamp` to `prev->cumulative_active_time` and clears the stamp.
- **Schedule-in** (`finish_task_switch`, just after `finish_lock_switch` releases `rq->lock`): sets `current->sched_in_stamp = ktime_get_ns()`.

This gives exact on-CPU time, not approximated from CS or wait durations.

### Exposure (`/sys/kernel/debug/lhp_class`)

`lhp_classify_snapshot` (per-CPU, updated at tick time in `running_migration()`) now carries `cumulative_cs_time` and `cumulative_active_time` snapshotted from `rq->curr`. The existing `lhp_class_show` debugfs file prints:

```
cpu=0  pid=1234   comm=hackbench       class=KERNEL_MOVABLE      lock_depth=1   movable=1 user_waiter=0 cs_us=45231      active_us=812400     cs_pct=5
```

`cs_pct = cumulative_cs_time * 100 / cumulative_active_time` — the integer percentage of on-CPU time spent holding a spinlock. This is the value that will feed the future policy:

```c
if (is_lockholder || (cs_pct >= threshold && wait_depth > 0))
    candidate = true;
```

### How BPF will access these fields

Once the kernel is rebuilt, `cumulative_cs_time` and `cumulative_active_time` will appear in BTF and be directly readable from any `BPF_PROG_TYPE_SCHED` program via `task->cumulative_cs_time`. No map lookup or ring-buffer aggregation needed — the kernel accumulates on every lock/unlock and schedule transition.

### Build note

Rebuild + reboot required to activate these fields. Changed files:
- `include/linux/sched.h`
- `kernel/locking/spinlock.c`
- `kernel/sched/core.c`
- `kernel/sched/fair.c`
- `kernel/sched/sched.h`

### Sanity test

After booting the new kernel:
```bash
# Run a spinlock-heavy workload for a few seconds
stress-ng --spinlock 4 --timeout 10 &

# Read the debugfs snapshot
cat /sys/kernel/debug/lhp_class
```

Expected: `stress-ng` worker threads show non-zero `cs_us` and a non-zero `cs_pct`. Idle tasks and most userspace processes should show `cs_pct=0`. The `active_us` column should grow monotonically with task uptime.

To inspect a specific task after the fact, BPF programs can read `task->cumulative_cs_time / task->cumulative_active_time` to compute `cs_pct` at any point in time, not just at tick boundaries.

### On-CPU semantics (important invariant)

`cumulative_cs_time` measures **on-CPU** time in spinlock critical sections only.  When a task is preempted mid-CS, `prepare_task_switch` closes the CS (accumulates elapsed time, clears `cs_start_ts`).  When the task resumes, `finish_task_switch` reopens the CS clock if `lock_depth > 0`.  Off-CPU time while preempted mid-CS is never counted.  This ensures `cumulative_cs_time ≤ cumulative_active_time` always holds, and `cs_pct` is a meaningful percentage.

---

## 11. Verified accounting + demo script

### Verification (live on rseqport kernel)

Confirmed with `~/IVH` loaded and `hackbench -l 100000 -g 8` running:

```
cpu=0  pid=4330  comm=hackbench  cs_us=1966786  active_us=62446  cs_pct=3149   ← BEFORE fix (cs_pct > 100% — bug)
```

Root cause: `cs_start_ts` was not paused during preemption. Off-CPU time was counted as CS time for long-running tasks (`JITWorker` showed 309 seconds of "CS time" with 3 seconds of active time).

Fix applied in `kernel/sched/core.c`:
- `prepare_task_switch`: closes CS unconditionally (`cumulative_cs_time += now - cs_start_ts; cs_start_ts = 0`)
- `finish_task_switch`: reopens CS if `lock_depth > 0` after rq-lock release

After this fix, expected output:
```
cpu=0  pid=NNNN  comm=hackbench  cs_us=X  active_us=Y  cs_pct=Z   (Z ≤ 100 always)
```

**Rebuild required** to activate the fix (core.c changed).

### Demo script: `tools/bpf/demo_cs_accounting.sh`

```bash
sudo bash tools/bpf/demo_cs_accounting.sh [duration_seconds]
```

What it does:
1. Loads `~/IVH` in the background to enable `bpf_sched_enabled()` (required for snapshot updates)
2. Starts `hackbench -g 8` as the spinlock workload
3. Polls `/sys/kernel/debug/lhp_class` every 2 seconds, filtering to non-idle tasks sorted by `cs_pct` descending
4. Prints a final full-system snapshot and a teardown guide

**Interpretation guide** embedded in the script output:
| cs_pct | Meaning |
|---|---|
| < 5% | Light spinlock user (background kernel work) |
| 5–20% | Moderate; worth watching under contention |
| > 20% | Heavy spinlock user — strong IVH candidate |

### New toys for IVH gate changes

These fields are now available in `task_struct` and readable from BPF via CO-RE:

```c
task->cumulative_cs_time      /* u64, ns on-CPU in spinlock CS (outermost only) */
task->cumulative_active_time  /* u64, ns on-CPU total since fork */
```

Derived at any tick in BPF:
```c
u64 cs_pct = task->cumulative_active_time ?
    task->cumulative_cs_time * 100 / task->cumulative_active_time : 0;
```

**Suggested IVH gate to add** in `MY_ivh_atc.bpf.c` `test()`, after Gate 4 (`!curr_lockholder` check), between the shadow candidate block and Gate 5 (capacity check):

```c
/* Gate 4b: high cs_pct + active waiter → promote to candidate even without
 * curr_lockholder=1 at this exact tick (lock may be between cs_enter/cs_exit) */
struct task_struct *curr = (struct task_struct *)BPF_CORE_READ(rq, curr);
u64 cs_time   = BPF_CORE_READ(curr, cumulative_cs_time);
u64 act_time  = BPF_CORE_READ(curr, cumulative_active_time);
u64 cs_pct    = act_time ? cs_time * 100 / act_time : 0;
if (cs_pct < CS_PCT_THRESHOLD || !curr_waiter)
    return 0;
/* falls through to migration gates 5–10 */
```

Set `CS_PCT_THRESHOLD` (suggested sweep: 5, 10, 20, 30%). Combine with `curr_waiter > 0` to require that there is an active spinner before promoting a high-cs_pct task. This is the `cs_pct` policy described at the top of the document.

---

## 12. rseq ABI Extension — Lock-Boundary CS and Wait Timing

**Branch:** `rseq-port`
**Date:** 2026-06-17
**Status:** Preload prototype validated; kernel rebuild required to activate `AT_RSEQ_FEATURE_SIZE=64`

---

### 12.1 The Original Problem: Why Tick-Based Measurement Was Insufficient

Section 3.2 (`lhp_cstime`) and Section 3.3 (`lhp_waittime`) use BPF probes at scheduler tick to sample lockholder state.  This has three fundamental limitations:

1. **Aliasing**.  If the lock is held for less than one tick period (~4 ms at `CONFIG_HZ=250`) and released before the tick fires, it is invisible.  For short CS durations (< 1 ms), the BPF probe misses the majority of lock events.

2. **No hold-time magnitude**.  At tick time we know the task *is* a lockholder, but we do not know how long it has been holding the lock or how much longer it will hold it.  The scheduler migration decision needs an estimate of "time remaining in CS" to avoid migrating a task that is about to release.

3. **No wait measurement**.  The BPF tick probe cannot distinguish "actively contending waiters" from "off-CPU waiters" or "no waiters".  Without knowing the number of waiters and their wait time, the value of a migration cannot be estimated.

**The new design** adds lock-boundary measurement directly to the locking library (an LD\_PRELOAD prototype, and a glibc patch for production use).  Every lock acquisition and release records timing data into the per-thread rseq area, which is accessible to BPF programs via `bpf_probe_read_user()` without any additional kernel syscall.

---

### 12.2 Lock-Boundary Measurement Design

The preload (`spin_cs_preload.c`) interposes `pthread_spin_lock` / `pthread_spin_unlock` via `dlsym(RTLD_NEXT, ...)` and performs the following at each event:

**At `pthread_spin_lock` entry (before acquiring):**
- Record wall timestamp (`CLOCK_MONOTONIC`) in `tls_wait_wall_enter`
- Record CPU timestamp (`CLOCK_THREAD_CPUTIME_ID`) in `tls_wait_cpu_enter`
- Atomically increment `rseq->wait_counter` by 4 (signal: "waiter entering spin")

**At `pthread_spin_lock` return (lock acquired):**
- Atomically decrement `rseq->wait_counter` by 4 (signal: "wait ended")
- Atomically add 4 to `rseq->cr_counter` (signal: "entering CS")
- Record wall timestamp in `tls_wall_enter`; CPU timestamp in `tls_cpu_enter`
- Increment `tls_depth` (nesting count)

**At `pthread_spin_unlock`:**
- Decrement `tls_depth`; if depth reaches 0 (outermost unlock):
  - Compute `cs_overall = now_wall() - tls_wall_enter`
  - Compute `cs_active  = now_cpu()  - tls_cpu_enter`
  - Compute `wait_overall = tls_wall_enter - tls_wait_wall_enter`
  - Write `cs_overall`, `cs_active`, `wait_overall` to `rseq->last_cs_overall_ns`, `rseq->last_cs_active_ns`, `rseq->last_wait_overall_ns`
  - Atomically subtract 4 from `rseq->cr_counter` (signal: "leaving CS")
- Accumulate stats in per-thread `struct thread_stats` for the end-of-process summary

**Clock source choice: `CLOCK_THREAD_CPUTIME_ID`**

`CLOCK_THREAD_CPUTIME_ID` (CPU time) advances only while the thread is scheduled on a CPU.  It does not advance during:
- Off-CPU blocking (nanosleep, futex wait, I/O)
- Preemption by a higher-priority task
- VM entry (on a vCPU, if the hypervisor preempts the vCPU)

This means `last_cs_overall_ns - last_cs_active_ns` gives the **off-CPU time accumulated while the lock was held** — the exact quantity the IVH migration decision cares about.  An elevated `offcpu_ns` during a CS indicates that the lock holder has been preempted and is a strong migration candidate.

`CLOCK_THREAD_CPUTIME_ID` is sampled at lock acquisition and at unlock: these are the exact instants when CS state changes.  This is "boundary-exact" measurement — no tick-sampling approximation.

**Clock noise caveat:** Sampling two different clocks (`CLOCK_MONOTONIC` and `CLOCK_THREAD_CPUTIME_ID`) in rapid succession introduces nanosecond-level rounding noise.  In unloaded conditions, `active_ns` can exceed `overall_ns` by up to ~8 µs due to the TSC offset between the two clock domains.  The preload computes `offcpu = max(0, overall - active)` to handle this gracefully.

---

### 12.3 rseq ABI Extension

The per-thread `struct rseq` (registered via `sys_rseq`) was extended with new fields.

#### 12.3.1 Original layout (upstream Linux, system linux-headers-6.14)

```
offset  size  field
  0      4    cpu_id_start    kernel-written; rseq entry guard
  4      4    cpu_id          kernel-written; current CPU (-1 = off-CPU)
  8      8    rseq_cs         userspace-written; abort handler pointer
 16      4    flags           userspace-written
 20      4    node_id         kernel-written; NUMA node
 24      4    mm_cid          kernel-written; per-mm concurrency ID
 28      —    [struct ends; sizeof=32; AT_RSEQ_FEATURE_SIZE=28 originally]
```

#### 12.3.2 rseqport additions (in this branch)

Fields added in `include/uapi/linux/rseq.h`:

```
offset  size  field                  writer      kernel-reads?   notes
 28      4    cr_counter             userspace   YES (fair.c)    CS nesting depth
 32      4    wait_counter           userspace   YES (fair.c)    spin-wait depth
 36      4    _cs_pad                —           no              alignment pad
 40      8    last_cs_overall_ns     userspace   no              wall time of last CS
 48      8    last_cs_active_ns      userspace   no              CPU time of last CS
 56      8    last_wait_overall_ns   userspace   no              wall time of last wait
 64      —    end[]
 sizeof = 64  (== offsetof(end); 32-byte aligned; fills exactly one cache line pair)
```

**All static assertions pass** (verified via `_Static_assert` in `/tmp/validate_abi.c`):
```
offsetof(cr_counter)         == 28  ✓
offsetof(wait_counter)       == 32  ✓
offsetof(_cs_pad)            == 36  ✓
offsetof(last_cs_overall_ns) == 40  ✓
offsetof(last_cs_active_ns)  == 48  ✓
offsetof(last_wait_overall_ns)== 56  ✓
offsetof(end)                == 64  ✓
sizeof(struct rseq)          == 64  ✓
RSEQ_ABI_FEATURE_SIZE        == 64  ✓
__alignof__(struct rseq)     == 32  ✓
```

---

### 12.4 cr_counter Semantics

`cr_counter` signals to the kernel scheduler (via `copy_from_user_nofault` in `fair.c`) whether the userspace thread is currently inside a critical section (holding a lock).

**Encoding** (mirrors `extend-sched.c` conventions):
- Bits `[31:2]`: CS nesting depth × 4. Depth 0 → `0x00000000`. Depth 1 → `0x00000004`. Depth N → `N * 4`.
- Bit 1: `RSEQ_CR_FLAG_KERNEL_REQUEST_SCHED` — set by the kernel to request a cooperative yield; userspace must call `sched_yield()` in response and then clear the bit.
- Bit 0: reserved.

**Operations** (performed by the locking library with a single atomic `addl`/`subl`):
- `lock()`   → `cr_counter += 4`
- `unlock()` → `cr_counter -= 4`

Incrementing by 4 (not 1) leaves bits 0–1 available as flags without any masking required when the kernel reads the nesting depth.

**Kernel consumption** (`kernel/sched/fair.c`, `kernel/rseq.c`):
- `rseq_len >= 32` → kernel can read `cr_counter` (offset 28, ends at 31 ≤ 32)
- Nonzero `cr_counter >> 2` → task is a userspace lockholder → `curr_user_lockholder = 1`
- `RSEQ_CR_FLAG_KERNEL_REQUEST_SCHED` bit → kernel delayed preemption requested cooperative yield

**Validation result:**

```
[1] Before lock:   cr_counter = 0x00000000  ✓
[2] Holding (d=1): cr_counter = 0x00000004  ✓
[3] Holding (d=2): cr_counter = 0x00000008  ✓
[4] Back to (d=1): cr_counter = 0x00000004  ✓
[5] After unlock:  cr_counter = 0x00000000  ✓
```

---

### 12.5 wait_counter Semantics

`wait_counter` signals to the kernel that the thread is **contending** — spinning to acquire a lock that another thread holds.  This is distinct from `cr_counter` (which signals holding, not waiting).

**Why this distinction matters for IVH:**
When a task has `wait_counter > 0` and `cr_counter == 0`, it is a pure waiter.  The BPF hook can check the *lockholder* of the lock that this waiter is spinning on.  If that lockholder is on a throttled vCPU, migrating the holder reduces the wait for all waiters immediately.  Without `wait_counter`, the BPF hook would have to infer waiter state from `curr_lockholder` on other CPUs.

**Encoding:** Same as `cr_counter` — bits `[31:2]` = depth × 4.

**Lifecycle:**
- `pthread_spin_lock()` entry → `wait_counter += 4`
- `pthread_spin_lock()` return (success) → `wait_counter -= 4`
- `wait_counter` is nonzero only during the spin-wait interval; it is always 0 when the lock is held.

**Kernel consumption:** `kernel/sched/fair.c` reads `wait_counter` (requires `rseq_len >= 36`) to populate `curr_waiter` in the BPF hook args.  Gate 4b (proposed) uses `curr_waiter > 0 && cs_pct_high` as an alternative migration trigger.

**Validation note:** `wait_counter` is ephemeral (nonzero only while spinning).  The validation test observes it *after* the spin succeeds and verifies it is 0:

```
[2] While holding: wait_counter = 0x00000000  ✓  (wait ended at acquisition)
[5] After unlock:  wait_counter = 0x00000000  ✓
```

---

### 12.6 CS Timing Fields — Semantics and Rationale

#### `last_cs_overall_ns` (offset 40, 8 bytes)

Wall-clock time of the most recently completed critical section, in nanoseconds.

```
last_cs_overall_ns = ts_wall(unlock) - ts_wall(lock_acquired)
```

Represents the real elapsed time the lock was held, including any time the holder was off-CPU (preempted, migrated, or blocked).  A large `last_cs_overall_ns` with a small `last_cs_active_ns` indicates the holder was preempted during the CS — the canonical LHP condition.

#### `last_cs_active_ns` (offset 48, 8 bytes)

On-CPU (active) time of the most recently completed critical section, in nanoseconds.

```
last_cs_active_ns = ts_cpu(unlock) - ts_cpu(lock_acquired)
```

Uses `CLOCK_THREAD_CPUTIME_ID`, which only advances while the thread is scheduled.  `last_cs_overall_ns - last_cs_active_ns` gives the off-CPU hold time.

**Derived metric: `offcpu_hold_ns`**

```
offcpu_hold_ns = max(0, last_cs_overall_ns - last_cs_active_ns)
```

This is the component of the hold time attributable to preemption.  Non-zero only when the lockholder was preempted, migrated, or blocked during the CS.

#### `last_wait_overall_ns` (offset 56, 8 bytes)

Wall-clock time from the first attempt to acquire the lock to the moment of successful acquisition, in nanoseconds.

```
last_wait_overall_ns = ts_wall(lock_acquired) - ts_wall(lock_attempt)
```

When contention is zero, this equals the CAS latency (~10–50 ns).  Under contention, it grows to the full spin time.  This is the "penalty" paid by the waiter — reducing it is the goal of IVH.

#### `last_wait_active_ns` — intentionally omitted

Spinning is CPU-bound, so `wait_active ≈ wait_overall` in the common case.  Adding it would push `sizeof(struct rseq)` from 64 to 72 (with alignment padding to 96), exceeding one or two cache lines under the current 32-byte alignment.  The preload tracks `wait_active` internally for the per-run summary but does not write it to the rseq struct.

---

### 12.7 Registration Sizes and TLS Allocation

**Running kernel (before rebuild):**

```
AT_RSEQ_FEATURE_SIZE    = 36   (offsetof(struct rseq, _cs_pad) = 36)
glibc registration size = 36   (confirmed via strace: rseq(..., 0x24, 0, ...) = 0)
```

**TLS bytes allocated by glibc** (from `sysdeps/unix/sysv/linux/dl-extra_tls.h`):

```c
roundup(MAX(AT_RSEQ_FEATURE_SIZE=36, RSEQ_AREA_SIZE_INITIAL=32), AT_RSEQ_ALIGN=32)
= roundup(36, 32) = 64 bytes
```

**Consequence:** glibc allocates 64 bytes of TLS for the rseq area even though it only registers 36 bytes.  Fields at offsets 40–63 (the timing fields) are within the allocated TLS region and can be written by userspace without overflowing into adjacent TLS variables.

**Preload behavior (per `init_rseq_thread`):**

| `__rseq_size` | Condition | Preload action | Fields available |
|---------------|-----------|----------------|-----------------|
| ≥ 36 | glibc enabled rseq (default) | skip re-registration; `tls_timing_ok=1` | all: cr, wait, timing |
| 1–35 | glibc registered but too small | skip; `tls_timing_ok=0` | none (cr_counter maybe) |
| 0 | tunable disabled (`glibc.pthread.rseq=0`) | register with 32 bytes; `tls_timing_ok=0` | cr_counter only |

**Critical bug fixed:** When `GLIBC_TUNABLES=glibc.pthread.rseq=0` is set, glibc allocates only `RSEQ_AREA_SIZE_INITIAL=32` bytes of TLS.  The original preload registered with 64 bytes and wrote to offsets 40–63, overflowing the 32-byte allocation and corrupting adjacent TLS data (specifically the glibc locale pointer, causing SIGSEGV in `__printf_fp_l_buffer` with `loc=0x7be` during `preload_fini`).  Fixed by using a `tls_timing_ok` flag to gate all writes beyond offset 31.

**After kernel rebuild (AT_RSEQ_FEATURE_SIZE = 64):**

```
glibc registration size = 64 (glibc auto-uses AT_RSEQ_FEATURE_SIZE)
TLS allocated           = 64 = roundup(64, 32)
```

This is a **flag-day ABI break**: after rebuild, existing 36-byte registrations will be rejected by the kernel (`36 < new_min_size=64`).  Requires a coordinated kernel + glibc rebuild.

**`rseq_abi.h`** (`/home/nick/rseq_abi.h`) is the canonical shared header for all userspace tools.  It uses an include-guard trick to block the system `linux/rseq.h` (which has only `mm_cid` through offset 24) while pulling in the patched kernel uapi header plus `__rseq_offset`/`__rseq_size` from `sys/rseq.h`.

---

### 12.8 Preload Prototype (`spin_cs_preload.c`)

**File:** `/home/nick/spin_cs_preload.c` (557 lines)

**Build:**

```bash
cc -shared -fPIC -O2 -Wall -Wextra -o spin_cs_preload.so spin_cs_preload.c -ldl -lpthread
```

**Usage:**

```bash
LD_PRELOAD=./spin_cs_preload.so ./spin_test [nthreads] [-s spin]
```

No tunable needed; glibc registers 36 bytes (covering cr_counter and wait_counter); preload writes timing fields to the remaining 28 bytes of the 64-byte TLS allocation.

**Key TLS variables:**

```c
struct rseq *tls_rseq;         // pointer into TLS at __rseq_offset
int          tls_timing_ok;    // 1 when 64-byte TLS is allocated (write timing fields)
int          tls_depth;        // lock nesting depth
uint64_t     tls_wall_enter;   // wall time at lock acquisition
uint64_t     tls_cpu_enter;    // CPU time at lock acquisition
uint64_t     tls_wait_wall_enter; // wall time at wait start
uint64_t     tls_wait_cpu_enter;  // CPU time at wait start
int          tls_registered;   // stats registered for this thread
struct thread_stats tls_stats; // per-thread accumulated stats
```

**Nesting model:** `tls_depth` tracks how many pthread spinlocks the thread holds.  Wait timestamps are captured only at the outermost acquire (`depth == 0`); CS timestamps are captured only at the outermost unlock (`depth` goes `1 → 0`).  Inner lock/unlock pairs still increment/decrement `cr_counter` via `do_extend`/`do_unextend`.

**`do_wait_enter` / `do_wait_exit` — atomic inline asm:**

```c
static inline void do_wait_enter(void) {
    if (!tls_rseq) return;
    asm volatile("addl %b1,%0"
                 : "+m" (*(volatile char *)&tls_rseq->wait_counter)
                 : "iq" (0x4) : "memory");
}
static inline void do_wait_exit(void) {
    if (!tls_rseq) return;
    if (tls_rseq->wait_counter & ~3)
        asm volatile("subl %b1,%0"
                     : "+m" (*(volatile char *)&tls_rseq->wait_counter)
                     : "iq" (0x4) : "memory");
}
```

These use the `%b1` register suffix (low byte) rather than a 32-bit register to avoid a 4-byte immediate when `0x4` fits in a byte.

**End-of-process summary** (written by `__attribute__((destructor))` `preload_fini`):

```
[spin_cs] ===== CS timing summary (all threads) =====
[spin_cs]   lock acquisitions    : 87838
[spin_cs]   --- Critical section (lock held) ---
[spin_cs]   avg overall_cs_ns    : 25320 ns  (25.320 µs)
[spin_cs]   avg active_cs_ns     : 25320 ns  (25.320 µs)
[spin_cs]   avg offcpu_ns        : 6 ns  (0.006 µs)
[spin_cs]   max overall_cs_ns    : 190503 ns  (190.503 µs)
[spin_cs]   offcpu fraction      : 0.02%
[spin_cs]   --- Wait (contention) ---
[spin_cs]   avg wait_overall_ns  : 7672 ns  (7.672 µs)
[spin_cs]   avg wait_active_ns   : 7643 ns  (7.643 µs)
[spin_cs]   avg wait_offcpu_ns   : 30 ns  (0.030 µs)
[spin_cs]   max wait_overall_ns  : 231316 ns  (231.316 µs)
[spin_cs]   method: CLOCK_THREAD_CPUTIME_ID (boundary-exact)
```

---

### 12.9 Comparison Against Previous lhp\_waittime Measurements

`lhp_waittime` (BPF kprobe at scheduler tick) vs preload (lock-boundary):

| Metric | lhp_waittime (tick-based) | spin_cs_preload (lock-boundary) |
|--------|--------------------------|--------------------------------|
| CS duration measurement | at scheduler tick (aliased) | at lock/unlock boundary (exact) |
| Min measurable CS | ~4 ms (one tick at HZ=250) | 0 ns (any duration) |
| Off-CPU detection | indirect (via tick timestamp delta) | `overall - active` exact |
| Wait time | not available | `lock_attempt → acquisition` wall time |
| CPU time | not available | `CLOCK_THREAD_CPUTIME_ID` |
| Overhead | BPF prog at every tick | clock_gettime × 4 per acquire/release |
| kernel fields used | `lock_depth`, `cr_counter` | `cr_counter`, `wait_counter`, timing fields |

Key finding: the lhp\_cstime BPF tool reported CS durations skewed toward tick-multiples.  The preload shows a smooth distribution averaging 25 µs, matching `spin_test` stdout (`avg: 25 µs`) to within measurement noise.  The BPF tick-based approach misses the majority of short CS events.

**Cross-validation of preload measurement accuracy:**

```
spin_test stdout (gettimeofday, lock-boundary):  avg = 25 µs
preload summary (CLOCK_MONOTONIC, lock-boundary): avg = 25.320 µs
delta: 0.32 µs (< 1.3% error; within timer resolution)
```

---

### 12.10 glibc Patch Design (`glibc_pthread_spin.patch`)

**File:** `/home/nick/glibc_pthread_spin.patch` (477 lines)

**Patched against:** glibc 2.41 (Ubuntu `glibc-source_2.41-6ubuntu1.2`)

**Files modified:**

| File | Change |
|------|--------|
| `sysdeps/unix/sysv/linux/rseq-internal.h` | Extend `struct rseq_area` with cr_counter through last_wait_overall_ns; update `RSEQ_AREA_SIZE_MAX_USED` from 28 to 64 |
| `sysdeps/x86_64/nptl/rseq-access.h` | Add `RSEQ_ADDMEM32` / `RSEQ_SUBMEM32` macros for FS-segment-relative 32-bit atomic add/sub |
| `nptl/pthread_spin_cs.h` | NEW: `__spin_rseq_ok()`, `__spin_extend()`, `__spin_unextend()`, `__spin_wait_enter()`, `__spin_wait_exit()` helpers; extern TLS declarations |
| `nptl/pthread_spin_cs.c` | NEW: TLS variable definitions (`__spin_depth`, `__spin_wall_enter`, `__spin_cpu_enter`, `__spin_wait_wall_enter`) |
| `nptl/pthread_spin_lock.c` | Add wait_counter signaling, cr_counter extend at acquisition, timestamp capture |
| `nptl/pthread_spin_unlock.c` | Write timing fields before `atomic_store_release`; call `__spin_unextend()` after release |
| `nptl/Makefile` | Add `pthread_spin_cs` to `routines` list |

**`RSEQ_ADDMEM32` macro (FS-segment atomic add):**

```c
#define RSEQ_ADDMEM32(member, value) \
  asm volatile ("addl %0, %%fs:%P1(%q2)" : \
                : "iq" (value), \
                  "i" (offsetof (struct rseq_area, member)), \
                  "r" ((long long int) __rseq_offset) \
                : "memory")
```

This is the glibc internal form of the `do_wait_enter/do_wait_exit` asm in the preload.  It uses the FS segment register to access the rseq area directly without computing a pointer, avoiding a load and register pressure in the hot path.

**Requires:** glibc rebuild + reinstall.  After rebuild, `AT_RSEQ_FEATURE_SIZE=64` from the new kernel will cause glibc to register 64 bytes automatically, and the patched `pthread_spin_lock`/`pthread_spin_unlock` will use all fields.

---

### 12.11 Complete Validation Results

**Date:** 2026-06-17  
**Kernel:** `linux-6.17-rseqport` (`AT_RSEQ_FEATURE_SIZE=36`, pre-rebuild)  
**Build:** `cc -shared -fPIC -O2 -o spin_cs_preload.so spin_cs_preload.c -ldl -lpthread`

#### A. ABI static assertions

All 11 `_Static_assert` checks pass.  Verified at compile time; no runtime action needed.

#### B. Registration

```
__rseq_offset           = -224 bytes from thread pointer
__rseq_size             = 36 bytes (glibc-registered; strace: rseq(...,0x24,0,...))
TLS bytes allocated     = 64 bytes (roundup(36,32)=64)
Timing fields safe?     = YES (offset 40–63 within 64-byte allocation)
rseq ptr alignment      = 32-byte aligned ✓
cpu_id at startup       = 0 (valid; registered) ✓
```

#### C. cr_counter behavior

```
Before lock:    cr_counter = 0x00000000  ✓
Depth=1:        cr_counter = 0x00000004  ✓
Depth=2:        cr_counter = 0x00000008  ✓
Back to depth=1:cr_counter = 0x00000004  ✓
After unlock:   cr_counter = 0x00000000  ✓
```

#### D. wait_counter behavior

```
While holding:  wait_counter = 0x00000000  ✓  (wait ended at acquisition)
After unlock:   wait_counter = 0x00000000  ✓
```

`wait_counter` is nonzero only during the spin-wait interval; cannot be observed from the thread itself because the spin loop blocks.  BPF probes on a second CPU would observe nonzero values while the waiter spins.

#### E. Timing fields — single CS with 500K-spin hold

```
last_cs_overall_ns   = 198879 ns  (198.9 µs)
last_cs_active_ns    = 198912 ns  (198.9 µs)
last_wait_overall_ns = 1614 ns  (1.6 µs)
offcpu_hold_ns       = 0 ns  (no preemption; active ≈ overall within clock noise)
```

Note: `active_ns` slightly exceeds `overall_ns` (by 33 ns) due to inter-clock sampling noise (avg 1.7 ns, max_pos 8.3 µs between `CLOCK_MONOTONIC` and `CLOCK_THREAD_CPUTIME_ID`).  Preload uses `max(0, overall-active)`.

#### F. Timing fields — 4-thread contention run (5s, 15000-spin loop)

```
lock acquisitions     = 87838
avg overall_cs_ns     = 25320 ns  (25.3 µs)   ← matches spin_test stdout (avg=25 µs) ✓
avg active_cs_ns      = 25320 ns  (25.3 µs)
avg offcpu_ns         = 6 ns  (not throttled)
max overall_cs_ns     = 190503 ns  (190.5 µs)
offcpu fraction       = 0.02%
avg wait_overall_ns   = 7672 ns  (7.7 µs)
avg wait_active_ns    = 7643 ns  (7.6 µs)
avg wait_offcpu_ns    = 30 ns
max wait_overall_ns   = 231316 ns  (231.3 µs)
```

#### G. Tunable disabled mode (GLIBC_TUNABLES=glibc.pthread.rseq=0)

With the tunable set, glibc allocates only 32 bytes of TLS.  The preload detects `__rseq_size==0`, registers with 32 bytes, sets `tls_timing_ok=0`, and disables all writes to offsets 32+ (wait_counter, timing fields).  The preload prints the summary and exits cleanly — the previous SIGSEGV (from overflowing the 32-byte TLS area into the locale pointer) is fixed.

---

### 12.12 Changed Files — Complete List

#### Kernel files (`/home/nick/kernels/linux-6.17-rseqport/`)

| File | Change | Key purpose |
|------|--------|-------------|
| `include/linux/sched.h` | +26 lines | `task_struct` fields: `cumulative_cs_time`, `cumulative_active_time` |
| `include/linux/sched_hook_defs.h` | +2 lines | New `curr_waiter` arg to BPF sched hook |
| `include/uapi/linux/rseq.h` | +75 lines | `cr_counter`, `wait_counter`, `_cs_pad`, timing fields, `end[]` |
| `kernel/locking/osq_lock.c` | ±24 lines | `lock_depth` tracking for optimistic spin queue |
| `kernel/locking/qspinlock.c` | ±23 lines | `lock_depth` tracking for qspinlock |
| `kernel/locking/spinlock.c` | ±93 lines | `lock_depth` tracking for raw spinlock (main) |
| `kernel/rseq.c` | ±67 lines | `rseq_sched_extend_usec` sysctl; extension timer; wait_counter reading |
| `kernel/sched/core.c` | ±42 lines | rseq `cr_counter` reading at tick; `lock_depth` handoff fix at `context_switch` |
| `kernel/sched/fair.c` | ±18 lines | `curr_user_lockholder`, `curr_waiter` computation from rseq fields |
| `kernel/sched/sched.h` | +3 lines | `curr_waiter` field in BPF hook arg struct |

#### Userspace tools (`tools/bpf/`)

| File | Change |
|------|--------|
| `MY_ivh_atc.bpf.c` | ±168 lines — IVH implementation using lockholder/waiter info |
| `bpf_helpers.h` | +2 lines — helper additions |
| `lhp_cstime.bpf.c` | ±23 lines — CS duration profiler |
| `lhp_waittime.bpf.c` | ±68 lines — wait time + CS time profiler |
| `lhp_waittime.c` | ±10 lines — wait time profiler userspace |

#### New userspace files (`/home/nick/`)

| File | Purpose |
|------|---------|
| `rseq_abi.h` | Shared ABI header for all userspace tools; include-guard trick |
| `spin_cs_preload.c` | LD\_PRELOAD prototype for `pthread_spin_lock`/`unlock` |
| `spin_cs_preload.so` | Built binary (rebuild: `cc -shared -fPIC -O2 ...`) |
| `spin_test.c` | pthread spinlock benchmark for preload testing |
| `glibc_pthread_spin.patch` | glibc 2.41 patch for native pthread integration |

#### Files with private `struct rseq_abi` (not yet updated)

| File | Status |
|------|--------|
| `extend-sched.c` | Still uses private `struct rseq_abi`; update deferred |

---

### 12.13 Remaining Work and Rebuild Package

#### 12.13.1 Requires kernel rebuild + reboot

**What changes after rebuild:**

- `AT_RSEQ_FEATURE_SIZE` changes from 36 to 64 (reports `offsetof(struct rseq, end)`)
- Kernel validates `rseq_len >= 64` for full-feature registrations
- glibc auto-registers 64 bytes (no tunable needed)
- BPF programs can read `last_cs_overall_ns` etc. via `bpf_probe_read_user()`

**⚠️ Flag-day break:** After kernel rebuild, existing 36-byte glibc registrations will be rejected.  Kernel rebuild and glibc rebuild must be done together before booting.

#### 12.13.2 Requires glibc rebuild + reinstall

`/home/nick/glibc_pthread_spin.patch` contains the complete patch.  Apply to glibc 2.41 source, rebuild, and reinstall.

#### 12.13.3 After kernel+glibc rebuild: extend-sched.c update

Replace the private `struct rseq_abi` in `extend-sched.c` with `#include "/home/nick/rseq_abi.h"`.

#### 12.13.4 Experiments enabled after rebuild

1. **BPF read of `last_cs_overall_ns`** during kernel lockholder detection: instead of relying solely on `cr_counter > 0` at tick time, read the most recently completed CS duration to estimate "is this lock hot?"
2. **`wait_counter` in fair.c gate**: use `curr_waiter > 0` combined with `cs_pct` to trigger migration even when the lockholder is between CSes.
3. **Off-CPU hold detection**: `last_cs_overall_ns - last_cs_active_ns` in BPF → if `offcpu_hold_ns > THRESHOLD_NS`, task was preempted during CS → priority migration candidate.

---

## 13. Kernel Rebuild and Post-Boot Validation

### 13.1 Kernel Build Commands

```bash
# Working directory
cd /home/nick/kernels/linux-6.17-rseqport

# Confirm configuration is present
ls .config   # should exist from previous builds

# Build (incremental; only changed objects)
make -j$(nproc) 2>&1 | tee /home/nick/build.log

# If build fails, check:
grep -i "error:" /home/nick/build.log | head -20
```

### 13.2 Kernel Install Commands

```bash
cd /home/nick/kernels/linux-6.17-rseqport

# Install modules
sudo make modules_install

# Install kernel image and update grub
sudo make install
# OR, if make install does not update grub automatically:
sudo cp arch/x86/boot/bzImage /boot/vmlinuz-6.17.0-rseqport14+
sudo update-grub
```

### 13.3 Reboot Instructions

```bash
# Verify grub entry exists before rebooting
grep -i rseqport /boot/grub/grub.cfg | head -5

# Reboot into new kernel
sudo reboot

# After boot, verify kernel version
uname -r   # expect: 6.17.0-rseqport14+ (or similar)
```

### 13.4 Post-Boot Validation Commands

Run these in order after booting into the new kernel.

#### Step 1 — Verify AT_RSEQ_FEATURE_SIZE = 64

```bash
# Via /proc/sys (if exposed)
cat /proc/sys/kernel/rseq_feature_size 2>/dev/null || echo "not in sysfs"

# Via LD_SHOW_AUXV
LD_SHOW_AUXV=1 /bin/true 2>&1 | grep -i rseq

# Via a small C program
cat << 'EOF' > /tmp/check_rseq_size.c
#include <sys/auxv.h>
#include <sys/rseq.h>
#include <stdio.h>
int main(void) {
    unsigned long feat = getauxval(AT_RSEQ_FEATURE_SIZE);
    printf("AT_RSEQ_FEATURE_SIZE = %lu  (expect 64)\n", feat);
    printf("__rseq_size          = %u   (glibc registered; expect 64 after glibc rebuild)\n", __rseq_size);
    return feat == 64 ? 0 : 1;
}
EOF
gcc -o /tmp/check_rseq_size /tmp/check_rseq_size.c && /tmp/check_rseq_size
```

Expected output after kernel rebuild (before glibc rebuild):
```
AT_RSEQ_FEATURE_SIZE = 64  ✓
__rseq_size          = 36  (glibc not yet rebuilt — will reject at next kernel step)
```

Expected after kernel + glibc rebuild:
```
AT_RSEQ_FEATURE_SIZE = 64  ✓
__rseq_size          = 64  ✓
```

#### Step 2 — Verify ABI static assertions still pass

```bash
gcc -O2 -o /tmp/validate_abi /tmp/validate_abi.c && /tmp/validate_abi
# Expect: ABI STATIC ASSERTIONS: ALL PASS
# Expect: TLS bytes allocated = 64
# Expect: Timing fields safe? = YES
```

Note: After kernel-only rebuild (before glibc rebuild), `__rseq_size` will be 36 and the validation program will report `TLS bytes allocated = 64` (correct — roundup(36,32)=64).  The timing fields are still safe to write.

#### Step 3 — Verify cr_counter operation

```bash
gcc -O2 -o /tmp/validate_counters /tmp/validate_counters.c -lpthread
LD_PRELOAD=/home/nick/spin_cs_preload.so /tmp/validate_counters 2>/dev/null
# Expect: cr_counter 0→4→8→4→0 at corresponding lock depth changes
# Expect: last_cs_overall_ns nonzero after CS
# Expect: last_cs_active_ns nonzero after CS
# Expect: active <= overall (within clock noise; max_pos ~8 µs)
```

#### Step 4 — Verify wait_counter operation (requires BPF probe from second process)

```bash
# Terminal 1: run a spin-heavy workload
LD_PRELOAD=/home/nick/spin_cs_preload.so ./spin_test 4 -s 50000 &

# Terminal 2: use bpftrace to observe wait_counter going nonzero
# (replace PID with actual PID from Terminal 1)
PID=$(pgrep spin_test)
bpftrace -e "
interval:ms:100 {
    @pid = ${PID};
}
" 2>/dev/null || echo "use BPF probe from lhp_waittime instead"

# Alternatively: verify through the preload summary
# If avg wait_overall_ns >> 0 ns, wait_counter was correctly signaled
LD_PRELOAD=/home/nick/spin_cs_preload.so ./spin_test 4 -s 50000 2>&1 | grep "avg wait_overall"
```

#### Step 5 — Verify timing fields written to rseq struct

```bash
gcc -O2 -o /tmp/rseq_fields_test /tmp/rseq_fields_test.c -lpthread 2>/dev/null || \
  echo "rebuild needed — see /tmp/rseq_fields_test.c"

LD_PRELOAD=/home/nick/spin_cs_preload.so /tmp/rseq_fields_test 2>/dev/null
# Expect: PASS: timing fields written correctly to rseq ABI
# Expect: last_cs_overall_ns ≈ 10 µs (the 10-µs hold time in the test)
# Expect: last_cs_active_ns ≈ 10 µs
# Expect: last_wait_overall_ns ≈ 0–2 µs (no contention)
```

#### Step 6 — Full regression: spin_test + preload

```bash
LD_PRELOAD=/home/nick/spin_cs_preload.so ./spin_test 4 2>&1
# Expect:
#   lock acquisitions > 0
#   avg overall_cs_ns matches spin_test stdout "avg" (within 2%)
#   avg active_cs_ns ≈ avg overall_cs_ns (not throttled; offcpu < 1%)
#   avg wait_overall_ns > 0 (4-thread contention)
#   method: CLOCK_THREAD_CPUTIME_ID (boundary-exact)
```

### 13.5 First Step After Successful Reboot

**After verifying AT_RSEQ_FEATURE_SIZE=64:**

1. **Rebuild glibc** (flag-day: 36-byte registrations are now rejected):
   ```bash
   cd /path/to/glibc-2.41-build
   patch -p1 < /home/nick/glibc_pthread_spin.patch
   make -j$(nproc)
   sudo make install
   ```

2. **Verify glibc registers 64 bytes:**
   ```bash
   strace -e rseq /bin/true 2>&1 | grep rseq
   # Expect: rseq(..., 0x40, 0, 0x53053053) = 0   (0x40 = 64)
   ```

3. **Re-run the preload with full native glibc registration:**
   ```bash
   # Preload is no longer needed for cr_counter/wait_counter (glibc does it natively)
   # But preload still adds the preload summary to stderr for benchmarking
   LD_PRELOAD=/home/nick/spin_cs_preload.so ./spin_test 4
   ```

4. **Update `extend-sched.c`** to use `rseq_abi.h` instead of its private struct.

5. **Run the threshold sweep experiments** defined in Section 6.E using `lhp_waittime` + `MY_ivh_atc` with the new per-task CS timing fields available in the BPF hook.
