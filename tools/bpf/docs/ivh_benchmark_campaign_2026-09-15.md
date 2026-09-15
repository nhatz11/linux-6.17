# IVH benchmark campaign: PV vs IVH at half contention, 2026-09-15

Goal: find every benchmark that shows a **consistent** improvement under IVH
(migration + tier-1/tier-2 adaptive spinning) versus stock PV qspinlock.
Acceptance floor **5%**; the interesting set is **double digit**.

- Kernel `6.17.0-G-LOCK-30-csfast+` (the kernel that produced the 2026-09-15
  half-contention results: hackbench 12.3 s vs PV 61.7 s, dbench +19.9%,
  ebizzy +114.7%). All `ivh_cs_*` knobs at their defaults, i.e. off.
- MY_ivh_atc with the **loose capacity gate** (`HARDFLOOR 500`, `TOPBAND 250`,
  kernel commit `931d6c933250`), verified live in the loaded program.
- Host co-runner on **vCPUs 0-7 only** (half contention), confirmed from
  `ivh_uc_capacity` (~470-580 on 0-7, 1023-1024 on 8-15).
- Harness: `/root/ivh_tools/campaign/` (`run_campaign.sh`, `benchmarks.tsv`,
  `analyze.py`, `validate.sh`, `status.sh`). Output in `run_main/`.

## 1. Arms

| arm | `ivh_universal_eligible` | `spin_mode` | meaning |
|---|---|---|---|
| **pv** | 0 | 1 (VANILLA, tier 1 + exhaustion, `preempt_src=0`) | stock PV qspinlock, no migration |
| **ivh** | 1 | 2 (ADAPTIVE, tier 1 + tier 2 + exhaustion, `preempt_src=2`) | migration + IVH adaptive spinning |

Both arms keep `MY_ivh_atc` and `vcap_probe` **running**. Stopping `vcap_probe`
costs a ~130 s capacity-EMA reconvergence (`goto_mode.sh`), which would bias
whichever arm followed it; migration is switched off at the gate instead. Every
arm switch is read back and the run aborts if it did not take.

## 2. Method (why it is shaped this way)

From `ivh_sustained_load_drift_and_capacity_test_2026-09-15.md`:

1. **Never compare across boots.** Every comparison lives inside one block.
2. **ABBA / BAAB blocks.** Block order alternates `pv ivh ivh pv` and
   `ivh pv pv ivh`, so linear drift and position effects cancel within a block.
3. **Capacity-settled wait before each block** (`wait_capacity_settled.sh`,
   min 45 s), so the previous workload's load history does not bias the next.
4. **Two stages.** Screen = 3 blocks per workload. A workload becomes a
   CANDIDATE when all blocks agree in sign and the median effect is >= 5%.
   Confirm = 5 more blocks for candidates only (8 total).
5. **Effect sign is normalised** so positive always means IVH is better,
   whichever direction the metric runs.
6. **Resumable.** Results append to `results.csv` after every run; finished
   (workload, block) pairs are skipped on restart, so the run survives a killed
   Claude session, a lost SSH connection, or a restart of the harness.

### Guards

- daemons alive, disk >= 10 GB free, arm verified before every run
- run timeout per workload (120-180 s)
- **hard** kernel errors (BUG, oops, hung task, RCU stall, soft lockup) abort
- plain `WARNING:` lines are **recorded, not fatal**: the
  "Voluntary context switch within RCU read-side critical section"
  warning (`kernel/rcu/tree_plugin.h:332`) predates this campaign, appears in
  the boot that produced the 2026-09-15 results, and fires under load in
  whatever process happens to be running (perf bench, stress-ng, even the
  `spin_mode` script). Logged to `kernel_warnings.txt`.
- a workload whose extractor fails 3+ times in a block is skipped, not fatal

### Validation pass

Every workload was run once before the campaign (`validate.sh`) to prove its
command runs and its extractor yields a number. 71/76 passed first time. Fixes:
`perf bench futex wake/wake-parallel/requeue` do not accept `-r` (dropped) and
print to stderr (redirected), `schbench` prints to stderr (redirected),
`wis_malloc1` exceeds 120 s on this host (**dropped**). Final list: **75**.

## 3. The benchmarks (75)

### Kernel-side, lock-heavy (will-it-scale, 34 tests, threads variants, 16 tasks)

`lock1`, `lock2` (`blocked_lock_lock`, `flc_lock`), `futex1-4` (`hb->lock`),
`page_fault1-3`, `mmap1`, `mmap2`, `brk1`, `tlb_flush1`, `tlb_flush2`,
`open1`, `open2`, `unlink1`, `unlink2`, `dup1`, `pipe1`, `unix1`, `poll1`,
`eventfd1`, `signal1`, `context_switch1`, `sched_yield`, `posix_semaphore1`,
`fallocate1`, `lseek1`, `pread1`, `pwrite1`, `read1`, `write1`,
`getppid1` (near-zero-lock control).

### Scheduler / IPC / syscall microbenchmarks (13)

`hackbench` pipe-threads, socket-threads, pipe-processes;
`perf bench` sched messaging, sched pipe, futex hash / wake / wake-parallel /
requeue / lock-pi, epoll wait, epoll ctl, syscall basic.

### Kernel stressors (8)

`stress-ng` flock, futex, mmap, pipe, sem, fork, dentry, sock (16 workers each).

### Filesystem and I/O (3)

`dbench` 16 clients (`-F`, fsync), `fio` 16 jobs random read/write on tmpfs,
`fs_mark` 16 threads on tmpfs.

### Networking (3)

`netperf` TCP_STREAM x16 flows, `netperf` TCP_RR x16 flows (socket buffer and
qdisc spinlocks, short critical sections), `iperf3 -P 16`.

### Userspace spinlocks (7)

- `spinbench` (written for this campaign, `/root/bench/micro/spinbench.c`):
  16 threads, one `pthread_spinlock_t`, tunable CS length -- short (50 pause),
  medium (500), long (5000). The cleanest userspace LHP shape.
- `libslock` (Davidal./SOSP'13 harness) `stress_test` built four times, one per
  lock algorithm: **TAS**, **TTAS**, **ticket**, **MCS**. Shows whether a result
  depends on the spinlock algorithm.

**Note for the paper:** userspace spinlocks never enter the kernel's qspinlock,
so IVH's kernel adaptive spinning cannot act on them. Any win there belongs to
**migration**, not adaptive spinning.

### Applications (4)

`ebizzy` mmap mode and malloc mode, `schbench` (RPS),
Phoenix MapReduce `word_count` (123 MB input, 5 iterations) and `kmeans`.

### Dropped, and why

| dropped | reason |
|---|---|
| `wis_malloc1` | exceeds the 120 s timeout on this host |
| `locktorture` | `CONFIG_LOCK_TORTURE_TEST` is not set; needs a kernel rebuild. Fold into the next kernel build |
| `stress-ng --spinlock` | no such stressor in stress-ng 0.18.11 |
| `compilebench` | not packaged |
| PARSEC, PostgreSQL, memcached, nginx, RocksDB SpinMutex, TBB, Abseil, InnoDB | hours-to-days of build and configuration; a later wave |
| will-it-scale `_processes` variants (60 more) | time budget |

## 4. Cost

Screen: 75 workloads x 3 blocks x (settle + 4 runs) ~ 9-10 h.
Confirm: candidates x 5 blocks. Full run therefore lands in the morning.

## 5. Reading the output

- `run_main/log` -- live progress, per-run values, capacity at each block
- `run_main/results.csv` -- every measurement
- `run_main/decisions_screen.txt` -- verdicts after the screen
- `run_main/decisions_final.txt` -- verdicts over all blocks
- `status.sh` -- one-screen summary at any time

Verdicts: **CANDIDATE** (all blocks agree, median >= +5%), **REGRESSION**
(all blocks agree, median <= -5%), **NOISY** (>= 5% but blocks disagree),
**neutral** (< 5%).

## 6. Results

To be filled in when the run completes.
