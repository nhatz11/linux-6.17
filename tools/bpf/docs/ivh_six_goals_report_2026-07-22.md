# IVH: six-goal dispatch — syscall-skip crossover, PARSEC re-sweep, tick-migration verdict, TPAUSE/MCS design, one-line-fix reality check, steal-time-to-TSC survey

Date: 2026-07-22. Kernel `6.17.0-rseqport55-trimsys+` (confirmed via `uname -r`),
branch `kernel-43-clean`. Companion to `ivh_migration_cost_and_parsec_analysis_2026-07-22.md`
and `ivh_syscall_skip_throughput_analysis_2026-07-22.md` — read those first for
the migration-cost model and matched-pair binary provenance this doc reuses.

**Session condition, and why it matters for every number below**: a co-running
VM was generating real host-level contention via `sysbench` for the entire
session. `/proc/vcap_info` showed live, non-trivial steal (HP% up to ~11.7% was
observed in one arm of the Goal 1 sweep — far above the <0.01% "quiet box"
baseline this project's earlier docs measured against). This is a *different*,
noisier background than several prior sweeps in this project's history, and it
is called out explicitly wherever it plausibly changes a conclusion, rather
than silently blended in.

---

## Top summary — what to do next, per goal

| # | Goal | Kernel change made? | What to do next | What to read / check |
|---|---|---|---|---|
| 1 | Syscall-skip crossover point | **N** (measurement only) | Nothing to rebuild. Read the real numbers in §1 — the honest verdict is **mixed, not a clean win**, and there is a plausible, numbers-grounded reason why. | §1's table; `goal1_results.txt` in this session's scratchpad if you want the raw per-round output. |
| 2 | Can PARSEC dedup/vips be tuned into a win? | **N** (measurement only) | Nothing to rebuild. Read §2 — answer is **no**, across 5 threshold values on real, live-contention hardware, for both workloads. | §2's tables. |
| 3 | Tick-driven mid-wait migration — worth pursuing? | **N** (design reasoning only, no code touched) | Nothing to build. Read §3's verdict: **drop it** — not "fold into goal 4/5", literally dead, for a reason distinct from the one the user's reframing anticipated. | §3 in full — the reasoning is the deliverable. |
| 4 | TPAUSE/MCS-predecessor adaptive spin design | **N** (design + a definitive coverage grep; no code written) | This is real, unimplemented kernel design work. If you want to build it: start with the **role-C-only** version (real MCS queue waiters, 2nd+ node) — the plumbing already exists (`pv_wait_node`/`pv_kick_node`/`pv_wait_early`, `arch/x86/kernel/kvm.c`'s `ivh_pv_wait_mechanism`). Roles A/B (pending-bit waiter, queue head) need new owner-CPU bookkeeping that doesn't exist yet — do not attempt those without deciding whether that's in scope. | §4's design sketch, real struct fields/functions named throughout; §4.4's coverage table for the separate `ivh_pre_lock`/`cs_enter`/`cs_exit` question. |
| 5 | "One-line" TPAUSE-on-no-target fix | **N** (investigated, not applied — recommend against building it as scoped) | Nothing to build as literally requested. If pursued at all, it should be redesigned as UMONITOR+TPAUSE on `&lock->val`, which is really "goal 4 lite," not a standalone one-liner. | §5 — exact line numbers and the reasoning for why "just add TPAUSE here" under-delivers. |
| 6 | Steal-time → TSC | **N** (survey + design reasoning only) | Nothing to build. If the professor's proposal proceeds, the concrete next step is a **userspace prototype** (like `NHextend3`'s own `calibrate_tsc()`) measuring the false-positive/negative rate of a TSC-elapsed heuristic against `/proc/vcap_info` ground truth, before touching kernel code. | §6 — survey table of every steal-time site, the CPUID/clocksource facts specific to this VM, and the concrete heuristic sketch. |

**Bottom line up front** (expanded in the closing section): this dispatch does
**not** close out "the current mechanism" — but it also doesn't uniformly open
a big new phase either. Goals 1–3 are cleanup/closure (one confirms a mixed,
non-clean result; one confirms a negative; one kills an idea outright). Goals
4–6 are real, novel, unimplemented design surfaces — but goal 5, on inspection,
collapses into a smaller piece of goal 4 rather than standing on its own, and
goal 6 is farther from being "new work" than it looks, because a serious
prototype of it is a **userspace** measurement task before it's a kernel task.
Engagement with the "4 and 6 are a new phase" framing is in the closing
section — the short version is: agreed for 4, more qualified for 6.

---

## Goal 1 — does the syscall-skip optimization move NHextend3's loss/win crossover point?

### Method

Used the matched-pair binary from the prior dispatch (`NH_checked`, compiled
from the *current* `NHextend3.c` source, default build = syscall-skip
optimization active), still present in this session's scratchpad
(`/tmp/.../scratchpad/NH_checked`, verified identical-except-one-`#ifdef` to
current `NHextend3.c` via `diff` before use). Swept
`NHEXTEND_LOOP_SPIN` ∈ {600000, 100000, 50000, 25000, 10000, 5000},
`ivh_universal_eligible` ∈ {0 (off), 1 (on)}, 3 rounds each,
`NHEXTEND_DURATION=20`, `-n -l 16` (unpinned, 16 threads = 16 vCPUs, `-l`
print-last-stats flag — this VM's `nproc` is 16). Standard sysctls held
constant throughout (`ivh_capacity_threshold=1010`,
`ivh_time_left_threshold_ns=4000000`, `ivh_max_concurrent=8`,
`ivh_time_left_source=1`, `ivh_selection_trylock=0`, `ivh_migrate_mechanism=0`,
`ivh_eval_cooldown_ns=50000`). `ivh_universal_eligible` restored to 0 after.

### Results (real numbers, all 3 rounds shown, averages)

| loop_spin (≈CS) | IVH-off avg throughput | IVH-off HP% | IVH-on avg throughput | IVH-on HP% | IVH-on avg migrations/20s | **Δ throughput (on vs off)** | Historical (pre-opt) Δ |
|---|---|---|---|---|---|---|---|
| 600,000 (~1.6ms) | 13,897 (13964,13897,13830) | 11.74% | 15,073 (15134,15095,14989) | 1.67% | 2,171 | **+8.46%** | +21% to +35% |
| 100,000 (234µs) | 85,455 | 1.74% | 36,247 (39198,40457,29087) | 0.69% | 9,525 | **−57.58%** | −15% to −54% |
| 50,000 (102µs) | 168,982 (125865,191435,189647) | 0.61% | 53,370 (65055,46693,48363) | 0.40% | 18,409 | **−68.42%** | −16% to −32% |
| 25,000 (58µs) | 399,255 | 0.053% | 202,598 | 0.060% | 63,064 | **−49.26%** | *(no historical data point)* |
| 10,000 (22µs) | 767,482 | 0.014% | 785,330 | 0.059% | 53,020 | **+2.33%** | −1.2% to −1.6% |
| 5,000 (13µs) | 873,092 | 0.013% | 893,489 | 0.028% | 55,364 | **+2.34%** | +0.26% to +0.8% |

### Verdict: mixed, not a clean win — and the real numbers say something specific

**The loss region did *not* uniformly shrink.** At the two shortest CS lengths
tested (10,000 and 5,000), the sign is the same or better than historical: 10,000
actually **flips from a small loss to a small win** (−1.2/−1.6% → **+2.33%**) —
this is the one genuine, real crossover-point shift the sweep found. 5,000
stays a win and gets slightly larger (+0.26–0.8% → +2.34%).

**But the middle regime (25,000–100,000) — historically IVH's worst
case — got the same or *worse*, not better.** 100,000 (−57.58%) is in the same
ballpark as the worst historical case (−54%), not improved. 50,000 (−68.42%)
is meaningfully **worse** than the historical range (−16% to −32%). 25,000
(−49.26%, no historical baseline to compare against) is also a large loss.
600,000's win **shrank**, not grew (+21–35% historically → +8.46% now).

**A plausible, numbers-grounded explanation, not just noise:** the migration
counts column tells a coherent story. The syscall-skip optimization doesn't
change *whether* a migration happens (same downstream Gate 1–4 in
`bpf_sched_pre_lock_migrate()` as before) — it only removes the syscall
*overhead* for attempts the advisory bit marks safe. That means the harness
can now attempt lock acquisition **faster** per unit wall time whenever it's
not actively skipping. At medium CS lengths (25,000–100,000) this shows up as
a *large increase in the number of migrations attempted* (18,409–63,064 per
20s) relative to the very long CS case (2,171) — more attempts per second
means more chances to hit "danger" and pay the ~250–560µs migration cost
identified in the migration-cost-model doc. The syscall-skip optimization's
own effect (fewer syscalls) is real and cheap, but by letting the harness spin
through more iterations per second, it plausibly **feeds more raw material
into the same already-expensive migration-cost bottleneck** at exactly the CS
lengths where that bottleneck already dominated. This is consistent with,
not a contradiction of, the already-established "migration economics is the
real story" conclusion — it does not overturn it, it adds one more data point
to it.

**Confound that must be named honestly:** this session's background
contention (real, live, from the co-running sysbench VM) is a different, and
apparently *heavier and more variable*, load than whatever the July 20
historical sweep ran under — IVH-off HP% ranges from 0.013% (short CS) to
11.74% (longest CS) *within this single sweep*, and the 50,000/off round 1
(125,865) is a visible outlier against its own two sibling rounds
(191,435/189,647). Some of the magnitude differences from the historical
sweep (especially the shrunk 600,000 win) may be partly attributable to this
different contention regime rather than to the syscall-skip code change in
isolation — the two sweeps are not a perfectly controlled A/B on that axis. I
am not able to fully separate "syscall-skip's own effect" from "different
background contention" with the data collected here, and I am saying so
rather than picking the more flattering explanation.

**Bottom line for goal 1**: syscall-skip earns its keep (§ system-CPU
reduction, already established) but it does **not** broadly move NHextend3's
loss region into a win. It produces one genuine win-flip at the very shortest
CS tested (10,000, ~22µs) and leaves the historically-worst middle band
(25,000–100,000) equally or more negative. No kernel change resulted from this
goal; nothing to rebuild.

---

## Goal 2 — can any tuning help PARSEC dedup/vips?

### Method

Swept `ivh_time_left_threshold_ns` ∈ {50,000 (50µs), 500,000 (500µs),
1,000,000 (1ms), 4,000,000 (4ms, standard), 10,000,000 (10ms)} against dedup
(native `FC-6-x86_64-disc1.iso` input) and vips (native
`orion_18000x18000.v`, run from its own `run/` dir), using
`ivh_exec -v` / `ivh_exec -v -n` for paired HP%+throughput, 3 rounds per
config (60 runs total). Single-run wall time: vips ≈9.5s, dedup ≈13.7s (both
measured directly before the sweep), so 3 rounds × 2 modes × 5 thresholds ×
2 workloads comfortably exceeds "≥20s-equivalent" in aggregate per config.
`ivh_capacity_threshold=1010`, `ivh_max_concurrent=8` held at standard values
throughout this sweep (per the goal's fallback plan, only pursued if the
primary threshold sweep showed nothing to explain).

### Results — `ivh_time_left_threshold_ns` sweep (real wall-clock seconds, 3 rounds each)

**vips** (`im_benchmark orion_18000x18000.v output.v`, off-baseline ≈4.5–6.4s
across rounds — noisy on its own, see caveat below):

| threshold | off (s), 3 rounds | off avg | on (s), 3 rounds | on avg | on HP% (3 rounds) | **Δ (on vs off)** |
|---|---|---|---|---|---|---|
| 50µs | 4.62, 4.61, 4.40 | 4.54 | 6.83, 6.95, 6.81 | 6.86 | 0.0008, 0.001, 0.0007 | **−51.1%** |
| 500µs | 7.51, 5.47, 6.09 | 6.36 | 6.86, 6.52, 6.39 | 6.59 | 0.0002, 0.0005, 0.0005 | **−3.7%** |
| 1ms | 4.58, 4.27, 4.63 | 4.49 | 5.98, 5.45, 6.09 | 5.84 | 0.0005, 0.0012, 0.0002 | **−30.0%** |
| 4ms (standard) | 4.71, 4.64, 4.69 | 4.68 | 5.89, 5.95, 6.22 | 6.02 | 0.0, 0.0001, 0.0 | **−28.6%** |
| 10ms | 4.84, 4.48, 4.45 | 4.59 | 6.23, 5.92, 6.09 | 6.08 | 0.0, 0.0, 0.0001 | **−32.5%** |

**dedup** (`-c -p -v -t 16 -i FC-6-x86_64-disc1.iso`, off-baseline ≈7–12s,
also noisy on its own):

| threshold | off (s), 3 rounds | off avg | on (s), 3 rounds | on avg | on HP% (3 rounds) | **Δ (on vs off)** |
|---|---|---|---|---|---|---|
| 50µs | 6.97, 6.72, 9.62 | 7.77 | 11.36, 9.57, 12.02 | 10.98 | 0.0004, 0.0001, 0.0005 | **−41.4%** |
| 500µs | 11.25, 9.98, 7.37 | 9.53 | 9.36, 10.59, 9.89 | 9.95 | 0.0001, 0.0001, 0.0001 | **−4.3%** |
| 1ms | 7.27, 10.26, 9.87 | 9.13 | 10.66, 10.65, 10.66 | 10.66 | 0.0002, 0.0001, 0.0001 | **−16.7%** |
| 4ms (standard) | 7.05, 9.08, 11.80 | 9.31 | 10.79, 9.70, 11.43 | 10.64 | 0.0, 0.0, 0.0 | **−14.3%** |
| 10ms | 7.21, 11.29, 8.47 | 8.99 | 11.51, 9.73, 10.01 | 10.42 | 0.0, 0.0, 0.0 | **−15.9%** |

(Δ sign convention: negative = IVH-on slower, i.e. a loss, matching this
project's established convention.)

### Secondary sweep — `ivh_capacity_threshold` and `ivh_max_concurrent` (vips only)

Because **no** threshold value escaped a loss (range −3.7% to −51.1% for
vips, −4.3% to −41.4% for dedup — every single one of the 10 workload×threshold
cells is negative), the fallback sweep was run: `ivh_capacity_threshold` ∈
{700, 1010 (standard), 1150}, `ivh_max_concurrent` ∈ {1, 4, 8 (standard), 16},
vips only (dedup shows the qualitatively identical pattern; vips is this
goal's structurally-relevant case, per its `allocate_lock` topology), 3 rounds
each, `ivh_time_left_threshold_ns` held at the standard 4ms.

| param | value | on avg wall time (s), 3 rounds |
|---|---|---|
| `ivh_capacity_threshold` | 700 | 6.06, 6.27, 6.38 (avg 6.24) |
| `ivh_capacity_threshold` | 1010 (standard) | 6.25, 5.98, 6.26 (avg 6.16) |
| `ivh_capacity_threshold` | 1150 | 6.37, 6.01, 6.25 (avg 6.21) |
| `ivh_max_concurrent` | 1 | 5.92, 6.30, 6.23 (avg 6.15) |
| `ivh_max_concurrent` | 4 | 6.46, 7.24, 7.11 (avg 6.94) |
| `ivh_max_concurrent` | 8 (standard) | 6.36, 6.54, 6.21 (avg 6.37) |
| `ivh_max_concurrent` | 16 | 5.94, 6.16, 5.94 (avg 6.01) |

(off baseline for comparison, same threshold/round data as the primary sweep,
4ms row: **4.68s avg**.)

**Every single secondary-sweep configuration is still a clear loss** —
best case (`ivh_max_concurrent=16`, avg 6.01s) is still **28.4% slower** than
the 4.68s off-baseline; worst case (`ivh_max_concurrent=4`, avg 6.94s) is
**48.3% slower**. `ivh_capacity_threshold` barely moves the number at all
(6.16–6.24s across a 700→1150 range, i.e. essentially flat) — this gate isn't
the lever either. `ivh_max_concurrent` shows some real spread (6.01–6.94s)
but **never gets close to parity with off**, let alone a win, and the pattern
isn't even monotonic (4 is worse than both 1 and 8), which is itself evidence
against "just tune concurrency" being a real lever here rather than more
sampling noise on top of a uniformly-bad regime.

### Verdict: no, plainly — and this is a *stronger* negative than the prior dispatch found

**This directly contradicts, and updates, the prior dispatch's "neutral in
isolation" paired-measurement finding** (dedup +6.5%, vips +1.7%, from
`ivh_hp_correlation_analysis_2026-07-20.md`). Today, under the same paired
(single-workload, `ivh_exec -v`/`-v -n`) design, with real live background
contention from the co-running sysbench VM (§ top of this doc) — a variable
that was different or absent in whatever conditions the July 20 measurement
ran under — **both workloads show a consistent, real loss at every threshold
value tested**, not noise-level neutral. The direction is unanimous across
all 10 (workload × threshold) cells despite real round-to-round noise in the
raw wall times (e.g. dedup/50µs/off: 6.97s, 6.72s, 9.62s — a genuinely noisy
baseline) — that consistency of *sign*, even with noisy magnitudes, is itself
the signal worth trusting here.

**Why, mechanistically**: HP% for both workloads, at every threshold, stays
in the same tiny 0.0001–0.0027% band this project's July 20 doc already
established as "real but rare" for these workloads — the loss is not
explained by dedup/vips's own locks being protected more or less effectively
at different thresholds. It is explained by the same migration-collateral
mechanism this project has repeatedly found dominates short-CS, low-leverage
workloads: `ivh_exec -v`'s wrapped process is still subject to real migration
attempts (Gate 1+2 in `bpf_sched_pre_lock_migrate()` fires independent of
whether the resulting protection is ever "used" by a genuine steal event), and
each attempt that proceeds costs the same ~250–560µs this project's migration-
cost model already quantifies — paid whether or not a steal ever actually
threatened this specific critical section. With live, real, non-trivial
background contention now present system-wide (unlike whatever quieter
conditions underlay the July 20 "neutral" result), the *rate* of migration
attempts plausibly rose, and dedup/vips's own critical sections (already
established as short and rarely worth protecting on their own terms) simply
don't generate enough real benefit to offset that now-higher-frequency cost.

**Is there a CS-length-like tunable for vips?** No dedicated one was found or
built this session — `im_benchmark`'s tile/work-unit size is fixed by the
input image and VIPS's own internal tiling logic
(`vips_thread_allocate()` in `threadpool.c`), not exposed as a simple runtime
flag the way `NHEXTEND_LOOP_SPIN` tunes NHextend3's CS directly. A different
native VIPS input (smaller/larger image) would change the *number* of
`allocate_lock` acquisitions but not obviously the *per-acquisition* hold
time, which is already established (§ prior dispatch, bpftrace measurement)
to be ~1.72µs mean — i.e. the "CS length" for vips's real hot lock is already
about as short as this project's data covers, and there's no in-workload knob
that stretches it the way `loop_spin` does for NHextend3. Building one would
mean instrumenting/patching VIPS itself (out of scope) rather than tuning an
existing parameter.

**Plain statement, as instructed rather than a hedged "marginal
improvement"**: **no configuration tested — 5 threshold values, 3 capacity
thresholds, 4 concurrency caps, 2 workloads — produced a win for dedup or
vips.** The topology story from the prior dispatch (dedup: 4-lane, moderate
concentration; vips: single global lock, NHextend3-like concentration)
remains a correct, separately-true structural fact about *why neither
workload's own lock was ever a strong protection-benefit candidate* — but
today's numbers say the *reported* loss is real, not a measurement artifact,
under the live-contention conditions this session actually ran under. That is
a meaningfully different, more negative answer than the July 20 paired
measurement gave, and it should be read as an update, not a contradiction to
paper over — the honest reconciliation is that "neutral" was itself
conditional on a quieter background than this session had.

---

## Goal 3 — is the tick-driven / `exit_to_user_loop` migration idea worth pursuing at all?

**Verdict: no — drop it. Not "fold into goal 4/5," genuinely dead, for a
reason the reframing itself doesn't survive either.**

Restating the corrected premise precisely (per the orchestrating agent, not
re-derived): rescuing a CS that's already in trouble is off the table because
migration (~250–560µs) is too slow relative to the CS lengths this project
cares about. The question was whether a *softer* version survives: use the
tick to migrate a **waiting** (not holding) thread earlier/more often than the
one-shot `ivh_pre_lock()` evaluation allows.

Splitting "waiting" into the two states that actually exist in
`kernel/locking/qspinlock.c`'s slowpath makes the answer concrete rather than
hand-wavy:

1. **A thread already linked into the MCS queue** (has called
   `xchg_tail()`, `WRITE_ONCE(prev->next, node)` done, is spinning on its own
   `node->locked` via `arch_mcs_spin_lock_contended()`). Migrating this task to
   another CPU mid-wait is not a performance question, it's a **correctness
   hazard**: its `struct mcs_spinlock` node lives in a **per-CPU array**
   (`DEFINE_PER_CPU_ALIGNED(struct qnode, qnodes[_Q_MAX_NODES])`,
   `kernel/locking/qspinlock.c:82`), and the *predecessor* node already holds a
   raw pointer to it (`prev->next`, set at `qspinlock.c:322`) that will be
   dereferenced later by `arch_mcs_spin_unlock_contended(&next->locked)`
   (`qspinlock.c:404`) or read via `smp_cond_load_relaxed(&node->next, ...)`
   (`qspinlock.c:402`). Migrating the *task* doesn't move the *node* — the
   node's identity is bound to the CPU it was allocated on
   (`this_cpu_ptr(&qnodes[0].mcs)`, `qspinlock.c:249`), and nothing in this
   file supports unlinking a live node from the middle of the chain. This
   isn't "expensive," it's **not supported by the data structure at all**
   without new kernel-side MCS-queue surgery — genuinely out of scope, not a
   tuning question.

2. **A thread waiting on the pending bit or as queue head** (spinning
   directly on `lock->locked`/`lock->val`, no node allocated — see Goal 4's
   §4.2 for why). Migrating this thread *is* structurally safe (nothing
   points to it by pointer), but there is **nothing to rescue** — an
   unstolen-but-waiting thread isn't stranding anyone else the way a stolen
   *holder* does. The only thing a proactive migration here buys is
   *this thread's own* latency, at the cost of a real ~250–560µs migration —
   in the same short-CS regime this whole project is built around, that trade
   is exactly the losing one Goal 1's numbers just reconfirmed (paying
   migration cost that dwarfs the thing it's trying to speed up).

So the honest shape of the answer is **not** "this is secretly goal 4/5 in
disguise, so go read those." The softened version of the idea fails on its own
terms in both of its only two possible referents: case 1 is unsafe/unsupported,
case 2 is safe but not worth its own cost. What tick-driven, cadence-based
intervention on a **waiting** thread *can* legitimately do is change **how it
waits in place** (TPAUSE vs busy-spin, keyed to predecessor health) — never
its **location**. That is genuinely Goal 4's design, and only that narrow
slice survives — not because "goal 3 folds into 4" as a face-saving reframe,
but because migration-of-a-waiter was never viable to begin with once you
look at what "waiting" actually means in this codebase. **Recommendation:
close this idea out. It should not appear as an open item in the next phase
of work.**

---

## Goal 4 — TPAUSE-based adaptive spinning: MCS-predecessor design, and the instrumentation coverage check

### 4.1 The user's reasoning, checked against the real code: correct, and not just correct in theory — it's the pattern the kernel already uses

Read `kernel/locking/qspinlock.c`, `kernel/locking/mcs_spinlock.h`, and
`kernel/locking/qspinlock_paravirt.h` in full. The holder-vs-MCS-predecessor
question has **three independent existing precedents in this exact kernel
tree**, all choosing MCS-predecessor (or an equivalent one-to-one handoff),
none choosing "signal off the shared holder":

- `kernel/locking/qspinlock_paravirt.h`'s `pv_wait_early(struct pv_node *prev, int loop)`
  (line 264) — already checks `vcpu_is_preempted(prev->cpu)` where `prev` is
  literally the MCS predecessor node, passed down from
  `pv_wait_node(struct mcs_spinlock *node, struct mcs_spinlock *prev)`
  (line 310), which in turn received it from `queued_spin_lock_slowpath()`'s
  own `prev = decode_tail(old, qnodes)` (`qspinlock.c:319`). **This is IVH's
  own existing `ivh_pv_wait_mechanism` machinery** (`arch/x86/kernel/kvm.c`,
  sysctl `ivh_pv_wait_mechanism`, default 0) — the user's proposed semantics
  are not hypothetical, a version of them is already live code in this tree,
  just using a bounded TSC-deadline self-poll (`ivh_pv_backoff()`,
  `IVH_PV_TPAUSE_CYCLES = 512` cycles per nap, `IVH_PV_WAIT_TSC = 65536` cycles
  total window, `arch/x86/kernel/kvm.c:1079-1134`) instead of a real IPI wake.
- `kernel/locking/osq_lock.c:154` — `vcpu_is_preempted(node_cpu(node->prev))`,
  literally the same one-to-one MCS-style predecessor check, in the
  **optimistic-spin queue** used by mutex/rwsem slow paths. The code's own
  comment there is worth quoting: *"vcpu_is_preempted() relies on polling, be
  careful"* — upstream itself flags this exact "no real wake, only polling"
  gap.
- `kernel/locking/mutex.c:395` — `vcpu_is_preempted(task_cpu(owner))` — this
  one *does* key off the true holder, but only because `struct mutex` carries
  an explicit `owner` field (a real `task_struct *`, read via
  `__mutex_owner(lock)`) that a bare `qspinlock` does not have. This is the
  concrete confirmation of *why* qspinlock can't do the same thing for free
  (§4.2).

**Confirmed, not just accepted**: the user's reasoning about thundering herd
is right, and MCS-predecessor is the design this kernel already trusts
everywhere it has the choice. Committing to MCS-predecessor semantics, per the
instructions, unless a concrete reason not to turned up — none did.

### 4.2 What "prev" means for waiter #1 and #2 — a real, load-bearing correction to the "first two waiters" framing

Reading `queued_spin_lock_slowpath()` end-to-end (`qspinlock.c:132-416`)
against the qspinlock word layout (`include/asm-generic/qspinlock_types.h:14-40`,
fields `locked`/`pending`/`tail` packed into one `atomic_t val`) gives a more
precise, and slightly different, picture than "waiter #1 is the owner, waiter
#2 is the pending-bit holder, waiter #3+ get real MCS nodes":

- **The pending-bit waiter** (`qspinlock.c:198-214`, reached via
  `queued_fetch_set_pending_acquire()`) waits directly on
  `smp_cond_load_acquire(&lock->locked, !VAL)` — no MCS node at all. Its
  "prev" is conceptually the current lock **owner**, but **the qspinlock word
  contains no owner-CPU field** — `locked` is a single bit, not an identity.
  There is no in-band way to answer "which CPU holds this lock" here.
- **The first thread to reach the `queue:` label** does allocate an MCS node
  (`node = this_cpu_ptr(&qnodes[0].mcs)`, `qspinlock.c:249`) — but if
  `xchg_tail()` (`qspinlock.c:311`) reports no prior tail
  (`!(old & _Q_TAIL_MASK)`), it **skips the `prev`/`arch_mcs_spin_lock_contended()`
  branch entirely** (`qspinlock.c:318-336`) and falls straight through to
  waiting on `atomic_cond_read_acquire(&lock->val, ...)` — the same raw,
  identity-free lock word as the pending-bit waiter. **This waiter has an MCS
  node, but no usable "prev."** It is the queue *head*, and queue heads wait
  on the lock word directly, not on a predecessor's `node->locked`.
- **Only the *second* thread to reach `queue:` and later** gets a real,
  identity-bearing predecessor: `decode_tail(old, qnodes)` (`qspinlock.c:319`)
  returns an actual `struct mcs_spinlock *`, and — critically — under
  `CONFIG_PARAVIRT_SPINLOCKS` (confirmed `=y` in this tree's `.config`) this is
  reinterpreted as `struct pv_node *`, which carries a real `int cpu` field
  (`qspinlock_paravirt.h:50-54`) set at `pv_init_node()` time
  (`pn->cpu = smp_processor_id()`, line 301). **This is the only one of the
  three roles where a waiter can name, in-band, which physical CPU it's
  waiting on** — exactly the qspinlock analogue of `mutex.c`'s `owner` field,
  but arising for a different reason (MCS chain identity, not holder
  identity).

So the accurate three-way split for IPI design purposes is:

| Role | Who | Waits on | Has CPU identity of what it's waiting for? |
|---|---|---|---|
| A | Pending-bit waiter | `lock->locked` directly | **No** — no owner-CPU field exists anywhere in `qspinlock` |
| B | Queue head (1st MCS node) | `lock->val` directly | **No** — same blindness as A, despite having a node |
| C | 2nd+ MCS node | own `node->locked`, set by predecessor | **Yes** — `pv_node->cpu`, real, already exists |

### 4.3 Concrete design: build it for role C, name the real gap for A/B

**Role C (real MCS predecessor) — buildable now, on existing plumbing:**

The unlock path already identifies the exact successor to hand off to:
`arch_mcs_spin_unlock_contended(&next->locked)` (`qspinlock.c:404`), where
`next` was obtained via `smp_cond_load_relaxed(&node->next, (VAL))`
(`qspinlock.c:402`) — this is precisely the pointer you'd cast to
`struct pv_node *` to read `next_pn->cpu` and target an IPI at. Concretely,
next to `pv_kick_node(lock, next)` (`qspinlock.c:405`, currently a no-op under
`ivh_pv_wait_mechanism=1` per the existing comment — "nobody is ever truly
blocked... `ivh_pv_kick()` is a no-op"), a real implementation would replace
that no-op with something like `apic->send_IPI(next_pn->cpu, IVH_TPAUSE_KICK_VECTOR)`
or reuse `smp_send_reschedule(next_pn->cpu)` — **TPAUSE's own architectural
definition (Intel SDM) is to resume on the earlier of {TSC deadline, any
unmasked interrupt}**, so *any* IPI, even a content-free one, is sufficient to
cut a TPAUSE nap short; no UMONITOR/MONITOR wiring onto the lock address is
required for this specific role. The natural place to grow the current
`ivh_pv_wait_mechanism=1` substitute (`arch/x86/kernel/kvm.c:1117-1203`) is
right here: keep `ivh_pv_backoff()`'s existing TPAUSE call, but let a real
IPI arrive and cut a *much longer* nap window short, rather than the current
tiny, self-polling `IVH_PV_TPAUSE_CYCLES = 512` (≈170ns at 3GHz). **This is
the load-bearing point**: an IPI (≈1–5µs cost) is not worth adding to save
waiting out a 170ns self-poll window — it only pays for itself if paired with
a genuinely longer nap (the user's ~1ms figure), trading a per-unlock IPI cost
for a much lower polling/cache-traffic rate while still reacting fast on the
*real* unlock. Building the two together (longer TPAUSE window + real IPI
kick) is the correct pairing; building either alone doesn't make sense.

**Roles A and B (pending-bit waiter, queue head) — a real, named gap, not a
detail to paper over:** neither can be given "prev" semantics without adding
owner-CPU bookkeeping that plainly does not exist in `qspinlock` today.
Two honest options, both real engineering, neither "one line":
1. Grow `struct qspinlock` (or a side field on `raw_spinlock_t`) with an
   explicit `owner_cpu`, mutex-style — touches every spinlock in the kernel
   and reintroduces exactly the cache-line/size cost qspinlock's own design
   deliberately avoided (that tradeoff is *why* mutex can afford an owner
   field and qspinlock historically chose not to).
2. A side hash table keyed by lock address (the same shape as the existing
   `pv_lock_hash` used for `_Q_SLOW_VAL` bookkeeping,
   `qspinlock_paravirt.h:167-257`) recording "current holder CPU" — real,
   working precedent exists in this exact file for "a hash table keyed by
   lock pointer," but it comes with the same "must unhash before release"
   lifetime discipline that file's own comments call out as delicate.

Given the "role C only" path reuses machinery that's already live and tested
in this tree, and roles A/B require a genuinely new subsystem either way, the
concrete recommendation is: **prototype role C first** (longer TPAUSE window
+ real IPI kick on the existing `ivh_pv_wait_mechanism=1` path), and treat
A/B coverage as an explicit, separately-scoped follow-up — not an oversight to
patch quickly.

### 4.4 Instrumentation coverage check: does `ivh_pre_lock`/`cs_enter`/`cs_exit` fire for every lock/unlock variant?

Read `kernel/locking/spinlock.c` in full and cross-checked against
`include/linux/spinlock.h`'s macros and this tree's `.config`
(`CONFIG_UNINLINE_SPIN_UNLOCK=y`, `CONFIG_DEBUG_LOCK_ALLOC` not set, no
`CONFIG_INLINE_SPIN_LOCK*` symbols present at all — meaning every
`#ifndef CONFIG_INLINE_SPIN_LOCK*` guard in `spinlock.c` is **active**, so the
out-of-line functions below are genuinely the ones compiled in and used, not
bypassed by an arch-inlined fast path).

| Function | `ivh_pre_lock()` called? | `cs_enter()`/`cs_exit()` called? | Verdict |
|---|---|---|---|
| `_raw_spin_lock` (`spinlock.c:481`) | Yes | Yes | Covered |
| `_raw_spin_lock_irqsave` (`:494`) | Yes | Yes | Covered |
| `_raw_spin_lock_irq` (`:510`) | Yes | Yes | Covered |
| `_raw_spin_lock_bh` (`:523`) | Yes | Yes | Covered |
| `_raw_spin_trylock` (`:454`) | **No** (correct — trylock can't block, `ivh_pre_lock`'s own comment forbids trylock callers) | Yes, on success (`:459`) | **Correctly partial** — CS timing/observability still fires, migration gate correctly skipped |
| `_raw_spin_trylock_bh` (`:467`) | **No** (same reason) | Yes, on success (`:473`) | **Correctly partial**, same as above |
| `_raw_spin_unlock` (`:537`, gated `CONFIG_UNINLINE_SPIN_UNLOCK=y`) | n/a (unlock) | Yes | Covered |
| `_raw_spin_unlock_irqrestore` (`:549`) | n/a | Yes | Covered |
| `_raw_spin_unlock_irq` (`:561`) | n/a | Yes | Covered |
| `_raw_spin_unlock_bh` (`:573`) | n/a | Yes | Covered |
| `_raw_read_lock`/`_raw_read_lock_irq{,save}`/`_raw_read_lock_bh` (`:596-625`) | **No** | **No** | **Not instrumented at all** |
| `_raw_write_lock`/`_raw_write_lock_irq{,save}`/`_raw_write_lock_bh`/`_raw_write_lock_nested` (`:668-707`) | **No** | **No** | **Not instrumented at all** |
| `_raw_read_unlock*` / `_raw_write_unlock*` (`:628-739`) | n/a | **No** | **Not instrumented at all** |

**Definitive answer**: every `raw_spin_lock`/`raw_spin_lock_{irq,irqsave,bh}`
variant and their matching unlocks are covered, correctly and completely.
Trylock variants correctly get CS-timing coverage but correctly skip the
migration gate (by design, not a gap). **`rwlock_t` (`raw_rwlock`
read/write/read_trylock/write_trylock and all their `_irq`/`_irqsave`/`_bh`
variants) receive zero IVH instrumentation** — no pre-lock gate, no CS timing,
no observability — this is a real, clean, verifiable gap, not a guess (it's
visible directly in the function bodies, which call only the bare
`__raw_read_lock()`/`__raw_write_lock()` primitives with nothing else added).
Whether this matters depends on how much real contention lives on `rwlock_t`
in the workloads this project cares about — that wasn't measured this
session, but the coverage question itself now has a definitive answer.

---

## Goal 5 — "one-line fix": TPAUSE if target-CPU selection fails

### The exact code

`bpf_sched_pre_lock_migrate()` (`kernel/sched/fair.c:13343-13566`) selects a
target via `bpf_sched_cfs_select_run_cpu_spin()` (`fair.c:13403`) and the
"no healthy target" path is exactly:

```c
if (target_cpu < 0 || target_cpu == smp_processor_id()) {
        if (target_cpu >= 0)
                atomic_fetch_andnot(PRMPT_HELD_MASK, prmpt_flags(target_cpu));
        current->lock_depth--;
        return;
}
```

(`fair.c:13414-13419`) — today this just falls through and the caller proceeds
to ordinary `__raw_spin_lock()`, competing for the lock exactly as if IVH
weren't involved at all.

### Is it really one line? Yes for safety, no for completeness

**Context safety, checked, not assumed**: by this point `my_spinlock` has
already been released (`raw_spin_unlock_irqrestore(&my_spinlock, flags)`,
`fair.c:13408`, executed *before* the `target_cpu < 0` check), IRQs are
re-enabled, and the caller entered `ivh_pre_lock()` with `preemptible()==true`
and `lock_depth==0` at the top (`spinlock.c:184`; `lock_depth` is only
temporarily bumped to 1 for *this function's own* reentrancy guard and is
decremented on every exit path including this one, `fair.c:13417`). **A
TPAUSE call inserted right here runs in a fully preemptible, IRQ-enabled,
lock-free context — this part of the worry is unfounded; it is safe.**

**The primitive already exists, verbatim, elsewhere in this tree**:
`arch/x86/kernel/kvm.c:1117-1134`'s `ivh_pv_backoff()` is exactly
`if (cpu_feature_enabled(X86_FEATURE_WAITPKG)) { u64 until = rdtsc() + N; __tpause(TPAUSE_C02_STATE, upper_32_bits(until), lower_32_bits(until)); } else cpu_relax();` —
copy-adaptable directly, feature-checked correctly (`X86_FEATURE_WAITPKG`,
confirmed present in `arch/x86/include/asm/cpufeatures.h:391`).

**Cooldown interaction, checked, not assumed**: `ivh_eval_cooldown_ok()`
(`fair.c:13188`) stamps `rq->ivh_last_eval_ns = now` (`:13201`) at the top of
`ivh_pre_lock()`, **before** `bpf_sched_pre_lock_migrate()` is ever called —
so a TPAUSE added inside the "no target" branch cannot retroactively affect
or double-count against the cooldown timer; there is no interaction to design
around here.

**Where it stops being "one line"**: TPAUSE by itself, with no
`UMONITOR`-established address, is a **plain timed nap** — it does not wake
early on the lock becoming free, only on its own TSC deadline or an
interrupt. Inserted bare at this call site, it would simply delay entry into
`__raw_spin_lock()`'s ordinary contended path by however long the nap runs,
without any way to react if the lock frees mid-nap (unlike Goal 4's role-C
design, which *does* have a real wake channel via the MCS predecessor's own
`node->locked`). It also needs a duration parameter from *somewhere* — the
`kvm.c` precedent uses a hardcoded constant (`IVH_PV_TPAUSE_CYCLES`), so
"one line using a constant" is achievable, but a properly tunable version
(matching this project's own convention of exposing every other IVH knob as a
sysctl) is one more sysctl, one more `late_initcall` registration, the usual
non-trivial-but-small plumbing — not free, just small.

**Honest verdict**: **yes, literally one line is possible and safe**, but a
bare, unmonitored TPAUSE inserted here buys almost nothing (it can't detect
the lock freeing, so it just delays this thread for no responsive benefit) —
its value only appears if it's the beginning of Goal 4's larger design
(`UMONITOR` on `&lock->val`, `TPAUSE`, real wake on write). **Recommendation:
don't build this as a standalone one-liner; fold it into whichever future
Goal-4 prototype gets built, as the "no target found, so behave like a
regular contended waiter with the new backoff behavior" fallback case rather
than a separate mechanism.**

---

## Goal 6 — steal-time → TSC

### 6.1 Every steal-time usage in the repo, surveyed

**Kernel-side, direct `paravirt_steal_clock()` callers:**
- `kernel/sched/core.c:195`, `get_steal_and_preemptions()` — IVH's own
  bridge, feeds `/proc/vcap_info` (via the out-of-tree
  `vsched_module.ko`/`custom_modules/vsched_module.c`, per `NHextend3.c`'s own
  header comment) and is what `vcap`/`NHextend3` both read as ground truth.
- `kernel/sched/core.c:929` (`update_rq_clock_task()`, gated
  `CONFIG_PARAVIRT_TIME_ACCOUNTING`) and `kernel/sched/cputime.c:263`
  (`steal_account_process_time()`) — the two stock-Linux CPU-accounting
  consumers (feed `/proc/stat`'s `st` field and per-task steal accounting).
- `kernel/sched/cputime.c:288`, `is_cpu_preempted(int cpunum)` — **not** a
  direct steal-clock reader; it's a *heartbeat* check
  (`sched_clock() - cpu_rq(cpunum)->clock_preempt > 1500000` i.e. >1.5ms
  stale), where `clock_preempt` is itself refreshed inside
  `steal_account_process_time()`'s own `steal > 1000000` branch (`:270-276`).
  This is IVH's main target-health primitive, consumed at
  `kernel/sched/core.c:1457,1470` (`custom_idle_poll`, `preempt_migrate_func`)
  and `kernel/sched/fair.c:13119,13435,13510` (the danger gate and both
  migration mechanisms' target-health checks).

**Kernel-side, `vcpu_is_preempted()` / `pv_ops.lock.vcpu_is_preempted`
callers** (arch-dispatched, ultimately backed by the same steal-clock
infrastructure via `pv_vcpu_is_preempted()` → `PVOP_ALT_CALLEE1`):
- `kernel/locking/qspinlock_paravirt.h:289`, `pv_wait_early()` — IVH's own
  `ivh_pv_wait_mechanism` (§4.1).
- `kernel/locking/osq_lock.c:154` — stock optimistic-spin-queue (mutex/rwsem
  slowpaths), unmodified by this project.
- `kernel/locking/mutex.c:395` — stock `mutex_spin_on_owner()`, with an
  additional IVH-specific stat (`ivh_mutex_spin_owner_preempted`) layered on,
  unmodified logic otherwise.
- `kernel/sched/syscalls.c:229`, `available_idle_cpu()` — stock, used by the
  load-balancer's idle-CPU search.
- `include/linux/sched.h:2380-2400` — the generic (non-PV-arch) fallback
  definition and its use in `owner_on_cpu()`.

**Arch-level backends** (one steal-time system, three hypervisor
implementations): `arch/x86/kernel/kvm.c` (`kvm_steal_clock`,
`has_steal_clock`, the actual KVM steal-time MSR reader this VM uses),
`arch/x86/xen/time.c` (`xen_steal_clock`), `arch/x86/kernel/cpu/vmware.c`
(`vmware_steal_clock`) — all register through the same
`DEFINE_STATIC_CALL(pv_steal_clock, ...)` indirection
(`arch/x86/kernel/paravirt.c:71`), so `paravirt_steal_clock()` is a single
call-site abstraction over whichever hypervisor is actually running.

**Userspace:**
- `vcap` (`/home/nick/vsched_main/vcapacity/main.cpp`) — steal-time is its
  *entire* raw input. `get_finalized_data()` (`main.cpp:349-428`) computes
  `capacity_perc = used_time / (used_time + stolen_pass)` directly from the
  `steal_time` field read out of `/proc/vcap_info`-style data
  (`data_end[i].steal_time - data_begin[i].steal_time`, line 363), and
  `latency = stolen_pass / preempts` (line 404) — i.e. average stolen
  nanoseconds per detected preemption event. There is no TSC-based signal
  anywhere in this file; steal-time is the entire foundation.
- `NHextend3.c`'s own `read_vcap_steal()` (line ~40-60) and `ivh_exec.c`'s
  `-v` stats — both read the same `/proc/vcap_info` steal-time ground truth,
  independently of this project's kernel-side EWMA classifiers.
- **`vact` — not found.** Searched the entire filesystem (`find / -iname
  "*vact*"`, excluding `/proc` and `/sys`), the whole `/home/nick/vsched_main`
  tree, its `.gitmodules` (three submodules only: `vsched_kernel`,
  `vtopology`, `vcapacity` — no fourth tool), and every `.sh`/`.md` file in
  that repo for the literal string "vact". **It does not exist anywhere on
  this machine.** The closest real candidates, in case this was a
  mis-transcription: `vtop` (the `vtopology` submodule's binary,
  `/home/nick/vsched_main/vtopology/vtop` — a real, built binary, sibling to
  `vcap`, but it does **not** touch steal-time at all — grepping its
  `main.cpp` for "steal"/"preempt" returns nothing; it appears to be a
  cache/NUMA topology prober, a different concern entirely) or one of
  `activate_vsched_bpf.sh`/`activate_vsched_total.sh` (setup scripts, not
  measurement tools). I am reporting this as "not found" rather than guessing
  further, per the instruction to do so.

### 6.2 TSC background, for evaluating the professor's proposal on its merits

- **`rdtsc`/`rdtscp`**: read a per-logical-CPU free-running cycle counter.
  `rdtscp` additionally returns `IA32_TSC_AUX` (typically the CPU/node ID) in
  one serializing instruction, useful for detecting a migration mid-read.
  Cost: tens of cycles — dramatically cheaper than a `pvclock`/steal-time
  read, which is a seqlock-protected memory structure read
  (`kvm_steal_clock()`, `arch/x86/kernel/kvm.c:409`, does exactly this: a
  retry loop around a versioned struct).
- **Invariant/constant TSC**: the CPUID leaf is **`0x80000007`, `EDX` bit
  8** ("Invariant TSC" — ticks at a fixed rate regardless of P-state/C-state
  transitions, and continues through them). This kernel approximates that
  guarantee via two separate feature bits,
  `X86_FEATURE_CONSTANT_TSC` and `X86_FEATURE_NONSTOP_TSC`
  (`arch/x86/include/asm/cpufeatures.h:80,96`), both checked together at
  several points in `arch/x86/kernel/tsc.c` (e.g. `:1261-1263`) before the
  kernel trusts TSC as a general-purpose clocksource.
- **This VM's actual exposed CPUID, checked directly** (`/proc/cpuinfo`
  flags): `tsc`, `rdtscp`, `tsc_deadline_timer`, `tsc_adjust` are present —
  **`constant_tsc` and `nonstop_tsc` are absent.** The current active
  clocksource is `kvm-clock` (confirmed:
  `/sys/devices/system/clocksource/clocksource0/current_clocksource`), a
  paravirtualized clock (the `pvclock` ABI) layered over the host's own TSC,
  not raw guest `rdtsc`. **This means the guest's own CPUID does not let it
  self-verify the invariant-TSC guarantee a TSC-only heuristic would be
  leaning on** — in practice modern cloud KVM hosts generally do provide a
  stable, correctly-scaled TSC to guests (that's *why* `tsc` is even listed as
  an available clocksource alternative here), but that stability is presently
  a property of *this specific host's* hardware and hypervisor configuration,
  not something this guest kernel can architecturally assert for itself. Worth
  being explicit about since it's exactly the kind of assumption a paper
  reviewer would probe.
- **`sched_clock()`**: with `kvm-clock` active, `sched_clock()` in this guest
  ultimately derives from the paravirtual clock, not bare `rdtsc` — the same
  general-purpose time source the rest of the kernel's scheduling/accounting
  logic already uses, and notably a *different* one from what IVH's own PV
  spinlock substitute (`arch/x86/kernel/kvm.c`'s `ivh_pv_wait()`/`ivh_pv_backoff()`)
  already uses internally — that code reads raw `rdtsc()` directly for its
  TSC-deadline windows (`IVH_PV_WAIT_TSC`, `IVH_PV_TPAUSE_CYCLES`), sidestepping
  `sched_clock()`/`kvm-clock` entirely already. **This is real, existing
  precedent in this exact tree for trusting raw TSC deltas over short windows
  without CPUID-asserted invariance** — it already works well enough to ship
  as IVH's own backoff primitive, which is a point in the proposal's favor,
  not against it.

### 6.3 The concrete TSC-only heuristic, worked out as asked (taking "we can tune around not knowing why" at face value)

Sketch, directly modeled on `NHextend3.c`'s own existing `calibrate_tsc()` /
`tpause_wait_ns()` pattern (lines ~125-150 — this project already has a
working, empirically-calibrated TSC-rate measurement in userspace, which is
exactly the right starting point):

1. **At spin-loop entry** (kernel-side MCS-node analogue of `pv_init_node()`):
   record `u64 spin_start_tsc = rdtsc();` and an **expected-work estimate**
   — e.g. a decaying average of this lock's own recent hold-time-in-cycles
   (the same idea as this project's existing per-task `last_cs_ns` EWMA
   machinery, but keyed to cycles instead of `sched_clock()` ns, and possibly
   per-lock rather than per-task).
2. **On each poll iteration** (replacing today's `PV_PREV_CHECK_MASK`-gated
   `vcpu_is_preempted(prev->cpu)` check in `pv_wait_early()`): compute
   `elapsed = rdtsc() - spin_start_tsc` and compare against
   `expected_work_cycles * ivh_tsc_slack_multiplier` (a new tunable, same
   shape as every other `ivh_*` sysctl in `kernel/sched/bpf_sched.c`). If
   `elapsed` exceeds that bound, treat it as "prev is probably not making
   progress" and back off / reroute — **without ever reading `prev->cpu`,
   `vcpu_is_preempted()`, or any steal-time state at all.**
3. **Calibration** of `ivh_tsc_slack_multiplier` is the entire game — too
   tight and ordinary scheduling jitter (a tick, an unrelated IRQ, normal CFS
   preemption of the predecessor) reads as false-positive "stolen," causing
   spurious backoff/reroute on a perfectly healthy predecessor; too loose and
   real host steal goes undetected for too long, which is precisely the
   failure mode `vcpu_is_preempted()`/steal-time is supposed to catch fast.

**Is this a viable tunable proxy?** Directionally, plausibly yes, for exactly
the reason the user names: you don't need to know *why* elapsed time exceeded
expectation to decide "stop trusting this wait," and the kernel already has
one real, live precedent (IVH's own `rdtsc()`-based TPAUSE deadline machinery)
trusting raw TSC deltas for short windows without needing CPUID-asserted
invariance. But it is **not a free substitute** — it trades one known,
already-measured false-positive-rate problem (the vSpin work already found
symmetric-vs-asymmetric EWMA constants matter enormously for exactly this
kind of "how sensitive should the trigger be" tuning, per this project's own
memory of the `ivh_hot_preempt_ewma_k_rise`/`_fall` asymmetric-cooldown fix)
for a *new*, uncalibrated one (the slack multiplier). The honest comparison
is: steal-time gives you a **ground-truth signal with a calibration problem
already solved** (this project has already tuned `IVH_HOT_STEAL_FLOOR_NS`,
the EWMA rise/fall constants, etc., against real measured behavior); a
TSC-only heuristic gives you a **cheaper-to-read but noisier proxy signal
with a calibration problem not yet solved at all.** That's a real tradeoff,
not a clear win either way, and it should be evaluated as such rather than
assumed to be strictly simpler just because `rdtsc` is cheaper than a
steal-time read.

### 6.4 Would ditching steal-time actually simplify `vcap`/`vact`, or just move the complexity?

**Moves it, does not remove it.** `vcap`'s entire capacity model
(`capacity_perc = used_time / (used_time + stolen_pass)`, §6.1) is a direct,
simple ratio *because* steal-time is a ground-truth signal the host already
computes and hands to the guest for free via the KVM steal-time MSR — vcap's
own code does no inference, no calibration, no thresholding to get that
number. A TSC-only reimplementation would have to **replace that one clean
ratio with exactly the calibration problem in §6.3** — an
expected-work-cycles baseline that has to be established and continuously
re-validated per-vCPU, per-workload, or both, plus a slack multiplier tuned to
avoid both false positives (ordinary scheduling noise misread as theft) and
false negatives (real theft misread as normal jitter). This is not a smaller
problem than reading a steal-time counter; it is a **different, harder
problem being asked to produce the same answer**. If the motivation for
moving off steal-time is portability (steal-time/pvclock requires
paravirt/hypervisor cooperation; raw TSC does not, in principle) that's a
legitimate, separate reason — but "simplification" is not the honest framing
for it.

---

## Closing: does this dispatch close out "the current mechanism," or open new work?

**Pushing back on, not just agreeing with, the "4 and 6 are a new phase"
framing — with real differentiation between the two.**

**Goal 4 genuinely is the start of new work**, and the investigation here
makes that concrete rather than aspirational: there is a real, specific,
buildable next step (role-C TPAUSE+IPI, reusing the existing
`ivh_pv_wait_mechanism` plumbing), a real, specific gap that's out of scope
for a quick pass (roles A/B need new owner-CPU bookkeeping), and a definitive
answer to a previously-open instrumentation question (rwlock is uncovered).
Agreed without reservation: this opens a new phase, not a cleanup item.

**Goal 6 is a more qualified "new phase."** The kernel-side survey and the
concrete heuristic sketch are real, novel-to-this-project analysis — but the
actual next unit of *work* this goal points to is not a kernel patch, it's a
**userspace measurement exercise** (calibrate `ivh_tsc_slack_multiplier`
against ground-truth steal-time, the same way `NHextend3.c` already
calibrates its own TSC rate empirically) — closer in spirit and effort to
Goal 1/2's measurement work than to Goal 4's kernel design work. Calling it
"a new phase" is fair in the sense that nobody has done this measurement
before; it's less fair if it's taken to mean "kernel work is imminent" — the
honest sequencing is: prototype the heuristic and its false-positive rate in
userspace first (cheap, fast, no rebuild), and only escalate to kernel changes
if that prototype actually shows the multiplier can be tuned to a usably low
error rate. Skipping straight to kernel implementation would be getting ahead
of the evidence.

**Goals 1–3, by contrast, are real closure, not new work**: Goal 1 answers a
concrete, previously-open empirical question with a mixed (not clean) result
and a grounded explanation. Goal 2 reconfirms, on fresh hardware with real
live contention, that neither PARSEC workload has a tunable sweet spot — a
negative result, stated plainly, not dressed up. Goal 3 kills an idea outright
with a structural (not just empirical) reason, which is a stronger, more
final kind of closure than "we tried and it didn't help."

**Overall**: this dispatch does not close out "the current mechanism" as a
whole — Goal 4 alone guarantees that isn't true, and it's real, substantive,
unimplemented work, not busywork. But it also isn't a uniform pivot to a new
phase: three of six goals (1, 2, 3) are closures, and even within the two
"new work" goals, 4 and 6 are at very different levels of readiness for
kernel-code investment. The single most concrete, lowest-risk next action
across all six goals is **Goal 4's role-C prototype** — it reuses existing,
already-compiled machinery, has a named safety argument (TPAUSE's own
architectural wake-on-interrupt behavior), and a clear success metric (does
a real IPI-cut-short at a longer nap window reduce host-preempted-CS
percentage further than the current 512-cycle self-poll, without regressing
throughput the way longer *migration* attempts already have in Goal 1).
