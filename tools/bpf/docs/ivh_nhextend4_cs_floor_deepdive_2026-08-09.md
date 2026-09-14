# The CS floor, deep dive: it was never the critical-section length

**Kernel** `6.17.0-rseqport73+`, branch `kernel-43-clean`, `nohz=off`, no reboot, no rebuild.
**Harness** `NHextend4.c` (extended in place with three new knobs, all defaulting to NHextend3
behaviour). **Not modified:** `NHextend3.c`, `NHextend3`, `vcap_probe`, `vcap_probe.cpp`,
`/home/nick/IVH`, `ivh_verify.sh`, kernel source. Nothing committed, nothing pushed.

Predecessor: `ivh_nhextend4_cs_floor_2026-08-09.md`. This document contradicts one of that
document's central conclusions and says so explicitly in section 3.

---

## 0. The answer, up front

**No, there is no low-hanging-fruit sysctl combination that helps both NHextend and hackbench.
And the reason is not that we failed to find it — it is that the sysctls were never where the
problem lived.** The best knob found tonight buys NHextend +5.3 % for about −2 % on hackbench
(section 5.4). At the same time, and without touching a single sysctl, workload *structure* moves
the same number by **31 percentage points** (section 3). That ratio is the finding.

### 0.1 The evidenced explanation

At a **fixed** critical-section length — the exact one the predecessor document calls structurally
stuck (`loop_spin=200000`, shipped default measured at −29.2 % there) — **splitting NHextend's one
global lock into 8 independent locks turns the shipped default from −28.6 % into +2.3 %, with no
tuning whatsoever.** Monotone in lock count; replicated in two independent batches (−28.6/−26.9,
−15.7/−15.7, +2.3/+1.0); every clean round agreeing in sign.

The critical section did not get longer. It got *shorter* (483 µs → 349 µs). The migration
mechanism did not change. No sysctl changed. **The CS-length model does not survive this.**

### 0.2 What actually controls the floor

Every cell of that experiment decomposes exactly — algebraically, not as a fit — into two measured
quantities. With `U` = the fraction of time a lock is actually held (`iters × cs_active /
(wall × nlocks)`), throughput is `U·nlocks·wall / CS`, so

> **Δ = (CS shortening IVH buys) × (lock utilisation IVH destroys)**

| cell | CS_off → CS_on | benefit | U_off → U_on | cost | net Δ |
|---|---|---|---|---|---|
| 200 k, 1 lock | 483 → 331 µs | **×1.46** | 0.999 → 0.489 | **×0.49** | **−28.6 %** |
| 200 k, 4 locks | 368 → 345 µs | ×1.07 | 0.951 → 0.753 | ×0.79 | −15.7 % |
| 200 k, 8 locks | 349 → 347 µs | ×1.01 | 0.706 → 0.719 | ×1.02 | **+2.3 %** |
| 400 k, 1 lock | 971 → 658 µs | ×1.48 | 0.997 → 0.657 | ×0.66 | −2.8 % |
| 400 k, 8 locks | 718 → 689 µs | ×1.04 | 0.823 → 0.738 | ×0.90 | **−6.6 %** |

IVH's **benefit** is always the same thing: it shortens the critical section. Its **cost** is
always the same thing: it idles the lock while migrating threads sit in `wait_for_completion()`.
The floor is where they cross. **Neither term is a function of CS length.** Both are functions of
workload structure — how many threads contend one lock, and whether idle destinations exist.

Note the row that stops this being "more locks is always better": **at `loop_spin=400000`,
8 locks is 3.8 pp *worse* than 1.** Lock topology is not a tuning dial. It is the variable the
floor is actually a function of, and it moves both terms at once, in different directions
depending on where you are.

### 0.3 Why the "single lock" theory survives the objection against it

The objection — `ivh_cs_enter()` fires before the lock is touched, so it cannot know or care how
many locks exist — is correct about the *decision* and irrelevant to the *outcome*. Two measured
consequences of topology, neither of which the decision sees:

1. **Staleness.** `grab_lock()` calls `ivh_cs_enter_checked()` as its first statement, then waits
   **13–14 ms** (measured, `avg wait`) for a **0.7–1.0 ms** critical section. The vCPU-danger
   signal it acted on has a ~5–6 ms timescale, so the prediction is 2–3 host timeslices stale
   before it matters. At 8 locks that wait is **0.10 ms**. Kernel-lock callers
   (`ivh_pre_lock()` → `__raw_spin_lock()`, i.e. hackbench and ebizzy-mmap) have a wait of ~0.
2. **Cost of being wrong.** The same `set_cpus_allowed_ptr()` call takes **1092 µs at one lock and
   162 µs at eight**, with 42.7 % vs 0.4 % of migrations stuck over 1 ms — because with one lock
   fifteen of sixteen threads are *spinning*, so the BPF selector's preferred idle destinations do
   not exist. The workload's own waiters consume the capacity its migrations need.

NHextend's single global lock manufactures its own worst case on both sides at once.

On the six threads:

| # | thread | verdict |
|---|---|---|
| 1 | cache/IPC signature | **Not the mechanism here.** NHextend's CS is `loop_spin × sfence` — literally zero memory traffic — so it is the workload least able to show a cold-cache penalty, and it has the worst floor. Section 1. |
| 2 | ebizzy counter-example | **Dissolves, in the user's favour.** ebizzy contains *zero* locks. The arm that wins (+136 %) contends a **kernel** rwsem; the arm whose lock is userspace does **not** win (−2 %). And ebizzy shows the *same* work-size gradient, so it is not a counter-example at all. Section 2. |
| 3 | multi-lock NHextend | **The answer, with a caveat.** −28.6 % → −15.7 % → +2.3 % for 1 → 4 → 8 locks at fixed CS, twice replicated — but it *reverses* at `loop_spin=400000`. Section 3. |
| 4 | rseq grace period | **Hypothesis not supported.** Killing the grace period alone is neutral-to-slightly-worse. But `cr_counter` publication *is* worth ~7 pp — through the BPF destination gate, not the grace period. Section 4. |
| 5 | which sysctl regresses hackbench | **`ivh_eval_cooldown_ns`, unambiguously** (~+23 % vs default on hackbench, worth only +2.6 pp on NHextend). Section 5. |
| 6 | open audit | IVH is migrating *waiters*, not holders, 15/16 of the time; the gate's runway term is fed by the *previous* CS; and three more. Section 6. |

The one usable tuning result is in section 5.4, and it is small.

---

## 1. Cache coherence / IPC — real, measured, and three orders of magnitude too small

### 1.1 What the PMU says, normalised per critical section

The guest has a working virtualised PMU (`cache-references`, `cache-misses`, `L1-dcache-*`,
`dTLB-*` all count; `LLC-*` is `<not supported>`). `perf stat` wrapping the whole harness, counters
land as `:u` — **userspace only**, which for "does the critical section itself suffer" is exactly
the right scope. `loop_spin=200000` is where the floor is failing (−29 %); `600000` is where it
succeeds (+17 %). One `600000` round dropped (a host episode collapsed the IVH arm to 2100 iters).

| `loop_spin` | arm | n | **IPC** | cycles/CS | cache-ref/CS | **cache-miss/CS** | L1d-miss/CS | dTLB-miss/CS |
|---|---|---|---|---|---|---|---|---|
| 200 000 (floor failing) | off | 3 | **0.299** | 17.6 M | 79.4 | **43.9** | 191 | 0.8 |
| 200 000 | IVH on | 3 | **0.292** | 23.2 M | 116.6 | **44.4** | 220 | 1.2 |
| 600 000 (floor succeeding) | off | 2 | **0.293** | 57.3 M | 111.4 | **68.4** | 326 | 1.8 |
| 600 000 | IVH on | 2 | **0.301** | 45.3 M | 126.4 | **65.3** | 304 | 1.7 |

**IPC is flat across all four cells: 0.292–0.301.** The cell where IVH loses 29 % and the cell where
it wins 17 % differ in IPC by less than the difference between the two IVH-off controls. There is
no IPC signature of the floor.

**Cache misses per critical section are unchanged by IVH**: 43.9 → 44.4 at 200 k, 68.4 → 65.3 at
600 k. Forty-four LLC misses per critical section at ~100 ns each is ~4.4 µs of memory stall
against a 331–483 µs critical section — about 1 % — and IVH moves it by **half a miss per CS,
≈ 50 ns**. Cache *references* do rise under IVH (79 → 117 per CS, the migration path's own
footprint plus post-landing coherence), but misses do not, so it costs no measurable time.

### 1.2 The absolute cold-cache penalty on this host, measured directly

`coldcache.c` (scratchpad): warm a pointer-chase working set on the current CPU, then either
`sched_setaffinity()` to a **different** CPU or to the **same** CPU — identical syscall and
scheduler work in both arms — then time one traversal. The difference is cache/TLB locality and
nothing else. 150 trials/arm.

| working set | STAY traversal | MIGRATE traversal | **cold-cache penalty** | affinity-syscall cost, migrate − stay |
|---|---|---|---|---|
| 64 KB | 20.4 µs | 24.0 µs | **+3.6 µs** (+17.8 %) | +36.5 µs |
| 256 KB | 131.2 µs | 134.5 µs | **+3.3 µs** (+2.5 %) | +55.5 µs |
| 1 MB | 734.7 µs | 783.1 µs | **+48.4 µs** (+6.6 %) | +16.7 µs |
| 8 MB | 34.1 ms | 41.6 ms | +7.4 ms (+21.7 %) | +114.2 µs |

The cold-cache penalty is **real** — 3–48 µs for working sets up to 1 MB, and very large once the
working set exceeds TLB reach. The bare migration itself costs 17–116 µs against 0.6–2.4 µs for the
same syscall pinning to the same CPU, which independently corroborates the predecessor's
76–237 µs best-case figure for the IVH path.

### 1.3 The arithmetic that closes it

NHextend's floor gap at `loop_spin=200000`, one lock, is 10331 → 7369 iterations over 5 s across
1262 migrations: **1.13 ms of throughput lost per migration.** The harness's own measured migration
duration in the same runs is **1092 µs**. Those match to within 4 %, so the cost is essentially
*entirely* the blocking wait.

The cache term available to explain it:

* measured directly by PMU, for this workload: **≈ 50 ns per CS**;
* upper-bounded by the microbenchmark, if NHextend's CS had a 1 MB working set (it does not):
  **48 µs**.

Against 1.13 ms that is **~20 000× too small** on the measured figure and **~24× too small** even
on the generous upper bound. **Cache effects do not explain this floor.** The mechanistic reason is
section 6.3: NHextend's critical section is `loop_spin × sfence` with no loads, no stores and no
working set, so it is the workload *least* able to be hurt by a cold cache — and it has the worst
floor this project has measured. If cache locality were the driver, that ordering would be
impossible.

The CS-length-scaling argument in the task framing (a fixed absolute penalty being proportionally
worse for a short CS) is sound in principle and does apply to *some* workload — just not this one,
and not at the magnitude required. It is worth noting for workloads with a real working set: at
1 MB, a 48 µs cold-cache penalty *would* dominate a 50 µs critical section.

## 2. ebizzy: the counter-example dissolves, and it dissolves the user's way

### 2.1 The task brief has the flag inverted, and it matters

`ebizzy -m` is **mmap mode**, not malloc mode — `case 'm': always_mmap = 1;`
(`ebizzy.c:140`), and `alloc_mem()` calls `mmap(MAP_PRIVATE|MAP_ANONYMOUS)` instead of `malloc()`
when it is set (`ebizzy.c:243`). Malloc mode is the *default*, i.e. the arm with **no** `-m`.
This inversion matters because the two arms behave oppositely under IVH.

### 2.2 ebizzy has no locks. None.

A grep of the entire `ebizzy.c` for `pthread_mutex`, `pthread_spin`, `pthread_rwlock`, `atomic`,
`__sync`, or `lock` returns **nothing**. The whole worker is:

```c
for (i = 0; threads_go == 1; i++) {
        chunk = rand_num(chunks, &state);
        copy = alloc_mem(copy_size);          /* mmap() or malloc() */
        memcpy(copy, mem[chunk], copy_size);
        found = bsearch(&key, copy, ...);
        free_mem(copy, copy_size);            /* munmap() or free() */
}
```

The only shared mutable state is the `volatile int threads_go` start flag. **ebizzy never calls
`ivh_cs_enter()`.** Whatever IVH does for ebizzy, it does entirely from inside the kernel.

So ebizzy's "one lock" is implicit, and it is a *different lock in each mode*:

* **`-m` (mmap mode)** — every iteration is an `mmap()`/`munmap()` pair plus a `memcpy` that
  faults the new mapping in. That contends **`mm->mmap_lock`, a kernel per-mm rwsem**, held
  across page faults. IVH reaches it through `ivh_pre_lock()` (`kernel/locking/spinlock.c:266`)
  and the `kernel/locking/mutex.c` hook — kernel-side, fired at the moment of acquisition.
* **default (malloc mode)** — glibc's arena mutex. Not even one lock: glibc creates up to
  `8 × ncores` arenas, and tcache makes the common path lock-free. Userspace, futex-mediated.

### 2.3 The prior data already splits exactly along that line

From `ivh_benchmark_search_2026-07-20.md`, which I did not re-derive but did re-read in full:

| ebizzy arm | what it contends | no-opt | IVH-on | Δ | rounds |
|---|---|---|---|---|---|
| `-S 12 -t 16 -m` (mmap) | **kernel** `mmap_lock` rwsem | 6420 rec/s | 15166 | **+136 %** | 4 |
| `-S 12 -t 16` (malloc) | **userspace** glibc arena mutex | 969k rec/s | 949k | **−2 %** | 2 |

**The ebizzy arm that wins is the kernel-lock arm. The arm whose lock lives in userspace does not
win.** ebizzy is therefore not a counter-example to NHextend's floor — it is a *second instance
of the same split*, and it confirms the user's structural hypothesis rather than testing it.

### 2.4 Live chunk-size sweep — and ebizzy has the same floor

`-s chunk_size` is ebizzy's `loop_spin`: it sets how much work sits inside each implicit critical
section. Both modes swept over a 1024× range, `-S 6 -t 16`, 3 rounds, arms rotated within each
(round, chunk), corunner-verified per run.

**mmap mode (`-m`) — kernel `mmap_lock`:**

| chunk | µs per record | IVH-off rec/s | IVH-on rec/s | **Δ** | IVH migrations |
|---|---|---|---|---|---|
| 4 KB | 135 | 118508 | 119846 | **+1.15 %** (sd 2.4) | 8788 |
| 64 KB | 621 | 25774 | 27353 | +6.39 % (sd 16.6) | 69100 |
| 512 KB | 2609 | 6184 | 10885 | **+78.0 %** (sd 29.0) | 56576 |
| 4 MB | 9438 | 1703 | 2905 | **+70.9 %** (sd 8.4) | 11951 |

**malloc mode (default) — glibc arena mutex, userspace:**

| chunk | µs per record | IVH-off rec/s | IVH-on rec/s | Δ | IVH migrations |
|---|---|---|---|---|---|
| 4 KB | 0.12 | 137.1 M | 161.5 M | +19.9 % (sd **18.8**) | **97** |
| 64 KB | 2.12 | 7.55 M | 7.46 M | **−1.20 %** (sd 0.8) | **123** |
| 512 KB | 15.8 | 1.011 M | 0.998 M | **−1.26 %** (sd 0.5) | **137** |
| 4 MB | 498 | 35442 | 40711 | +25.6 % (sd **47.5**) | **130** |

Three things fall out, and all three cut against the premise the task was built on:

1. **ebizzy has a floor too, and it is in the same place.** At its shortest work unit (135 µs/record)
   the mmap-mode win is **+1.15 %, indistinguishable from zero** — the same size as NHextend's best
   result at 8 locks (+1.7 %). ebizzy is not a workload that "benefits at single-digit-µs CS while
   NHextend cannot". It benefits at *hundreds of µs to milliseconds* of work per unit, exactly like
   NHextend does, and stops benefiting below that, exactly like NHextend does.
2. **Malloc mode is not a lock story at all — IVH essentially never fires.** 97–137 migrations for
   the whole run, against 8788–69100 in mmap mode: a factor of 100–700. With no kernel lock in the
   hot path there is no `ivh_pre_lock()` to hook, and ebizzy never calls `ivh_cs_enter()`. The two
   low-variance malloc rows (sd 0.8 and 0.5) read **−1.2 %**, reproducing the prior document's −2 %.
   The two high-variance rows (sd 18.8 and 47.5) are noise on an embarrassingly-parallel userspace
   memcpy and carry no signal.
3. **The single-digit-µs regime does exist here — in malloc mode, at 0.12 µs/record — and IVH does
   literally nothing in it** (97 migrations in 6 s). That is the honest resolution of the
   predecessor document's open question 9: there is no workload in this project's set that is both
   at single-digit-µs CS *and* being helped by IVH. The recollection conflated ebizzy's *mode*.

**So ebizzy is not a counter-example to NHextend's floor. It is the same phenomenon, in a workload
whose winning mode happens to be built on a kernel lock — which buys it a decision-to-CS delay of
zero (section 3.3) and page-fault-length hold times, and therefore a floor a couple of hundred
microseconds lower, not a couple of orders lower.**

## 3. Lock topology is the floor

### 3.1 The experiment

`NHextend4.c` gains `NH4_LOCKS=N`: the same 16 worker threads, the same critical-section body,
the same inter-iteration sleep, split across `N` independent locks (thread `i` uses lock
`i % N`), each lock in its own 64-byte cacheline so distinct locks never false-share.
`NH4_LOCKS=1` is bit-identical to NHextend3 — verified, and it is what every other batch in this
document runs.

`loop_spin=200000` was chosen deliberately: it is where the predecessor document puts the shipped
default at **−29.2 %**, well below the floor, in the regime it calls structurally stuck.

**Shipped-default sysctls throughout. No tuning of any kind.** 5 rounds, arms interleaved and
rotated within each round, 0 rounds dropped by the pre-declared contamination rule.

### 3.2 The result, and its independent replication

| locks × threads | IVH-off iters | IVH-on iters | **Δ batch A** (n=5) | **Δ batch B** (n=4/4/5) | IVH-off **wait** | migrations | mig avg | stuck > 1 ms |
|---|---|---|---|---|---|---|---|---|
| **1 × 16** | 10331 | 7369 | **−28.64 %** (sd 3.5) | **−26.87 %** (sd 2.4) | **6.45 ms** | 1262 | **1092 µs** | **42.7 %** |
| **4 × 4** | 51720 | 43604 | **−15.69 %** (sd 1.5) | **−15.73 %** (sd 3.3) | 0.52 ms | 19380 | 484 µs | 16.1 % |
| **8 × 2** | 81026 | 82896 | **+2.31 %** (sd 0.9) | **+0.95 %** (sd 1.5) | **0.10 ms** | 30099 | **162 µs** | **0.4 %** |

Every clean round in every cell agrees in sign; monotone in lock count on every column; the 4-lock
cell reproduces to two decimal places across batches.

**Contamination rule**, declared before analysis and — unlike the predecessor's — applied to
**both** arms, because batch B caught two host episodes that inflated the *control*, which an
IVH-arm-only rule cannot see: drop a round from a lock-count cell if either arm's `cs_active`
exceeds 1.3× the median `cs_active` of that cell's IVH-off arms. It flagged exactly two cells, at
1.57× and 2.07×; every other cell is ≤ 1.06×. Clean separation, no judgement call. Batch A loses
zero rounds under the same rule.

**The critical section did not get longer — it got shorter.** IVH-off `cs_active` is 483 µs at one
lock and 349 µs at eight (section 3.6 explains why). By the predecessor document's own table a
*shorter* CS should be *worse*. It is better.

### 3.2b It reverses at `loop_spin=400000`, and that is not a problem for the conclusion

Repeating the experiment at the longer CS, 4 rounds, same rule:

| locks | Δ | IVH-off wait | migrations | mig avg | stuck > 1 ms |
|---|---|---|---|---|---|
| 1 × 16 | −2.76 % (sd 1.8) | 13.70 ms | 587 | 1238 µs | 45.5 % |
| 8 × 2 | **−6.60 %** (sd 1.4) | 0.29 ms | 18612 | 433 µs | 17.1 % |

**At 400 k, eight locks is 3.8 pp *worse* than one.** So "more locks is better" is emphatically
*not* the finding, and I am not going to state it that way. The finding is that lock topology is
the variable the floor is a function of — and it moves **both** terms of section 0.2 at once, in
directions that depend on where you are. At 400 k, splitting the lock removes almost all of the
CS-shortening benefit (×1.04) while migration is still expensive (433 µs, 17 % stuck — at a ~70 %
per-thread duty cycle there are fewer idle destinations than at 200 k's ~54 %), so the trade goes
the other way.

What survives both cells, and is the actual claim: **the floor is not a function of
critical-section length.** At 200 k, one lock is −28 % and eight locks is +2 %. At 400 k, one lock
is −2.8 % and eight is −6.6 %. Four cells, two CS lengths, and CS length orders them in neither
direction.

### 3.3 Why: the decision-to-CS delay

`grab_lock()` calls `ivh_cs_enter_checked()` as its **first** statement (`NHextend4.c:589`) —
before `wait_enter()`, before the spin loop, before any `cmpxchg`. Everything the objection says
is true: the decision cannot see the lock, the holder, or the contention level.

What it also cannot see is **how long it will be until the critical section it is protecting
actually begins**. That interval is printed by the harness and it is enormous:

```
Total wait time: 69.987516  (avg: 0.013975)     # loop_spin=400000, 1 lock
```

**13.1–14.4 ms of mean wait, against a 0.67–0.97 ms critical section.** Sixteen threads round-robin
one lock at 99.8 % utilisation, so each thread queues behind roughly fifteen others. IVH evaluates
"is my vCPU about to be stolen", migrates, and then the critical section that was supposed to
benefit starts **14 ms later** — two to three host timeslices (~5–6 ms) after the signal it acted
on. The prediction is not merely imperfect at that horizon; it is uncorrelated with the state it
has to be right about.

Splitting the lock collapses exactly that interval, and nothing else about the mechanism:

| locks | decision-to-CS delay (IVH-off wait) | Δ |
|---|---|---|
| 1 | 6.45 ms | −28.6 % |
| 4 | 0.52 ms | −15.7 % |
| 8 | 0.10 ms | +2.3 % |

For comparison, the kernel-lock callers this project's wins come from — hackbench, ebizzy-mmap —
go `ivh_pre_lock()` → `__raw_spin_lock()` with a delay of **effectively zero**. That is the whole
difference in entry point, and it is why the same mechanism reads +136 % on ebizzy-mmap and
−28.6 % on NHextend at a comparable nominal CS length.

**This is the reconciliation of the objection.** Lock topology never enters the decision. It sets
(a) how stale the decision is by the time it matters and (b) what the mistake costs — below.

### 3.4 Why: migration cost is not a constant of `set_cpus_allowed_ptr()` either

The predecessor document roots the cost floor in `bpf_sched_pre_lock_migrate()` →
`set_cpus_allowed_ptr()` → `affine_move_task()` → `wait_for_completion()`, and calls ~10² µs a
scheduler-structural floor. The mechanism is right; the number is not a constant:

**1092 µs → 484 µs → 162 µs, and 42.7 % → 16.1 % → 0.4 % of migrations stuck over 1 ms — same
call, same kernel, same sysctls, purely from splitting the lock.**

The reason is visible in the same table. With one lock, fifteen of sixteen threads are *spinning*
at any instant, so every candidate destination is busy and `wait_for_completion()` waits out the
stopper thread **plus the destination's scheduling latency behind a running spinner**. The BPF
selector's strong preference for idle targets (`MY_ivh_atc.bpf.c:717-740`, `ACC_TIER2_IDLE` stops
the scan; the confusingly-named `ACC_TIER1_ACTIVE` is only recorded as a fallback) has nothing to
select: there are no idle vCPUs, because the workload's own waiters have consumed them. Split the
lock and waits become short, threads spend their time in `nanosleep` rather than spinning, real
idle destinations exist, and the identical migration completes ~7× faster.

So NHextend's single global lock **manufactures its own worst case on both sides at once**: it
maximises the staleness of the decision *and* it destroys the idle capacity the migration needs to
land cheaply.

### 3.5 And the holder is off-CPU for half its own critical section

`cs_overall / cs_active` is how much of the hold the holder spent off-CPU. Under IVH at one lock
it is **2.05** — the holder is off-CPU for *half its critical section*. At eight locks it is 1.04.
IVH is not merely failing to help at one lock; the migrations it induces in the other fifteen
threads are landing on, and preempting, the holder.

### 3.6 A control: 40 % of NHextend's "critical section" is its own spinners

The `cs_active` differences above needed a control, because if the CS length itself is a function
of topology then the decomposition's "CS shortening" term is measuring something other than LHP
relief. **IVH off throughout, one lock, `loop_spin=200000`, varying only the thread count:**

| threads on one lock | sibling spinners | `cs_active` (2 runs) |
|---|---|---|
| 2 | 1 | 350 / 344 µs |
| 4 | 3 | 355 / 368 µs |
| 8 | 7 | 428 / 402 µs |
| 16 | 15 | **479 / 487 µs** |
| **16 threads, 8 locks** | 1 per lock | **342 / 342 µs** |

Monotone, and the 16-thread/8-lock cell lands exactly on the 2-thread/1-lock value. **The
"critical section" at one lock with sixteen threads is inflated ~40 % (342 → 483 µs) purely by the
on-CPU cost the fifteen sibling spinners impose on the holder** — they are in a tight
`lfence` + load loop on the *same lock cacheline*. It is a pure function of spinners-per-lock,
measured with IVH off on both sides, and it vanishes the moment the spinners are spread.

That matters for reading section 0.2 honestly: **the ×1.46 "CS shortening" IVH buys at one lock is
not all lock-holder-preemption relief.** Under IVH the measured CS drops to 331 µs — *below* the
1-spinner value of 342 µs — so IVH is removing essentially all of the spinner-induced inflation as
well. I have not separated the two contributions further and am not going to guess at the split.
The consequence for the floor question is the same either way: at one lock NHextend is partly
paying IVH to repair damage the harness inflicts on itself, and that repair is worth less than the
lock idling it costs.

## 4. The rseq extend / grace-period hypothesis — not supported, but it found something else

### 4.1 What the mechanism actually is

Reading it end to end first, because it decomposes into **two separable effects** that the
hypothesis conflates:

* **Userspace side.** `extend()` is `inc_extend(&rseq_map->cr_counter)` — one non-atomic
  `addl $4, (byte*)cr_counter` to a thread-local cacheline (`NHextend4.c:428`). `unextend()` is
  the matching `subl`, plus a test of bit 1 (the kernel's request-to-yield), and on a hit a
  `sched_yield()`. The publish cost is a single store. It is not plausibly the problem, and the
  measurement below confirms it.
* **Kernel side, effect (a) — the grace period.** `rseq_delay_resched()` (`kernel/rseq.c:535`)
  fires from `exit_to_user_mode_loop()` on `_TIF_NEED_RESCHED_LAZY`, and only if `cr_counter`'s
  in-critical-section bit is set. It sets the KERNEL_REQUEST_SCHED bit and arms
  `hrtick_local_start(rseq_sched_extend_usec)` — **50 µs by default, and a live sysctl**
  (`kernel.rseq_sched_extend_usec`), so this is testable with no rebuild. Note the size: 50 µs
  against NHextend's 0.33–0.97 ms critical section is at most ~5–15 % of one CS. As protection it
  cannot be decisive at these lengths by construction.
* **Kernel side, effect (b) — the BPF destination gate.** `MY_ivh_atc.bpf.c` (`REJ_USER_LOCKHOLDER`,
  ~line 571, `GATE_LOCKHOLDER=1`) `bpf_probe_read_user()`s the *target CPU's* current task's
  `cr_counter` and refuses to migrate onto a vCPU that is running a userspace lock holder.
  `task->lock_depth` is only maintained for *kernel* spinlocks, so **`cr_counter` is this gate's
  only input.** Stop publishing it and the gate goes blind.

`rseq_sched_extend_usec=0` kills (a) alone. `NH4_NO_EXTEND=1` (new knob) kills (a) and (b).
Running both separates them.

### 4.2 The result, two independent batches

`loop_spin=400000`, Δ vs same-round IVH-off; and the derived Δ vs same-round shipped default.

| arm | batch A (n=3–4) | batch B (n=5) | vs default (A / B) |
|---|---|---|---|
| shipped default | +1.18 % (sd 4.0) | −2.03 % (sd 2.3) | — |
| `rseq_sched_extend_usec=0` (grace period off) | −1.22 % (sd 2.4) | −3.11 % (sd 2.5) | **−2.4 pp / −1.1 pp** |
| `NH4_NO_EXTEND=1` (no `cr_counter` at all) | −6.07 % (sd 4.2) | −8.82 % (sd 3.3) | **−7.3 pp / −6.8 pp** |
| `NH4_NO_WAITCNT=1` (no `wait_counter`) | +0.35 % (sd 2.4) | — | −0.8 pp |

Both batches agree on both arms.

**The hypothesis is not supported.** Turning the grace period off does not help at short CS — it is
neutral to *slightly worse* (−1 to −2.4 pp, inside the batch spread). The grace period is a small
positive, not a hidden cost, and it is not what is wrong at short CS.

**What is load-bearing is `cr_counter` publication — worth ~7 pp — and it earns that through the
BPF destination gate, not the grace period.** With `NH4_NO_EXTEND=1` the migrant no longer knows
which vCPUs are running lock holders, lands on them, and (per the gate's own source comment) the
enqueue-path wakeup preemption sets a *hard* need-resched that the rseq extension cannot defer.
NHextend's host-preempted CS rate went 0.53 % → 2.02 % in batch A, a 4× rise, exactly as the
source predicts. `wait_counter` (feeding `rq->user_waiter`) is not load-bearing here.

So: the extend mechanism's *documented* purpose (buy the holder 50 µs) is nearly irrelevant at
these CS lengths, while its *side effect* (publishing where the holders are) is worth 7 percentage
points. That is worth knowing independently of the floor question.

## 5. Which of the three tuned sysctls does the damage

Each of `ivh_time_left_threshold_ns=200000` (THR), `ivh_eval_cooldown_ns=1000000` (CD) and
`ivh_max_concurrent=4` (MC) tested **individually**, other two at shipped defaults, against both
workloads, interleaved and rotated within each round.

### 5.1 NHextend4 @ `loop_spin=400000`, Δ vs same-round IVH-off

| arm | batch A (n=4) | batch B (n=6) | pooled | migrations | mig avg |
|---|---|---|---|---|---|
| shipped default | −5.50 % (sd 8.9) | +0.38 % (sd 2.2) | ~ −2 % (break-even) | 2000–3500 | 413–686 µs |
| **THR alone** | **+4.31 %** (sd 4.0) | **+5.94 %** (sd 4.2) | **~ +5.3 %** | 1103–1522 | **140–198 µs** |
| CD alone | −0.04 % (sd 4.7) | — | ~ 0 % | 1795 | 544 µs |
| MC alone | −1.62 % (sd 5.2) | — | ~ −1.6 % | 2545 | 612 µs |
| THR + MC | — | +6.30 % (sd 2.3) | +6.3 % | 1172 | 164 µs |
| all three ("tuned") | +8.49 % (sd 5.9) | +8.92 % (sd 2.0) | **~ +8.7 %** | 1497 | 96–111 µs |

**On NHextend the load-bearing knob is `ivh_time_left_threshold_ns`.** It carries about 60 % of
the tuned gain on its own, and the way it does it is by collapsing *migration cost*
(413–686 µs → 140–198 µs), not by changing how many migrations happen in any dramatic way. The
`eval_cooldown` knob is worth roughly **+2.6 pp** on top.

### 5.2 hackbench (`ivh_exec -v hackbench -T -g 1 -f 8 -l 400000`), Δ vs same-round shipped default

Cost *relative to the shipped default*, which is the number that matters — the default already
wins −25 to −28 % against IVH-off here, so "regression" means giving that win back.

| arm | per-round Δ vs same-round default | mean |
|---|---|---|
| **THR alone** (batches A + C) | +1.3, +4.3, −3.4, +8.3, +0.3, +2.6 | **~ +2.2 %** |
| MC alone | +5.4, +2.0, −1.2 | ~ +2.1 % (near free) |
| **CD alone** | **+27.7, +27.3, −1.2, +29.2, +26.0, +27.3** | **~ +22.9 %** |
| THR + MC | +14.9, +12.4, +15.0 | ~ +14.1 % |
| all three ("tuned") | +46.0, +31.6, −1.2 | ~ +25.5 % |

**`ivh_eval_cooldown_ns` is the hackbench-killer, on its own, reproducibly, in 5 of 6 rounds
across two independent batches.** Batch A round 3 is the single dissenting round and it is a
different host regime — every IVH-on arm collapsed onto ~11.39 s regardless of knob. It is kept
in, and keeping it in only softens the CD and tuned numbers.

THR got a third, dedicated batch (C) because section 5.4 rests on it. One of its four rounds was
dropped: the *default* arm did **zero** migrations and clean-vCPU `ivh_uc_capacity` collapsed
999 → 517, a host episode rather than a knob effect. The three clean rounds moved THR's pooled
cost from ~+0.7 % (batch A alone) to **~+2.2 %**, which is why section 5.4 no longer calls it
free.

### 5.3 Why — from steal telemetry that is *not* `ivh_steal_source`

`/proc/stat`'s steal column comes from `paravirt_steal_clock()` via
`steal_account_process_time()` (`kernel/sched/cputime.c`), i.e. the real KVM `steal_time` page
written by the hypervisor. It is **not** `rq->ivh_tks_steal_ns` and is therefore independent of
`ivh_steal_source` — unlike NHextend's own printed "Host-preempted CS cycles … ground truth",
which at the shipped `ivh_steal_source=2` returns the TSC estimator's own output
(`get_steal_and_preemptions()`, `kernel/sched/core.c:443`). Every mechanism claim below rests on
the former; NHextend's `hp %` is quoted elsewhere in this document only as a relative
within-batch signal.

Contended-vCPU (cpu0–7) steal measured *during* each hackbench run:

| arm | contended steal | wall |
|---|---|---|
| IVH off | 36–40 % | 15.3–16.6 s |
| shipped default | 1.5–5.9 % | 11.2–11.6 s |
| CD alone | 15–17 % | 14.4–15.3 s |
| THR + MC | 12–15 % | 12.8–13.0 s |
| all three | 21–24 % | 15.8–16.9 s |

Wall time tracks residual contended-vCPU steal almost perfectly across all five arms.
**hackbench's IVH win *is* evacuation of the contended vCPUs** — 73 000 migrations in 11.6 s,
6300/s — and `ivh_eval_cooldown_ok()` (`kernel/sched/fair.c`) is a **per-vCPU** rate limiter on
evaluation. Raising it from 50 µs to 1 ms caps each vCPU at ~1000 evaluations/s, which throttles
evacuation directly. NHextend does a few thousand migrations in 5 s and does not care.

### 5.4 The one Pareto-safe setting, and why it is not the win the question was after

`ivh_time_left_threshold_ns=200000` **alone**: **+5.3 % on NHextend** (two independent batches,
+4.31 % and +5.94 %, both positive, pooled n=10) at a cost of **~+2.2 % on hackbench** (two
batches, six clean rounds: +1.3, +4.3, −3.4, +8.3, +0.3, +2.6).

That clears the project's ≥ 5 % bar on NHextend, and its hackbench cost is small but — after
replication — **not zero**; the first batch alone read ~+0.7 % and that was too optimistic. It is
still much the best of the three knobs: the "tuned" bundle buys +3.4 pp more on NHextend for +25 %
on hackbench.

**It is not a both-ways win and I will not present it as one.** It does not make hackbench faster;
it makes it slightly slower, inside but near the edge of noise. And it is worth being blunt about
the size: +5.3 % moves NHextend's floor a little; section 3 moves it by **31 percentage points** at
the same CS length without touching a single sysctl. The sysctl axis is the wrong axis, and that
is the finding, not the +5.3 %.

**Do not ship `eval_cooldown=1000000`.** It is the single most damaging knob of the three on the
workload class this project's real wins come from, and it is worth +2.6 pp on the one harness.

## 6. Open audit — five further structural findings

### 6.1 IVH is overwhelmingly migrating *waiters*, not holders

`ivh_cs_enter_checked()` is called once per `grab_lock()`, by **every** thread, on **every**
iteration — before the wait. Fifteen of every sixteen calls are made by a thread that is about to
spin for ~14 ms and then take the lock; only one is made by a thread that will hold it soon.
Protecting a *waiter* from host preemption is worth nothing to anybody — a preempted spinner costs
only itself — while the migration it triggers is charged to the one serialized resource everyone
needs. The mechanism's name is "pre-lock migration", and on this harness it is, 15/16 of the time,
pre-*wait* migration.

This is not a bug in IVH. It is what happens when a mechanism designed for "acquire the lock on a
good vCPU" is invoked by a workload where "acquire the lock" is 14 ms away.

### 6.2 The gate's runway term is fed by the *previous* critical section

`sys_ivh_cs_enter()` (`kernel/sched/bpf_sched.c:847`) reads the caller's
`rseq->last_cs_overall_ns` to feed `current->last_cs_ns`, which
`ivh_gate_time_left_reject()` (`kernel/sched/fair.c:13323`) uses as
`time_left = runway − last_cs_ns`. At `ivh_time_left_source=1` (the live value) that is the
retrospective duration of the CS this thread *last* completed. Combined with 6.1 and 3.3, the
gate is answering "will my vCPU survive a critical section as long as my previous one, starting
now" — when the critical section will not start now, and when the caller is usually not going to
be the holder.

That also explains *why* `ivh_time_left_threshold_ns=200000` is the knob that helps (section 5.1):
shrinking the window from 4 ms to 200 µs is, functionally, refusing to act on the stale prediction
except when preemption is genuinely imminent. It is a staleness filter, not a better predictor.

### 6.3 NHextend's critical section has, by construction, no working set

The CS body is `for (i = 0; i < loop_spin; i++) wmb();` with `#define wmb() asm volatile("sfence")`
(`NHextend4.c:317`). No loads, no stores, no data at all. Section 1's measurement has to be read
against that: NHextend is the workload *least* able to be hurt by a cold cache, and it has the
worst floor of anything this project has measured. Whatever else is true, cache locality is not
what is failing here.

### 6.4 The harness's own instrumentation sits inside the serialized region

`read_vcap_steal()` — a `pread` of `/proc/vcap_info` plus a `strtok`/`sscanf` parse of ~4·ncpu
lines — runs *inside the lock hold* (`grab_lock()`, after acquisition, before the CS body). The
predecessor document found this and added `NH4_VCAP_EVERY`. It remains at 1 (every CS) in every
measurement here, deliberately, so these numbers are comparable to the predecessor's — but it
means the *true* serialized region is somewhat longer than `loop_spin` alone implies, in every
arm equally. `tracefs_printf()` was checked and is a no-op stub (`NHextend4.c:13`), so it is not
an additional cost.

### 6.5 What would actually move the floor, and what would not

Given sections 3–5, ranked by evidence:

1. **Move the `ivh_cs_enter()` call site to after the wait, not before it.** The one change that
   directly attacks the 14 ms staleness: call it when the `cmpxchg` succeeds, not when the thread
   starts waiting. This is a *harness* change, and it changes what the benchmark measures, so it
   is not a fix to IVH — but it would tell you how much of NHextend's floor is staleness and how
   much is everything else. Not run tonight; it is the obvious next experiment.
2. **A cheaper migration primitive.** Section 3.4 shows the 10² µs floor is not fixed —
   it is 162 µs when idle destinations exist and 1092 µs when they do not. The lever is
   destination availability, not `set_cpus_allowed_ptr()` itself.
3. **Not** better prediction. At the shipped default the detector already drives host-preempted CS
   to 0.02–0.03 % (predecessor §7.2). Prediction quality is not the binding constraint.
4. **Not** the rseq grace period (section 4), **not** cache locality (sections 1, 6.3), **not**
   more sysctl sweeping (section 5).

---

## 7. What I did not establish, and where the uncertainty is

- **The Pareto claim on `ivh_time_left_threshold_ns=200000` weakened on replication.** Batch A's
  three hackbench rounds gave ~+0.7 % cost vs default; batch C's three clean rounds gave +8.3,
  +0.3, +2.6. Pooled over six clean rounds it is **~+2.2 %, range −3.4 % to +8.3 %**. So it is
  "small and noisy", not "free". Section 5.4 states it that way. One batch-C round was dropped:
  the default arm did **zero** migrations and clean-vCPU `ivh_uc_capacity` collapsed from 999 to
  517 — a host episode, not a knob effect.
- **`ivh_max_concurrent=4` and `ivh_eval_cooldown_ns` got one NHextend batch each**, not two.
  Their individual NHextend numbers (−1.62 %, −0.04 %) are single-batch and should be treated as
  weaker than the THR and tuned numbers, which have two batches each.
- **The 400 k lock-topology cell has n=4 in one batch**, against n=9 pooled at 200 k. The
  *reversal* is consistent within it (sd 1.4 and 1.8, non-overlapping), but it has not been
  independently replicated the way the 200 k result has.
- **ebizzy's 64 KB and 4 MB malloc-mode rows are noise** (sd 16.6, 18.8, 47.5). Only the two
  low-variance malloc rows and the mmap-mode trend are load-bearing.
- **`perf` counters are `:u` (userspace only).** The migration path's own kernel-side cache
  behaviour is therefore *not* in section 1's table. That scoping is right for "does the critical
  section suffer", and section 1.2's microbenchmark covers the migration itself independently, but
  a kernel-inclusive profile was not taken.
- **I did not move the `ivh_cs_enter()` call site** to after the wait (section 6.5 item 1). That is
  the direct test of the staleness mechanism and it is the obvious next experiment; it is a harness
  change, so it needs a decision about whether NHextend is then still measuring the same thing.
- **The host contention regime drifted materially over the session**, as it did for the
  predecessor: full-load corunner steal on cpu0–7 read **67.6 %** at the start of this session
  against the 32–37 % the predecessor's hackbench runs saw. Every comparison here is therefore
  *within* an interleaved, rotated batch, and cross-batch absolute numbers are not comparable.
- **NHextend's printed "Host-preempted CS cycles … ground truth" is not independent** of
  `ivh_steal_source` at the shipped value 2 (`get_steal_and_preemptions()`,
  `kernel/sched/core.c:443`, returns `rq->ivh_tks_steal_ns`). It is quoted in this document only as
  a relative within-batch signal. Every *mechanism* claim rests instead on `/proc/stat`'s steal
  column, which is `paravirt_steal_clock()` via `steal_account_process_time()`
  (`kernel/sched/cputime.c`) — the real KVM `steal_time` page, and genuinely independent.

## 8. Files and state

**Modified:** `NHextend4.c` only — three new env knobs, all defaulting to NHextend3 behaviour:
- `NH4_LOCKS=N` (default 1) — split the workers across N cacheline-isolated independent locks.
- `NH4_NO_EXTEND=1` (default 0) — never publish `rseq->cr_counter`.
- `NH4_NO_WAITCNT=1` (default 0) — never publish `rseq->wait_counter`.

Binary `/home/nick/NHextend4_dd`. `NH4_LOCKS=1` verified identical in behaviour to `NHextend4_exp`.

**Not modified:** `NHextend3.c`, `NHextend3`, `vcap_probe`, `vcap_probe.cpp`, `/home/nick/IVH`,
`ivh_verify.sh`, `MY_ivh_atc.bpf.c`, any kernel source. **Nothing committed, nothing pushed. No
reboot, no rebuild.** `/home/nick/IVH` was deliberately left alone: nothing found tonight is a
confirmed, reproduced, genuinely-better default — the one candidate knob is worth +5.3 % on one
harness for ~−2 % on the workload class the project's real wins come from, which does not meet
that bar.

**Scratchpad** (`…/4ab2961e-…/scratchpad/`): `dd.sh` (NHextend runner, per-run corunner check via
`/proc/stat`), `hbiso.sh` (hackbench arm-isolation runner), `ez.sh` (ebizzy sweep), `pf.sh` (perf
stat runner), `coldcache.c` (cold-cache microbenchmark), `t3agg.sh` / `ddagg.sh` / `model.sh`
(aggregators), `dd_*.csv`, `hbiso_*.csv`, `ez_*.csv`, `pf_*.csv`, `ddrun_*.txt`, `pfrun_*.perf`.
