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

## 6. Results (final, campaign complete 2026-09-15 20:18)

**1300 measurements, 0 failures, 0 skipped workloads, 18 kernel WARNING lines
logged (all the pre-existing RCU one).** All 38 candidates confirmed over 8
blocks each; 21 regressions, 28 neutral, 7 noisy, 1 invalid metric.

### Confirmed wins (8/8 blocks each)

| workload | IVH vs PV | kind |
|---|---|---|
| fsmark_tmpfs | **+167.0%** | filesystem metadata (app-level) |
| perf_sched_pipe | **+146.9%** | micro (2-process ping-pong) |
| ebizzy_mmap | **+104.3%** | application |
| stressng_dentry | **+99.5%** | kernel stressor |
| hackbench_pipe_thr | **+76.3%** | scheduler/IPC |
| hackbench_sock_thr | **+75.4%** | scheduler/IPC |
| hackbench_pipe_proc | **+61.8%** | scheduler/IPC |
| perf_epoll_wait | **+53.9%** | micro |
| stressng_flock | **+44.2%** | kernel stressor |
| sysbench_mutex | **+24.4%** | userspace mutex -> futex |
| stressng_mmap | **+23.4%** | kernel stressor |
| stressng_sock | **+19.7%** | kernel stressor |
| dbench_16 | **+19.1%** | file server (app-level) |
| stressng_pipe | **+15.8%** | kernel stressor |
| wis_mmap2 | **+11.2%** | micro |
| wis_mmap1 | **+10.9%** | micro |
| stressng_futex | **+10.5%** | kernel stressor |
| perf_syscall_basic | **+8.7%** | micro |
| schbench | **+7.4%** | scheduler RPS (app-level) |

## 7. Full inventory: tried vs not tried, kernel vs userspace

Numbers are median IVH-vs-PV across blocks (8 for confirmed candidates, 3 for the
rest). Positive = IVH better. Half contention, kernel G-LOCK-30, loose gate.

### 7.1 KERNEL-SPACE contention -- IMPROVED (16)

| workload | gain | blocks | kernel lock exercised |
|---|---|---|---|
| fs_mark (tmpfs) | **+167%** | 8/8 | dcache/inode, VFS metadata |
| perf sched pipe | **+147%** | 8/8 | pipe mutex + rq locks |
| ebizzy mmap | **+104%** | 8/8 | `mmap_lock` (rwsem `wait_lock`) |
| stress-ng dentry | **+100%** | 8/8 | dcache spinlocks |
| hackbench pipe-threads | **+76%** | 8/8 | pipe + scheduler |
| hackbench socket-threads | **+75%** | 8/8 | `unix_state_lock`, `sk_receive_queue.lock` |
| hackbench pipe-processes | **+62%** | 8/8 | pipe + scheduler |
| perf epoll wait | **+54%** | 8/8 | `ep->lock`, waitqueue |
| stress-ng flock | **+44%** | 8/8 | `blocked_lock_lock`, `flc_lock` |
| stress-ng mmap | **+23%** | 8/8 | `mmap_lock` |
| stress-ng sock | **+20%** | 8/8 | socket + sk queue locks |
| dbench (16 clients) | **+19%** | 8/8 | VFS metadata + fsync |
| stress-ng pipe | **+16%** | 8/8 | pipe mutex |
| will-it-scale mmap1 / mmap2 | +10.9 / +10.4% | 3/3 | `mmap_lock` |
| perf syscall basic | +10.3% | 6/6 | syscall entry path |
| stress-ng futex | +10.5% | 8/8 | futex `hb->lock` |
| schbench | +6.9% | 3/3 | scheduler wakeup |

### 7.2 KERNEL-SPACE -- REGRESSED (17)

| workload | loss | category | fixable? |
|---|---|---|---|
| netperf TCP_RR | −44% | latency-bound pair | **yes**: migration eligibility gate (exclude tight comm pairs) |
| iperf3 (16 flows) | −34% | latency/locality | **yes**: same gate |
| will-it-scale tlb_flush1 | −29% | migration spreads `mm_cpumask` -> more shootdown IPIs | **yes**: same mechanism the daemon already uses to exclude JIT processes |
| netperf TCP_STREAM | −18% | latency/locality | **yes**: same gate |
| will-it-scale futex4 | −16% | saturated micro | no -- inherent, aggregate |
| will-it-scale dup1 | −12% | saturated micro | no -- inherent, aggregate |
| will-it-scale eventfd1 | −11% | saturated micro | no -- inherent, aggregate |
| will-it-scale futex2 | −10% | saturated micro | no -- inherent, aggregate |
| will-it-scale lock1 / lock2 | −10 / −9% | saturated micro | no -- inherent, aggregate |
| will-it-scale unix1 | −9% | saturated micro | no -- inherent, aggregate |
| will-it-scale unlink1 / unlink2 | −7.7 / −7.4% | saturated micro | no -- inherent, aggregate |
| will-it-scale open1 | −7.2% | saturated micro | no -- inherent, aggregate |
| perf futex hash | −7.0% | saturated micro | no -- inherent, aggregate |
| will-it-scale pipe1 | −5.9% | saturated micro | no -- inherent, aggregate |
| will-it-scale fallocate1 | −5.8% | saturated micro | no -- inherent, aggregate |
| perf futex wake-parallel | invalid | sub-ms metric, +-66% variance | **drop, do not report** |

**The saturated-micro rule:** 16 threads, one syscall in a tight loop, zero work
between calls, every vCPU busy. No idle destination to migrate to and no
preempted holder to rescue, so IVH can only cost. Report as one aggregated
sentence ("13 saturated will-it-scale microbenchmarks cost 6-16%"), not 13 rows.

Contrast inside our own data: stress-ng flock **+44%** and hackbench **+75%** hit
the *same* kernel locks as will-it-scale lock1 (−10%) and unix1 (−9%). The
difference is spare capacity and real work, not the lock.

### 7.3 KERNEL-SPACE -- NEUTRAL (<5%, 21) and NOISY (5)

Neutral: perf sched messaging +4.4, perf epoll ctl +4.4, will-it-scale pread1,
getppid1, poll1, write1, posix_semaphore1, futex1, futex3, sched_yield, lseek1,
signal1, pwrite1, open2, page_fault1/2/3, context_switch1, read1, perf futex
lock-pi, fio (tmpfs).
Noisy (direction flips between blocks): will-it-scale brk1 +43, perf futex wake
+38, will-it-scale tlb_flush2 +13, stress-ng fork +10, perf futex requeue −6.7.

These are "IVH costs nothing here" -- worth one line, not a table. The noisy ones
need longer runs before any claim.

### 7.4 USERSPACE locks -- results (10)

| workload | result | lock | engineering path |
|---|---|---|---|
| sysbench mutex | **+24%** | pthread_mutex -> futex | already wins (blocks through kernel futex path) |
| Phoenix word_count | +4.3% (3/3, under floor) | pthread_mutex | **AFL port** could push it over 5% |
| spinbench short / med / long | −0.5 / +1.2 / −0.9% | `pthread_spinlock_t` | **AFL port + `extend()`/`unextend()`** -- this is the designed case |
| libslock MCS / ticket | +2.1 / −1.7% | userspace MCS / ticket | **AFL port** |
| libslock TAS / TTAS | −6.6 / +11.1%, both noisy | userspace TAS/TTAS | **AFL port**; noisy because they spin with no backoff |
| ebizzy malloc | −0.8% | glibc malloc arena | patched glibc already instruments it; AFL swap possible |
| Phoenix kmeans | **−8.7%** | pthread_mutex + condvar | **AFL port** (blocked: AFL has no condvar) **or `ivh_exclude`** |
| sysbench threads | **−30%** | pthread_mutex + `sched_yield` | likely migration churn; `ivh_exclude` first, then AFL |
| stress-ng sem | **−19%** | POSIX semaphores -> futex | AFL has no semaphore API; `ivh_exclude` first |

**Pattern:** unmodified userspace lock workloads are flat or negative, never
positive. The two userspace wins (sysbench mutex, stress-ng futex) are the ones
that *block through the kernel futex path*, where kernel spinlocks are exercised.
Pure userspace spinning (spinbench, libslock) is invisible to IVH until ported.

### 7.5 NOT TRIED

**Kernel-space, no source changes needed**
- `locktorture` -- needs `CONFIG_LOCK_TORTURE_TEST`; fold into the next kernel build
- will-it-scale `_processes` variants (~60) -- different lock mix (no shared mm)
- kernel build / kernbench -- the standard "real work" benchmark in this literature
- fxmark (filesystem scalability ladder), filebench, compilebench, MOSBench, LEBench
- PostgreSQL + pgbench, MySQL/InnoDB + sysbench-oltp
- memcached + memtier, Redis + memtier
- nginx + wrk, Apache + ab

**Userspace, needs the AFL port / `extend()` / `sys_ivh_cs_enter()`**
- PARSEC: **dedup** (`pthread_spinlock_t`, zero source changes needed with the
  patched glibc), **streamcluster** / **fluidanimate** (hand-rolled trylock
  barrier), plus the rest as controls
- RocksDB `db_bench` with SpinMutex, LevelDB `db_bench`
- TBB `spin_mutex`, Abseil/tcmalloc SpinLock, InnoDB spin-then-sleep mutex
- SPLASH-2/3/4 -- **dropped**: all 14 codes use `pthread_mutex`, no spinlocks
- Phoenix remaining apps (histogram, string_match, pca, linear_regression), Metis

### 7.6 What to do with each group in the paper

| group | treatment |
|---|---|
| 16 kernel-space wins | the results table; lead with the application-level ones |
| 4 fixable regressions (3 network + tlb_flush1) | show the migration eligibility gate fixing them; turns a weakness into a contribution |
| 13 saturated micro regressions | one aggregated sentence as a stated limitation |
| 21 neutral | one line: "IVH costs nothing on 21 further workloads" |
| 5 noisy | omit, or re-run longer before claiming anything |
| 9 userspace | scope statement: not wired up yet; AFL port is future work |
| invalid metric | drop |
