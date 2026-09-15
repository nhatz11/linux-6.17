# IVH roadmap, 2026-09-15

Written so none of this has to be remembered. Status as of the end of the
2026-09-15 benchmark campaign (`ivh_benchmark_campaign_2026-09-15.md`).

**Rule of thumb for planning:** every item marked **[no reboot]** is userspace
work or a live sysctl. The running kernel already provides the rseq `cr_counter`
(for `extend()`/`unextend()`) and `sys_ivh_cs_enter()` (syscall 470), so the
entire userspace track needs **no kernel rebuild and no reboot**. Only the kernel
variants below need one.

---

## 0. Where things stand

- Kernel in use: `6.17.0-G-LOCK-30-csfast+`, loose capacity gate
  (`HARDFLOOR 500`, `TOPBAND 250`).
- Campaign: 75 workloads, PV vs IVH, half contention, 1266 measurements,
  0 failures. **19 wins** (17 of them double digit, 8/8 blocks),
  **21 regressions**, **21 neutral**, **5 noisy**, 1 invalid metric.
- Every win is kernel-side and needed **no source changes**.
- Every unmodified userspace *spinning* workload is flat; the only userspace wins
  block through the kernel futex path.

---

## 1. Userspace track -- make the neutral/regressing userspace workloads benefit

**[no reboot for all of section 1]**

### 1.1 Close the AFL API gaps (do this first -- everything else depends on it)

`/root/linux-6.17/ivh_adaptive_futex_lock.h` currently has lock/unlock and the
shared-futex fix, and is missing:

| missing | difficulty | blocks |
|---|---|---|
| **trylock** | **easy** (hours) -- one cmpxchg attempt, no wait path | anything calling `pthread_mutex_trylock`; PARSEC streamcluster/fluidanimate barriers |
| **condition variable** | **medium** (~1 day) -- must interact with the AFL state machine, not just raw futex | Phoenix (its MapReduce scheduler waits on condvars), streamcluster |
| **rwlock** | medium | reader-heavy apps |
| **semaphore** | medium | stress-ng sem |

### 1.2 LD_PRELOAD interposition shim -- the big lever

One shim that overrides `pthread_mutex_lock/unlock/trylock/init/destroy` and
`pthread_spin_*`, routing them into the AFL, with `extend()`/`unextend()`
bracketing every critical section automatically. Precedent: LiTL
(Guerraoui et al., "Lock-Unlock: Is That All?"), which does exactly this to run
unmodified applications on arbitrary lock algorithms.

**Difficulty: medium.** The one real obstacle:

> `pthread_mutex_t` is 40 bytes; an AFL lock is ~192 bytes. It does **not** fit
> in place. Needs a side table keyed by the `pthread_mutex_t*` with lazy
> allocation on first use (LiTL does this), plus a fallback to the real pthread
> symbol for any operation the AFL does not implement.

Payoff: engineers Phoenix, sysbench, stress-ng, PARSEC dedup and any future
application **without editing a single benchmark or touching glibc**, and gives
the paper the strong claim "unmodified binaries, lock implementation swapped by
interposition".

### 1.3 Per-benchmark source swaps (fallback where interposition cannot reach)

| workload | how | difficulty |
|---|---|---|
| **PARSEC dedup** | one macro pair, `mbuffer.c:26-38`, covers all 11 sites. Also works with zero edits via the patched glibc, which already instruments `pthread_spin_lock` | **easy**. Careful: `mbuffer_realloc()` has 3 exit paths, all need `unextend()` |
| **libslock** | it already has a lock abstraction with a `LOCK_VERSION` define; add AFL as one more implementation next to TAS/TTAS/ticket/MCS | **easy**, and gives an AFL-vs-5-algorithms comparison for free |
| **spinbench** (`/root/bench/micro/spinbench.c`) | one line | trivial |
| **Phoenix** | edit the MapReduce library in `phoenix-2.0/src` | medium, **blocked on AFL condvar** |
| **PARSEC streamcluster / fluidanimate** | hand-rolled `while(trylock()==EBUSY);` in `parsec_barrier.cpp`; not a pthread call, so interposition cannot see it. Use `sys_ivh_cs_enter()` + `extend()`/`unextend()` instead of swapping the lock | medium. `parsec_barrier_wait()` has **6 unlock exit paths**; CS contains `pthread_cond_broadcast()` (a syscall under the lock) |

### 1.4 Needs library-level work (lowest priority)

- **ebizzy malloc mode**: glibc's *internal* malloc arena locks, not the public
  pthread API. Needs a patched glibc (one already exists for
  `pthread_spin_lock`). No reboot, but a library rebuild.
- **TBB `spin_mutex`, Abseil/tcmalloc SpinLock, InnoDB spin-then-sleep**:
  inlined atomics inside C++ libraries; interposition cannot see them.

---

## 2. Kernel track -- needs rebuild + reboot

| item | state |
|---|---|
| **G-LOCK-31** | built, installed, **never booted**. Adds: skip only preempted (never halted) waiters, `ivh_pv_tier2_enable`, `ivh_cs_criterion` (last-CS test), `ivh_cs_scan`, `spin_mode 6` preset. All default to G-LOCK-30 behaviour |
| **G-LOCK-32** | designed + independently reviewed (SOUND WITH FIXES), **not built**. Defers the next-waiter choice to unlock. F1 (state change before kick) is mandatory or a waiter can hang. See `ivh_glock32_unlock_skipping_design_2026-09-15.md` §8 |
| **locktorture** | `CONFIG_LOCK_TORTURE_TEST` is off. Turn it on in the next kernel build; it is the only in-kernel spinlock hammer with tunable hold times |

---

## 3. Measurement work still owed

**[no reboot]**

1. **Split the arms on the regressions.** Today's campaign compared PV vs
   migration+adaptive-spinning *together*. Re-run only the regressing workloads
   as PV vs AS-only and PV vs migration-only (~1 h). This decides whether the
   losses are the halting or the moving, and therefore whether the fix is a
   policy gate or a spin-threshold change.
2. **Demonstrate a migration eligibility gate** on the 4 fixable regressions
   (netperf TCP_RR/STREAM, iperf3, will-it-scale tlb_flush1). The daemon already
   excludes JIT processes for the same `mm_cpumask`-spreading reason; extend that
   to tight communication pairs. Turns a weakness into a contribution.
3. **Two experiments that test the "runnable oversubscription" rule** (the rule
   that explains why hackbench wins and netperf loses):
   - `hackbench -g8 -f2` (pair-like grouping) -- should still win, killing the
     "many-to-many vs pairs" explanation;
   - netperf with **far more flows than vCPUs** -- if the loss shrinks or flips,
     the rule is confirmed.
4. **Full-contention spot check** of the confirmed winners (needs the co-runner
   moved to all 16 vCPUs).
5. **Re-run the 5 noisy workloads** with longer runs before claiming anything.
6. **Drop `perf bench futex wake-parallel`** (sub-ms metric, +-66% variance).

---

## 4. Benchmarks not yet tried

**Kernel-side, no source changes [no reboot except locktorture]**
kernel build / kernbench (the standard "real work" benchmark in this
literature), will-it-scale `_processes` variants (~60, different lock mix),
fxmark, filebench, compilebench, MOSBench, LEBench,
PostgreSQL + pgbench, MySQL/InnoDB + sysbench-oltp, memcached + memtier,
Redis + memtier, nginx + wrk, Apache + ab. `locktorture` needs the rebuild.

**Userspace, needs section 1**
PARSEC (dedup, streamcluster, fluidanimate + the rest as controls),
RocksDB/LevelDB `db_bench`, Phoenix remaining apps, Metis.
**SPLASH-2/3/4 is dropped**: all 14 codes use `pthread_mutex`, no spinlocks.

---

## 5. Paper framing notes

- Lead the results table with the **application-level** wins (fs_mark, ebizzy,
  dbench, hackbench, schbench), not the microbenchmarks.
- Collapse the **7 stress-ng** results into one grouped line; stress-ng is a QA
  tool, not a benchmark people publish against.
- `perf bench sched pipe` (+147%) is the biggest number but is a 2-process
  ping-pong: use it to explain the mechanism, never as an application claim.
- Report the **13 saturated will-it-scale regressions as one aggregated
  sentence**, with the rule: no spare capacity and no preempted holder means IVH
  can only cost.
- State the userspace column as **scope** ("not wired up yet"), not as failure.
- The contrast that proves the rule is inside our own data: stress-ng flock
  **+44%** and hackbench **+75%** hit the *same kernel locks* as will-it-scale
  lock1 (**-10%**) and unix1 (**-9%**). The difference is spare capacity and real
  work, not the lock.

---

## 6. Suggested order

1. AFL trylock + condvar **[no reboot]** -- unblocks everything userspace
2. Split-arm run on the regressions **[no reboot]** -- decides the story for §3.2
3. LD_PRELOAD shim **[no reboot]** -- then Phoenix, sysbench, PARSEC dedup in one go
4. kernel build / kernbench + a database + a KV store **[no reboot]** -- fills the
   thin application column, which is what reviewers will ask for
5. Migration eligibility gate + re-measure the 4 fixable regressions
6. Boot and test G-LOCK-31; then build G-LOCK-32 with its F1-F5 fixes
