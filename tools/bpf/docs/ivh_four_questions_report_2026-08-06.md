# IVH: four questions — evidence report

**Date:** 2026-08-06 (overnight session)
**Tree:** `/home/nick/kernels/linux-6.17-rseqport`, branch `kernel-43-clean`
**Boot:** `6.17.0-rseqport70-stealfix+` — **no kernel rebuild, no reboot; the kernel binary was
never touched.** All kernel-side experiments are sysctl flips only.
**Mode at start and at end:** `tsc-vcap` — `ivh_pv_preempt_src=2`, `ivh_pv_wait_mechanism=2`,
`ivh_pv_kick_pure_ipi=1`, `ivh_steal_source=1`, `ivh_preempt_event_source=2`, `ivh_cap_source=0`,
`ivh_cfg[0]=0`; `vcap -p 200 -s 5000` and `MY_ivh_atc` running as singletons.
**Benchmark everywhere:** `/home/nick/ivh_exec [-v] hackbench -T -g 1 -f 8 -l 400000`.

Every claim below is tagged **PROVEN** (live measurement and/or exact code quotation in this
session), **INFERRED** (a model consistent with measurement but not directly measured), or
**UNKNOWN**.

---

## 0. Thirty-second summary

| Q | Answer | Tag |
|---|---|---|
| **Q1** | **The "inaccurate and actively harmful" characterisation does NOT reproduce under the current 16-sysbench uniform 2:1 scenario.** Measured live in shadow mode tonight: form 0 gets **precision 72.7%, sensitivity 69.8%** — against `is_wait_preempted`'s **34.8% precision and 0.35% sensitivity in the same windows**. The base-rate argument is correct *as a mechanism* but its conclusion is **workload-conditional**: precision is dominated by holder-preemption prevalence, which is **3.45% tonight vs 0.134% in the 2026-07-30 measurement — a 26× change**. The cascading-cost argument is real, prevalence-independent, and I measured it: acting on the predicate costs **+20% lock steals** and drops the anti-steal bit **within the first half of the spin budget 56% of the time**. It cost **no measurable throughput** at this workload. | PROVEN |
| **Q2** | **Confirmed, with three additions.** The PV overlay is exactly six substitutions plus one `pv_ops` unlock swap — I can enumerate them exhaustively from the source. Kick / halt / lock-stealing covers most of it. Three things do not fit: (a) the **pending-bit "second waiter" fast path is deleted outright**, and the bit is repurposed as an anti-steal flag owned by the queue head; (b) **every `spin_unlock` in the kernel becomes a `LOCK cmpxchg`** instead of a plain byte store, contended or not; (c) `virt_spin_lock()`'s test-and-set fallback is bypassed. | PROVEN |
| **Q3** | **The 2026-08-04 phantom-tax regime does not reproduce here — but a bigger, opposite error does.** Idle: error **−0.3%…+1.6%** at `ivh_ref_method=0`. Under sustained guest load — the regime IVH actually runs in — the inferred value **under-reports real steal by 25–35%**, and `ivh_ref_method=2` makes it **worse** (−27…−39%). Root-caused live: **the lock-path halt/poll correction is a large over-subtraction.** `ivh_ref_halt_correct` 2 → 1 → 0 moves the mean error **−30.1% → −14.2% → −5.4%** with **no cost at idle**. The user's structural argument is **two-thirds true**: capacity and Part-C `last_preemption` are TSC-built, but `rq->last_preemption` / `last_active_time` / `preemptions` / `max_latency` are still fed by a **direct, non-switchable `paravirt_steal_clock()`**. | PROVEN |
| **Q4** | **The early-exit hypothesis is RIGHT, but not for the stated reason.** The cost is not the CPU time of the scan — it is that the scan runs inside a **single global `raw_spinlock_t my_spinlock`, with IRQs disabled, ~99,000 times per second across 16 vCPUs** (`kernel/sched/fair.c:13705`). The candidate loop is **87% of that global critical section**; removing it drops the lock's busy fraction **38.4% → 7.7%** and recovers **80%** of the regression. Four independent ablations — early exit, `ivh_selection_trylock=1`, a 5 ms eval cooldown, and blocking Gate 1 — all recover **70–105%**, which is only consistent with "the per-evaluation fixed cost is the whole story". **`echo 1 > /proc/sys/kernel/ivh_selection_trylock` recovers 70–80% tonight with no rebuild.** Separately: a pure diagnostic tick hook, `ivh_scan_stuck_waiters()`, burns **~8.4% of all vCPU wall time in both arms** and can never emit anything at 0 migrations. | PROVEN |

**The single most actionable line in this document:**
`echo 1 > /proc/sys/kernel/ivh_selection_trylock` — see §1.6 for what it costs you.

---

## Provenance: the host corunner changed mid-session, and I caught it

This matters for reading every number below, so it goes first.

At **02:18** the corunner was exactly as handed off — I verified it by pinning a busy loop to all 16
vCPUs and reading `/proc/stat` deltas:

```
AGGREGATE steal% over all 16 vCPUs = 39.98    (uniform: every cpu 39.7-40.4%)
```

At **03:05** the same test read **27.90%**, and by 03:20 hackbench times had collapsed from 15–16 s
to 5–6 s on *every* arm including `noopt`. Re-measuring with in-run steal sampling:

```
run1 base_ivh time=6.277   mean_steal_during_run=2.2%
run2 base_ivh time=11.719  mean_steal_during_run=17.8%
run3 base_ivh time=16.064  mean_steal_during_run=23.2%
```

**The host corunner became intermittent.** Consequences, handled explicitly:

1. The **first sysctl matrix (§1.2)** ran 02:25–02:45 entirely inside the stable ~40% regime, was
   round-robin over 4 rounds, and is internally consistent (`base_ivh` = 15.29/15.55/15.62/16.31).
   **It is the headline matrix.**
2. Everything after 03:20 was re-run with a **paired, round-robin design and per-run steal
   recorded**, and is reported as a **ratio to the same round's `noopt`**, which removes host drift.
   That is §1.3.
3. One casualty: I could not reproduce a regime in which migrations actually fire (§1.8). At
   `IVH_CAP_HARDFLOOR=600` tonight, migrations were **still 0** because the 16 vCPUs are now too
   uniform for the top-band test. **UNKNOWN**: whether the Q4 fixes also help when migrations fire.

---

# 1. Q4 — why doesn't IVH fall back to "adaptive spinning only" when the scan finds nothing?

## 1.1 The answer in one paragraph

Because "finding nothing" is not free. Every full evaluation takes a **process-wide global
`raw_spinlock_t`**, `my_spinlock`, **with interrupts disabled**, holds it across a BPF trampoline and
two `bpf_loop()`s over 16 remote runqueues, and then releases it — **~99,000 times per second across
16 vCPUs**, with a hold time of ~5.9 µs. That is an offered utilisation of ~0.62 on a *single global
server* that all 16 vCPUs must pass through, in a guest where the holder is host-preempted a
non-trivial fraction of the time. The migration never happening is irrelevant: the cost is paid
before the decision is known.

Source, `kernel/sched/fair.c:13698-13712` (quoted exactly):

```c
	if (READ_ONCE(ivh_selection_trylock)) {
		if (!raw_spin_trylock_irqsave(&my_spinlock, flags)) {
			atomic_inc(&ivh_trylock_misses);
			current->lock_depth--;
			return;
		}
	} else {
		raw_spin_lock_irqsave(&my_spinlock, flags);
	}
	target_cpu = bpf_sched_cfs_select_run_cpu_spin(
			rq, current, sched_clock(),
			average_capacity_all, num_online_cpus());
	if (target_cpu != -1)
		atomic_fetch_or(PRMPT_HELD_MASK, prmpt_flags(target_cpu));
	raw_spin_unlock_irqrestore(&my_spinlock, flags);
```

`my_spinlock` is `static DEFINE_RAW_SPINLOCK(my_spinlock);` at `kernel/sched/fair.c:63` — one lock,
for the whole system.

## 1.2 Headline ablation matrix (stable ~40%-steal regime, 4 round-robin rounds)

All arms: 0 migrations. `/home/nick/ivh_exec hackbench -T -g 1 -f 8 -l 400000`, seconds.

| arm | what it removes | runs | mean | Δ vs no-opt | **regression recovered** |
|---|---|---|---:|---:|---:|
| `base_ivh` | — (floor 950, IVH on) | 15.288 15.552 15.624 16.308 | **15.693** | +2.850 | — |
| `noopt` | `ivh_universal_eligible=0` | 13.134 13.181 12.067 12.991 | **12.843** | 0 | 100% |
| `gate1_block` | `ivh_capacity_threshold=0` → Gate 1 rejects everything, so `my_spinlock` / BPF / `trace_printk` are never reached | 13.646 13.321 13.515 13.059 | **13.385** | +0.542 | **81%** |
| `trylock` | `ivh_selection_trylock=1` → never *blocks* on `my_spinlock`; the scan still runs when the trylock succeeds | 13.559 13.298 13.164 13.612 | **13.408** | +0.565 | **80%** |
| `cooldown5ms` | `ivh_eval_cooldown_ns=5000000` → 100× fewer evaluations | 13.140 11.866 12.505 13.241 | **12.688** | −0.155 | **105%** |
| `tracingoff` | `tracing_on=0` → kills the unconditional `trace_printk` at `fair.c:13714` | 16.138 16.722 16.246 15.597 | **16.176** | +3.333 | **−17%** |

**PROVEN, and this is the load-bearing table.** Four different ways of not paying the per-evaluation
fixed cost each recover 80–105% of the regression. The one thing that is *not* the cost is the
`trace_printk` — despite it firing on literally every evaluation (316,450 `ivh_selected:` records
were sitting in the ring buffer with 2.39 M overruns on cpu0 when I looked). Turning tracing off
changed nothing; I had it as my first suspect and it was wrong.

## 1.3 BPF-side ablations (10 paired round-robin rounds, drifting host, ratio to same-round `noopt`)

Each arm is `time / (that round's noopt time)`, which cancels host drift.

| arm | what changed in `MY_ivh_atc.bpf.c` | median ratio | mean ratio | **recovered** |
|---|---|---:|---:|---:|
| `base` | baseline | **1.286** | 1.313 | — |
| `nocapstat` | the `cap_sum`/`cap_cnt` `__sync_fetch_and_add` pair deleted (2 shared-array atomics × 15 candidates per scan) | **1.296** | 1.302 | **−3%** |
| `earlyexit` | skip the candidate `bpf_loop()` entirely when `scan_max` cannot clear either capacity gate | **1.058** | 1.116 | **80%** |
| `both` | `nocapstat` + `earlyexit` | 1.079 | 1.088 | 72% |
| `trylock` | (sysctl only) | 1.085 | 1.087 | 70% |
| `gate1` | (sysctl only) | 1.040 | 1.062 | 86% |
| `noopt` | — | 1.000 | 1.000 | 100% |

Two results here:

- **The early exit works — 80% recovery. Your hypothesis is confirmed.**
- **The diagnostic atomics are *not* the cost.** I expected `cap_sum`/`cap_cnt` — 30 atomic RMWs per
  scan on two *shared*, non-percpu `BPF_MAP_TYPE_ARRAY` maps — to be a big chunk. They are not
  (−3%, i.e. inside the noise). Recording this so nobody re-derives it.

The exact early-exit tested (**not left applied** — see §5):

```c
    /* Both capacity gates are monotone in dcap, and dcap <= scan_max for every
     * candidate by construction, so if scan_max itself cannot clear
     * GATE_CAPACITY_LOW's hard rail or GATE_NOT_BETTER's margin, NO candidate
     * can -- the whole per-candidate loop is provably dead work. */
    if (smc.max <= IVH_CAP_HARDFLOOR ||
        smc.max < (u32)ivh_cap_of(rq, cap_src) + IVH_CAP_MARGIN)
            return -1;

    bpf_loop(nr_loops, &process_cpu, &task_context, 0);
```

## 1.4 The mechanism, measured directly: the early exit works by *shortening the global critical section*

This is the measurement that separates "scan CPU cost" from "lock convoy", and it needs no
instrumentation at all — `ivh_trylock_misses` is already in `/proc/ivh_debug`, and under
`ivh_selection_trylock=1` **it is an unbiased estimator of the fraction of time `my_spinlock` is
held.** Two runs, identical except for the BPF variant:

| BPF variant | evaluations | trylock misses | **`my_spinlock` busy fraction** | gate evals | implied hold time |
|---|---:|---:|---:|---:|---:|
| `base` | 1,467,201 | 563,013 | **38.4%** | 11,195,856 | **5.91 µs** |
| `earlyexit` | 1,428,172 | 110,527 | **7.7%** | **0** | **0.79 µs** |

**PROVEN.** `TOTAL gate evals: 0` confirms the early exit fires on 100% of scans. The candidate loop
is therefore **5.1 µs of a 5.9 µs global critical section — 87% of it** — i.e. ~340 ns per candidate
for a gate chain that does a remote `rq` dereference, a remote `rq->curr->lock_depth` pointer chase
into another CPU's `task_struct`, and (for hackbench `-T`, where all 16 workers share one `mm`) a
`bpf_probe_read_user()` into a sibling thread's `rseq->cr_counter`.

**INFERRED** (queueing model, consistent with the above): offered utilisation of the single global
server is `99,000/s × 5.91 µs = 0.62` at baseline and `0.084` with the early exit. That is the
difference between a saturating server and a free one, and it is why removing 41% of the scans
(`trylock`) and removing 100% of the loop body (`earlyexit`) land in the same place.

## 1.5 Direct lock-identity measurement, and a second, larger finding

`bpftrace` kprobe/kretprobe on `__pv_queued_spin_lock_slowpath`, matching `arg0` against the exact
addresses from `/proc/kallsyms` (`ffffffff8ed83620 b my_spinlock`, `ffffffff8ed83200 b
ivh_wait_lock`). 10-second windows during hackbench; 16 vCPUs ⇒ 160 vCPU-seconds of wall per window.

| | IVH on | no-opt |
|---|---:|---:|
| `my_spinlock` slowpath entries | **552,876** | **0** |
| `my_spinlock` time blocked | **27.84 s** = **17.4% of all vCPU wall time** | 0 |
| `ivh_wait_lock` slowpath entries | 1,779,736 | 1,889,100 |
| `ivh_wait_lock` time blocked | 13.55 s = **8.5%** | 13.35 s = **8.3%** |

(A second IVH-on window read `my_spinlock` 607,852 entries / 15.21 s — kprobe overhead inflates the
absolute sums and the variance is large, so read these as **10–20% of vCPU wall time**, not as a
precise figure. The *identity attribution* is exact.)

The wait-time distribution for `my_spinlock` has a heavy tail that a pure serialisation model does
not produce: **~2,900 waits per 10 s window in the 1 ms – 32 ms range.** **INFERRED:** those are
holders that were host-preempted while holding a global, IRQ-disabled spinlock. That is textbook
lock-holder preemption — created by IVH's own anti-LHP machinery.

### 1.5b `ivh_scan_stuck_waiters()` — ~8.4% of all vCPU wall time, for nothing

The second row of that table is not about IVH's cost at all. `ivh_scan_stuck_waiters()` is called
**unconditionally** from `sched_tick()`:

```c
	/* EXPERIMENT: bare-schedule() hang diagnosis, 2026-06-30. Cheap
	 * 32-slot scan; only ever produces output when an IVH self-migration
	 * is actually stuck past a threshold. */
	ivh_scan_stuck_waiters();
```
— `kernel/sched/core.c:7832`

and its body (`kernel/sched/fair.c:235-283`) takes and releases the **global** `ivh_wait_lock`
**once per slot, 32 slots, every tick**. At `CONFIG_HZ=1000` × 16 vCPUs that is **512,000 global
raw-spinlock round trips per second on a single cacheline, from hardirq context.**

- **PROVEN:** 1.78–1.99 M `__pv_queued_spin_lock_slowpath` entries per 10 s window on *that specific
  lock*, in **both** the IVH-on and the no-opt arm.
- **PROVEN:** a direct kprobe on the function itself measured 84,161 calls / 8.97 s aggregate
  (IVH on) and 83,972 calls / 9.67 s (no-opt) per 10 s window — i.e. ~**6% of wall / ~8.4% of vCPU
  time, and flat across arms**.
- **PROVEN:** it is a *pure diagnostic*. It only reaches its `trace_printk` when
  `ivh_wait_slots[i].task != NULL`, which is only ever set by `ivh_wait_register()` around an actual
  migration. **At 0 migrations it cannot emit a single line.**

**A correction to my own first reading, recorded because it is exactly the class of error you have
caught before.** `perf record --call-graph fp` attributed 21.07% of cycles to this hook under IVH-on
versus 9.05% under no-opt, and I initially wrote that up as "IVH more than doubles the tick-hook
contention". Two independent kprobe-based measurements (function duration, and slowpath entries
resolved by lock address) both say it is **flat across arms**. The frame-pointer callgraph is the
odd one out — most likely mis-walking through the IRQ-nesting that IVH's IRQ-disabled regions create.
**I am reporting the kprobe reading as authoritative and discarding the perf amplification claim.**

This hook matches the failure documented in `project_ivh_scan_stuck_waiters_freeze.md` (removed
2026-07-06 after it froze the VM). The call site is present in commit `84f1e5fcc`
(`6.17.0-rseqport34+`) and is **not** part of the uncommitted working-tree diff, so it is back in the
committed tree. **Cannot be fixed this boot — it needs a rebuild.**

## 1.6 What to do — recommendation, with its cost stated

**Tonight, free, no rebuild:**

```
echo 1 > /proc/sys/kernel/ivh_selection_trylock
```

Recovers 70–80% of the regression in two independent host regimes. **The honest caveat:** this is not
purely a cost fix. With the trylock, **38–41% of evaluations decline to select a destination at all**
(`ivh_trylock_misses` 563,013 of 1,467,201). In the 0-migration regime that costs nothing. In a
regime where migrations *would* fire, it is a ~40% reduction in IVH's coverage. It is the right
*default* while the real fix is built, not the real fix.

**The real fix, in priority order (all need a rebuild; none is implemented here):**

1. **Delete the global lock.** `my_spinlock` exists to make "scan, then claim" atomic. But the claim
   is already an atomic RMW (`atomic_fetch_or(PRMPT_HELD_MASK, prmpt_flags(target_cpu))`) and the BPF
   program *already* re-checks the claim bit per candidate (`REJ_CLAIMED`,
   `MY_ivh_atc.bpf.c:493`). The lock narrows a race window that the claim protocol already closes
   correctly; losing the race costs one wasted migration attempt, not correctness. Replacing
   blocking acquisition with "scan lock-free, then `atomic_fetch_or` and verify the bit was clear,
   else abandon" removes the convoy *and* the coverage loss.
2. **Land the `scan_max` early exit** (§1.3). Independently worth 80%, and it composes with (1).
3. **Delete or gate `ivh_scan_stuck_waiters()`.** ~8.4% of all vCPU time, on both arms, producing
   nothing. Cheapest possible version: `if (!atomic_read(&ivh_in_schedule)) return;` at the top —
   one shared-read, no lock, on the overwhelmingly common path. Better: make the slot array
   `__read_mostly` with a `nr_active` counter, or drop the hook.
4. **Raise `ivh_eval_cooldown_ns`.** 50 µs → 5 ms recovered 105% (§1.2). The thing being gated is a
   vCPU property that changes on a millisecond timescale; 50 µs is far finer than the signal.

## 1.7 Two smaller things found while measuring this

- **`average_capacity_all` is permanently 0.** Every one of 316,450 `ivh_selected:` trace records
  reads `avg_cap=0`; the only distinct value in the whole ring buffer is `avg_cap=0`. It is passed
  into `test3()` and reaches `task_ctx.average_capacity`, which no live gate reads (only the dead
  `search_latency`, whose hook `cfs_latency_select` has no call site). **Harmless today, a
  landmine if any gate ever starts reading it.**
- **`reject_reasons[REJ_CAPACITY_LOW]` conflates two different gates** — the hard rail
  (`dcap <= IVH_CAP_HARDFLOOR`) and the top-band test (`dcap + IVH_CAP_TOPBAND < scan_max`), both at
  `MY_ivh_atc.bpf.c:579-590`. They need separate slots; see §1.8 for why that matters right now.

## 1.8 The floor is no longer the binding gate — and I could not reproduce a migrating regime

At `IVH_CAP_HARDFLOOR=950`, `REJ_CAPACITY_LOW` is 73.5% of gate evaluations and migrations are 0 —
as handed off. I rebuilt with `IVH_CAP_HARDFLOOR=600` to test the fixes in a migrating regime:

```
round=1 arm=base600    time=16.875 steal=25.8 migrations=0
round=1 arm=trylock600 time=13.819 steal=20.7 migrations=0  trylock_misses=577595
round=1 arm=noopt600   time=12.799 steal=19.3 migrations=0
(3 rounds; migrations = 0 in all 9 runs)
```

and an instrumented run at floor 600 showed mean per-candidate capacities of **613–731 — all above
600** — with `REJ_CAPACITY_LOW` still at **73.2%**.

**PROVEN:** at floor 600 the hard rail is *not* rejecting anything; the **top-band test**
(`dcap + 50 < scan_max`) is. Under tonight's uniform contention the 16 vCPUs sit within ~120 points
of each other, so nothing clears `scan_max − 50` **and** `src + 50`, and the destination set is empty
by design — exactly what `ivh_uc_gate_recalibration_2026-08-03.md` §5.2 predicted ("if every CPU is
equally stolen … IVH correctly does nothing — by construction rather than by accident").

**UNKNOWN, and I want to be blunt about it:** because migrations never fired tonight, I have **not**
shown that the Q4 fixes help in the regime IVH is supposed to operate in. What I have shown is that
they help in the regime IVH is *currently in*. The `trylock600` arm recovered the same ~70% (16.23 →
14.10 mean, `noopt` 13.00), which is at least consistent.

---

# 2. Q1 — `is_cs_preempted` vs `is_wait_preempted`

## 2.1 First, three precisions about what is actually being asked

1. **The function `is_cs_preempted()` has zero live call sites.** `grep` over the whole tree finds it
   only at its own definition (`arch/x86/include/asm/ivh_tsc_beat.h:573`). The live predicate is
   `ivh_cs_head_check()` (`kernel/locking/qspinlock_paravirt.h:494`), which open-codes the same
   expressions off a single `ivh_cs_age()` reading, and its only call site is
   `pv_wait_head_or_lock()` at `qspinlock_paravirt.h:1016`. Everything below is about that function.
2. **Nothing is exposed for it at the shipped setting.** `ivh_cs_preempt_src=0`, and every
   `ivh_cs_*` line in `/proc/ivh_debug` reads 0 (`ivh_cs_checks: 0`, `ivh_cs_publishes: 0`,
   `ivh_cs_age_hist_*` all zero). **So the literal answer to "can you observe it live" is: not as
   configured.**
3. **But it IS live-measurable without a rebuild**, because `ivh_cs_preempt_src=1` is a *shadow* mode
   by construction — `ivh_cs_head_check()` ends with `return (src == 2) && verdict;`, so at src 1
   every counter and histogram is fed under the real workload and the return value is unconditionally
   `false`. That is what I did. **This is the measurement that answers your question, and it
   contradicts the established framing.**

## 2.2 Live shadow measurement, 2026-08-06, current 16-sysbench uniform 2:1 scenario

`ivh_lock_holder_enabled=1`, `ivh_cs_preempt_src=1`, one full hackbench run per form, ~13.5 M
queue-head checks per arm. `is_wait_preempted`'s own 2×2 was captured **in the same windows**.

### `is_cs_preempted` (queue HEAD → lock HOLDER)

| form | n | prevalence | sensitivity | FPR | **precision** |
|---|---:|---:|---:|---:|---:|
| **0** (CS-stamp age, shipped default) | 13,546,576 | **3.4466%** | **69.83%** | 0.9373% | **72.67%** |
| 1 (in-CS AND heartbeat stale) | 13,630,920 | 3.0279% | 4.87% | 0.0706% | 68.32% |
| 2 (heartbeat stale only) | 13,661,031 | 3.9504% | 8.52% | 0.1019% | 77.48% |

Raw form-0 confusion matrix: **TP=326,033 FP=122,602 TN=12,957,077 FN=140,864.**

### `is_wait_preempted` (MCS node → `prev->cpu`), same three windows

| window | n | prevalence | sensitivity | FPR | **precision** |
|---|---:|---:|---:|---:|---:|
| with form 0 | 7,944,683 | 4.5543% | **0.348%** | 0.0312% | **34.76%** |
| with form 1 | 8,578,009 | 6.6305% | 0.324% | 0.0296% | 43.76% |
| with form 2 | 8,327,723 | 4.9159% | 0.367% | 0.0342% | 35.69% |

Raw, form-0 window: **TP=1,260 FP=2,365 TN=7,580,490 FN=360,568.**

**PROVEN, and it is the opposite of the premise.** Under the current scenario `is_cs_preempted`
form 0 is **more precise (72.7% vs 34.8%) and ~200× more sensitive (69.8% vs 0.35%)** than the
"good, validated" predicate. `is_wait_preempted` fired 1,260 true positives out of 361,828
opportunities in 18 seconds. **Its virtue right now is almost entirely the virtue of never firing.**

## 2.3 So is the base-rate argument wrong? No — it is right, and it is *conditional*

The argument is sound and I verified the arithmetic in both directions. What it shows is that
**precision is dominated by prevalence**, and prevalence is a property of the *workload and host
topology*, not of the predicate.

| measurement | prevalence | sens | FPR | precision |
|---|---:|---:|---:|---:|
| in-tree, 2026-07-30 (`ivh_tsc_beat.h:339-346`), ~27 M samples | **0.134%** | 34.15% | 0.203% | **18.36%** |
| the session figure handed to me | 2.05% | 74% | 2.2% | 41% |
| **tonight, uniform 2:1** | **3.4466%** | 69.83% | 0.9373% | **72.67%** |

All three are internally self-consistent (I recomputed each). Cross-substituting shows prevalence is
the dominant variable:

- the **2026-07-30 operating point** (sens 34.15%, FPR 0.203%) at **tonight's prevalence** would give
  **85.7% precision**;
- **tonight's operating point** (sens 69.8%, FPR 0.937%) at **2026-07-30's prevalence** would give
  **9.1% precision** — worse than what was measured then.

**The strongest correct form of your argument, which is stronger than how it was stated to me:**
sensitivity cannot rescue precision, because prevalence and FPR alone impose a **ceiling**:

```
precision_max = p / (p + (1-p)·FPR)      (i.e. at sensitivity = 100%)
```

- at p = 0.134%, FPR = 0.203% → **ceiling 39.8%**
- at p = 2.05%, FPR = 2.2% → **ceiling 48.8%**
- at p = 3.45%, FPR = 0.937% → **ceiling 79.2%**

So the claim "structurally independent of sensitivity" should be sharpened to: *sensitivity moves you
along a curve whose upper bound is fixed by prevalence and FPR.* At the 2026-07-30 prevalence that
bound is below 40% and the predicate is unusable no matter what you do to it. At tonight's prevalence
it is 79% and the predicate is fine. **Same predicate. Same code. 26× difference in prevalence.**

## 2.4 The cascading-cost argument is real, prevalence-independent, and I measured it

This is the argument that survives everywhere, and the code is unambiguous about it.

**What a false `is_wait_preempted` costs.** `pv_wait_early()` returns true → `pv_wait_node()` breaks
its spin, sets `VCPU_HALTED`, calls `pv_wait()`. The waiter's FIFO position is preserved; its
predecessor's `arch_mcs_spin_unlock_contended()` store still releases it; the outer `for(;;)`
re-checks `node->locked` regardless.

**Where I disagree with the premise as stated:** it is **not** fully private. The in-tree comment at
`qspinlock_paravirt.h:684-693` says so explicitly — an early `VCPU_HALTED` store drags the *next*
acquisition through `pv_kick_node()`'s `cmpxchg`, `_Q_SLOW_VAL`, `pv_hash()` **and the unlock
slowpath**, i.e. it bills work to the predecessor's *unlock*. The correct claim is **bounded and
local** — one extra halt/wake round trip, charged to at most two CPUs — not "no side effects on
others". That refinement strengthens the overall argument rather than weakening it.

**What a false `ivh_cs_head_check()` costs.** The function's own comment states it
(`qspinlock_paravirt.h:474-483`), and following the code path from the `break` at line 1017:

```c
			if (ivh_cs_head_check(lock, loop))
				break;
			...
		}
		clear_pending(lock);            /* line 1028 — the anti-steal bit is dropped */
		if (!lp) { lp = pv_hash(lock, pn);
			   if (xchg(&lock->locked, _Q_SLOW_VAL) == 0) { ... } }
		WRITE_ONCE(pn->state, VCPU_HASHED);
		pv_wait(&lock->locked, _Q_SLOW_VAL);
```

Three separate externalities, none of which is billed to the CPU that made the wrong call:

1. **`clear_pending()` runs early.** `pv_hybrid_queued_unfair_trylock()` steals only when
   `!(val & _Q_LOCKED_PENDING_MASK)` (line 100), and the entire starvation-freedom argument at the
   top of the file rests on the queue head holding that bit while it spins. Dropping it early
   reopens the stealing window **for every contender on that lock**, and the queue head can then be
   starved arbitrarily — which is precisely why the outer loop exists ("*Because of lock stealing,
   the queue head vCPU may not be able to acquire the lock before it has to wait again*", line 1071).
2. **The lock byte becomes `_Q_SLOW_VAL`.** From that moment, the **next unlock of that lock — by
   whoever holds it, including a perfectly healthy holder — takes `__pv_queued_spin_unlock_slowpath()`**:
   `smp_rmb()`, `pv_unhash()`, `pv_kick()`. The mis-predicting head has degraded the lock for its
   *owner*.
3. **The head's own wake now depends on that kick arriving**, rather than on a store it was already
   spinning on.

### Measured, tonight, form 0, `ivh_cs_preempt_src=1` (shadow — the bail is counted but not acted on)

```
ivh_head_bail_early     : 444,664  in 17.76 s  =  25,038 bails/s
ivh_head_bail_loop_hist :  ... b8=2932 b9=5874 b10=11776 b11=23799 b12=48542 b13=101771 b14=239220 b15=10750
```

`SPIN_THRESHOLD` is `1 << 15` and the loop **counts down**, so a high bucket means the bit is dropped
early. Buckets 13–15 (`loop ≥ 8192`) hold **351,741 of 444,664 = 79.1%**; buckets 14–15
(`loop ≥ 16384`, i.e. within the **first half** of the spin budget) hold **249,970 = 56.2%**.

By the function's own stated criterion — "*a distribution concentrated near SPIN_THRESHOLD means the
bit is being released almost immediately and the starvation guarantee is effectively gone*" —
**this distribution is the bad one.** PROVEN.

### And the A/B that turns it into a number

`ivh_cs_preempt_src=1` (measure, don't act) vs `=2` (act), **form 0, instrumentation cost identical
in both arms**, 4 rounds each:

| src | hackbench time (s) | mean | `ivh_head_bail_early` | `ivh_lock_steals` |
|---|---|---:|---|---|
| 1 (shadow) | 18.763 16.817 18.209 20.779 | **18.64** | 355,549 – 389,045 | 5,736,043 – 5,865,781 |
| 2 (authoritative) | 18.635 19.264 17.812 18.321 | **18.51** | **3,868,136 – 4,218,010** | **6,966,194 – 7,176,029** |

**PROVEN:** acting on the predicate causes **~10× more bail events** (each bail re-arms the outer
loop, which sets pending again and can bail again) and **+20% lock steals** — the externality,
quantified, exactly where the code says it would appear.

**Also PROVEN, and it is a correction:** it caused **no measurable throughput change** (18.64 vs
18.51 s, n=4 each, run-to-run σ ≈ 1.5 s). **Under the current scenario, `is_cs_preempted` at src=2 is
not "actively harmful" to hackbench throughput.** I cannot reproduce that part of the premise.

## 2.5 On "it re-stamps more often" — deliberately not foregrounded, and here is why that is justified

The structural asymmetry is real and I confirmed the write sites:

- `ivh_tsc_beat` (wait heartbeat): **four** publish sites — the spin loop
  (`qspinlock_paravirt.h:435`, one store per 4096 `cpu_relax()`, ≈90 µs), `pv_init_node()` (`:734`),
  `account_process_tick()` (`kernel/sched/cputime.c:549`, 1 kHz), and `ivh_pv_wait()`
  (`arch/x86/kernel/kvm.c:2172`).
- `ivh_cs_beat` (CS stamp): **one** publish site, `cs_enter()` at `kernel/locking/spinlock.c:408`,
  cleared at `cs_exit()` (`:475`). Written once at CS entry, never refreshed.

**But it cannot be the primary explanation, and this is provable rather than a matter of emphasis:
form 0's verdict never reads a heartbeat at all.** It is `age >= 0 && age > ivh_cs_beat_threshold`
on the CS stamp alone (`qspinlock_paravirt.h:574-575`). Refresh cadence explains why forms 1 and 2
are *worse than form 0* — which is exactly what the 2026-07-30 in-tree table found — but it cannot
explain form 0's behaviour, and form 0 is the default and the best form in both the old measurement
and mine. **Your instinct to demote it is correct.**

## 2.6 Evaluating your adaptive-threshold idea — measured, and the answer is "no, but…"

I swept the threshold offline against tonight's live `ivh_cs_age_hist_running[]` /
`ivh_cs_age_hist_preempted[]` (13,079,679 running + 466,897 preempted samples):

| threshold (cycles) | ≈ µs | sens | FPR | **precision** |
|---:|---:|---:|---:|---:|
| 2^10 = 1,024 | 0.5 | 77.25% | 1.585% | 63.50% |
| 2^14 = 16,384 | 7.4 | 76.11% | 1.130% | 70.62% |
| **220,000 (shipped)** | **100** | **69.8%** | **0.937%** | **72.7%** |
| 2^18 = 262,144 | 119 | 59.70% | 0.643% | 76.83% |
| **2^19 = 524,288** | **238** | 43.33% | 0.444% | **77.70% ← max** |
| 2^20 = 1,048,576 | 477 | 23.04% | 0.258% | 76.11% |
| 2^21 = 2,097,152 | 953 | 1.76% | 0.069% | 47.43% |

**Two hard results, both PROVEN from the histograms:**

1. **The maximum precision achievable by *any* threshold on this data is 77.70%**, and the shipped
   220,000 cycles already sits at 72.7% — essentially on the knee. The curve is nearly flat from
   1 µs to 240 µs. **An adaptive threshold that is a function of a *global* CS-length statistic can
   only move along this curve. It cannot beat 77.7%.** The ceiling is set by the *overlap* between
   the healthy-CS-duration tail and the preempted-CS-duration body, not by the choice of a constant.
   So: the idea as floated is not where the win is.
2. **The sensitivity ceiling is exactly 77.2%, and it equals the stamp-coverage rate.** Bucket 0 of
   `ivh_cs_age_hist_preempted` holds **106,237 of 466,897 = 22.8%** of preempted samples — those are
   holders with **no CS stamp at all** (the `age < 0` sentinel): holders in softirq/hardirq context,
   or holding an inner lock, or acquiring outside the `kernel/locking/spinlock.c` wrappers.
   `100% − 22.8% = 77.2%`, exactly the observed ceiling. **No threshold, adaptive or not, can see
   those.**

### What I would propose instead — three things, in value order. **None implemented, per your instruction.**

**(a) Close the stamp-coverage gap — this is the biggest single win available.** Move the CS stamp
from `cs_enter()` to the qspinlock layer, where holder *identity* already lives
(`ivh_lock_set_holder()`, sites A3–A9). Identity already covers every acquisition in the kernel; the
stamp covers only outermost, non-interrupt, wrapper-routed acquisitions. Unifying them lifts the
sensitivity ceiling from 77.2% toward 100% **at unchanged FPR** — a strictly larger effect than any
threshold move. It is the same edit that makes forms 1 and 2 stop being systematically blind.

**(b) The *right* version of your adaptive idea: make it per-lock, not per-system.** The 77.7%
ceiling exists because one global threshold is applied to a population that mixes locks with wildly
different natural hold times. A threshold of the form `k × (this lock's own EWMA hold)` is a
**different ROC curve**, not a different point on this one, and can in principle beat the ceiling.
Crucially, **the storage already exists**: the holder-identity table is already a per-lock hash slot,
written at every acquire and read at exactly the point the verdict is taken. One extra `u32` of EWMA
hold-cycles per slot would cost one store on a line that is already dirty:

```c
/* SKETCH ONLY — not applied. include/linux/ivh_lock_holder.h, slot layout */
struct ivh_holder_slot {
        u32 tag;
        u16 holder_cpu;
        u16 ewma_hold_c_log2;   /* NEW: per-lock EWMA of hold length, log2 cycles */
};
/* verdict becomes, in ivh_cs_head_check(): */
        verdict = (age >= 0 && age > ((s64)1 << (slot->ewma_hold_c_log2 + k)));
```
Note what this is *not*: it is not `current->last_cs_ns`. That field describes the **waiter's own**
last critical section and is the wrong quantity — it says nothing about the lock being waited on.
If "derived from `last_CS`" meant that field, I think it is the wrong input; if it meant "the
holder's recent CS length **on this lock**", it is the right input and needs the per-lock slot above.

**(c) Gate on prevalence, since prevalence is what actually decides.** §2.3 shows precision is
dominated by a quantity that swings 26× with host topology — and the guest can observe a proxy for it
directly, in `rq->ivh_ref_steal_ns`, with no host cooperation. A `ivh_cs_min_steal_pct` that leaves
the bail disarmed below some observed steal rate would turn the base-rate failure mode from
"silently unusable at low prevalence" into "off at low prevalence, on at high". This is the cheapest
of the three and directly targets the mechanism §2.3 identifies.

**(d) Independently of all of the above — fix the externality, since it is unconditional.** Do not
`clear_pending()` on a CS-preemption bail. Keep the anti-steal bit set while the head parks in
`pv_wait()`; `trylock_clear_pending()` needs `pending == 1` to succeed anyway, and the outer loop
calls `set_pending()` again on wake, so leaving it set is idempotent. That removes externalities (1)
and, by keeping the head able to acquire, reduces the pressure that produces (2). **Risk to check
before building it:** a head that parks with `pending` set relies entirely on the `pv_kick()` from
the unlock slowpath arriving — which it does, because `xchg(&lock->locked, _Q_SLOW_VAL)` forces the
holder's unlock through `__pv_queued_spin_unlock_slowpath()`. I have **not** verified this against
the `_Q_PENDING_BITS != 8` variant.

## 2.7 Health of the holder table tonight (it is fine)

```
ivh_holder_stamps            139,859,228     ivh_holder_clears   121,415,106   (87% matched)
ivh_holder_unknown_empty       6,289,093     ivh_holder_unknown_collision  11,372
ivh_holder_raced               2,375,628     ivh_holder_self               0
ivh_cs_clear_mismatch                231
```

The 680,000:1 stamps/clears imbalance of `rseqport67` is gone — the R2b fix at
`arch/x86/include/asm/qspinlock.h:128` works. `ivh_holder_self = 0` exactly. `ivh_holder_raced` is
17.5% of 2×2-eligible checks, which is the read-verify-read skew rate and is conservative in the safe
direction (it discards a verdict, never inverts one).

---

# 3. Q2 — is "kick, halt, lock-stealing" the complete list of what PV adds?

## 3.1 Method: the substitution surface is finite and I enumerated it exhaustively

`kernel/locking/qspinlock.c` is compiled **twice**. The second pass (`qspinlock.c:554-569`) redefines
exactly this set and re-`#include`s itself:

```c
#define _GEN_PV_LOCK_SLOWPATH
#undef  pv_enabled
#define pv_enabled()	true
#undef pv_init_node
#undef pv_wait_node
#undef pv_kick_node
#undef pv_wait_head_or_lock
#undef  queued_spin_lock_slowpath
#define queued_spin_lock_slowpath	__pv_queued_spin_lock_slowpath
#include "qspinlock_paravirt.h"
#include "qspinlock.c"
```

plus one more, defined inside the header at `qspinlock_paravirt.h:89`:

```c
#define queued_spin_trylock(l)	pv_hybrid_queued_unfair_trylock(l)
```

`grep` for every one of those identifiers inside `qspinlock.c` gives the complete list of points at
which the PV build can diverge: lines **216, 219, 368, 389, 396, 426, 461, 533**. There is nowhere
else. Separately, `pv_ops.lock.*` is populated at `arch/x86/kernel/kvm.c:2656-2661`
(`queued_spin_lock_slowpath`, `queued_spin_unlock`, `wait`, `kick`) and `vcpu_is_preempted` at
`:847`. **That is the entire surface.**

## 3.2 The complete diff, bucketed

| # | site | native behaviour | PV behaviour | fits kick/halt/steal? |
|---|---|---|---|---|
| **D1** | `qspinlock.c:216` `if (pv_enabled()) goto pv_queue;` | falls through to the pending-bit section (lines 231–325): the **second** waiter sets `pending`, spins on `lock->locked`, acquires **without ever touching an MCS node** | **the entire pending-bit fast path is dead.** Every contended acquisition allocates an MCS node and queues | **NO** — see §3.3 |
| **D2** | `qspinlock.c:219` `virt_spin_lock(lock)` | test-and-set fallback for non-PV hypervisors, gated on `virt_spin_lock_key` | **skipped by the same `goto`.** (Also runtime-dead: `kvm_spinlock_init()` does `static_branch_disable(&virt_spin_lock_key)` on *every* path, `kvm.c:2668`) | **NO** |
| **D3** | `qspinlock.c:368, 396` `queued_spin_trylock()` | plain `atomic_try_cmpxchg_acquire` on the whole word | `pv_hybrid_queued_unfair_trylock()` — spins in **unfair mode**, stealing the lock ahead of the queue as long as waiters are queued but `pending` is clear | **lock stealing** ✔ |
| **D4** | `qspinlock.c:389` `pv_init_node()` | no-op | records `pn->cpu`, `pn->state = VCPU_RUNNING` | halt/kick bookkeeping ✔ |
| **D5** | `qspinlock.c:426` `pv_wait_node()` | `arch_mcs_spin_lock_contended()` — unbounded spin on `node->locked` | bounded `SPIN_THRESHOLD` (`1<<15`) spin with `pv_wait_early()` checks (`prev->state != VCPU_RUNNING`, or `vcpu_is_preempted(prev->cpu)`), then `VCPU_HALTED` + `pv_wait()` | **halt** ✔ (+ adaptive-spin trigger) |
| **D6** | `qspinlock.c:533` `pv_kick_node()` | no-op | `cmpxchg(HALTED→HASHED)`; on success `WRITE_ONCE(lock->locked, _Q_SLOW_VAL)` + `pv_hash()`. *(IVH adds `smp_send_reschedule(pn->cpu)` at `:947` under mechanisms 1/2 — a project modification, not stock PV)* | **kick/halt protocol** ✔ |
| **D7** | `qspinlock.c:461` `pv_wait_head_or_lock()` | `atomic_cond_read_acquire(&lock->val, !(VAL & _Q_LOCKED_PENDING_MASK))` — a pure spin | the head **sets `pending`** (something the native head never does), spins `SPIN_THRESHOLD` on `trylock_clear_pending()`, then `clear_pending()`, `pv_hash()`, `xchg(&lock->locked, _Q_SLOW_VAL)`, `pv_wait()`; loops (`for(;; waitcnt++)`) because a stealer may take the lock. Non-zero return makes qspinlock.c **skip** its own `atomic_cond_read_acquire` | halt ✔ **+ pending repurposed** — see §3.3 |
| **D8** | unlock | call site is **ALTERNATIVE-patched to inline `movb $0,(%rdi)`** (`ALT_NOT(X86_FEATURE_PVUNLOCK)`, `arch/x86/include/asm/paravirt.h:560-564`) | `PV_UNLOCK_ASM`: **`LOCK cmpxchg %dl,(%rdi); jne .slowpath`** (`arch/x86/include/asm/qspinlock_paravirt.h`), falling into `pv_unhash()` + `pv_kick()` on `_Q_SLOW_VAL` | **NO** — see §3.3 |
| **D9** | `struct qnode` | `{ struct mcs_spinlock mcs; }` — one 64-byte line | `+ long reserved[2]` (`kernel/locking/qspinlock.h:40-45`); "PV doubles the storage and uses the second cacheline for PV state" | halt/kick storage ✔ |
| **D10** | boot | — | `__pv_init_lock_hash()` allocates `max(4·nr_cpus, PAGE_SIZE/16)` hash entries | halt/kick storage ✔ |
| **D11** | — | — | `pv_ops.lock.vcpu_is_preempted` (`kvm.c:847`) — a *new signal*, the steal bit, consumed by `pv_wait_early()` | enabler ✔ |

## 3.3 The three that do not fit your three buckets

**(a) The pending-bit optimisation is deleted, and the bit is repurposed (D1 + D7).**
Native qspinlock's pending bit is a **performance** feature: the second contender for a lock acquires
it without ever building an MCS queue node — the single most common contended case. PV throws that
away entirely and re-uses the same bit as a **fairness** flag owned by the queue head, whose only
job is to make `pv_hybrid_queued_unfair_trylock()` refuse to steal. It is a *counterweight to*
stealing, which is a different thing from stealing. This is also why the queue head's own comment
about `clear_pending()` (Q1, §2.4) matters so much: in native qspinlock the queue head has no such
responsibility at all.

**(b) Every `spin_unlock` in the kernel becomes a locked RMW (D8).**
Not "every contended unlock" — **every unlock**, including the millions of uncontended ones that
never touch the slowpath. Native x86-64 releases with a plain byte store (store-release is free under
TSO). PV releases with `LOCK cmpxchg`. This is a consequence of the halt protocol (the releaser must
detect `_Q_SLOW_VAL` to know it owes a kick), so it is *caused by* halt — but it lands on a code path
that the "kick / halt / lock-stealing" summary does not suggest is touched at all, and it is a
non-trivial standing cost. In tonight's profiles `__raw_callee_save___pv_queued_spin_unlock` was
**4.03% of all cycles** as a self-time leaf.

**(c) `virt_spin_lock()`'s test-and-set fallback is bypassed (D2).**
Runtime-dead on this host, but it is a real difference in the compiled code and worth stating for the
`nopvspin` comparison, where it becomes live again.

## 3.4 Verdict

**Your belief is correct as a taxonomy of *intent*: everything PV adds exists to serve kick, halt, or
lock-stealing.** It is incomplete as a description of *what changes*. The three items above are
consequences that a reader of the summary would not predict, and (a) and (b) both have direct
performance and fairness implications that matter to IVH specifically — (a) because IVH's CS
predicate manipulates that very bit, and (b) because it is a flat tax on the whole kernel that any
IVH-vs-native comparison silently includes.

---

# 4. Q3 — can `UNHALTED.REF` and plain TSC be trusted for steal inference here?

## 4.1 Method

`/proc/vcap_steal_compare` snapshotted at two instants, with **both `real_steal_ns` and
`inferred_steal_ns` diffed across the same window** — plus `/proc/interrupts` and `/proc/stat` at the
same instants. No cross-epoch comparisons anywhere. Two load conditions: **idle** (guest doing
nothing) and **load** (back-to-back `ivh_exec hackbench` for the whole window).

## 4.2 Result 1 — at idle the estimator is excellent, and the 2026-08-04 regime does not reproduce

`ivh_ref_method=0`, 120 s idle window, real steal ~1.1% of wall on every vCPU:

```
 cpu   d_real_ns   d_infer_ns    err%    d_LOC  d_oth  ticks  phantom_ns/irq
   0  1360645998   1360747887     0.01     5108   3990   4703            11
   4  1173728563   1171601512    -0.18     6316   6470   5590          -166
  11  1346801148   1367941571     1.57     5124    584   4798          3704
  13  1414993964   1428919426     0.98     4279    808   4218          2737
 err% over CPUs with real steal>0:  min -0.29  max 1.57  mean 0.73  worst|err| 1.57
```

**PROVEN:** worst error **+1.57%**, mean **+0.73%** — versus **+200% to +380%** on 2026-08-04.

**And the 2026-08-04 root cause is confirmed rather than contradicted.** The phantom tax is
*identical in absolute terms* (cpu13: `1,428,919,426 − 1,414,993,964 = 13.9 ms` over 4,218 ticks =
**3,300 ns/tick**, right in the 2,600–3,100 ns/irq band that document fitted). What changed is the
**denominator**. In the old 8-pinned-corunner topology cpu8–15 had ~7 ms of real steal per 90 s; in
tonight's uniform topology **every** vCPU accrues ~1.3 s of real steal per 120 s even while the guest
is idle. §1.1 of that document said exactly this ("*It only looks like two different populations
because it is divided by two wildly different denominators*") — tonight generalises it.

`ivh_ref_method=2` at idle: mean **−1.30%**, worst **−2.90%**. Slightly over-corrected, still fine.

## 4.3 Result 2 — under sustained guest load the estimator under-reports by 25–35%, and method 2 makes it worse

Same measurement, ~110 s of back-to-back hackbench, ~25% real steal:

| `ivh_ref_method` | mean err% | range |
|---|---:|---|
| **0 (shipped)** | **−30.05%** | −24.98 … −34.90 |
| **2 (deadband authoritative)** | **−33.53%** | −27.50 … −39.06 |

**PROVEN.** This is a *new* regime, not covered by 2026-08-04. That document's "loaded CPUs stay
within ~3%" meant **host**-loaded CPUs with an **idle guest**. Tonight's "load" is both. And the
`phantom_ns/irq` column goes **negative** (≈ −45,000 to −56,000), so the exit-overhead deadband —
which only ever subtracts more — moves the estimate in the wrong direction here.

## 4.4 Root cause, isolated live: the lock-path halt/poll correction is a large over-subtraction

`ivh_ref_accumulate()` subtracts halted/polled cycles from the steal estimate
(`kernel/sched/core.c:815-818`):

```c
	correct = READ_ONCE(ivh_ref_halt_correct);
	sub_c  = (correct >= 1) ? d_hlt_c  : 0;
	sub_c += (correct >= 2) ? d_poll_c : 0;
```

Sustained-load windows (~95 s each), sweeping that sysctl:

| `ivh_ref_halt_correct` | what it subtracts | **mean err%** | range |
|---|---|---:|---|
| **2** (shipped) | HLT + poll | **−30.1%** | −25.0 … −35.3 |
| **1** | HLT only | **−14.2%** | −10.5 … −17.7 |
| **0** | neither | **−5.4%** | −10.1 … **+0.9** |

with the buckets themselves, per vCPU per ~95 s window: `d_hlt_ns ≈ 1.7–3.7 s`,
`d_poll_ns ≈ 4.1–9.8 s`, against `d_real_steal ≈ 17–26 s`. Adding both back arithmetically moves
cpu0 from **−32.2% to −7.9%** and cpu15 from **−26.2% to −2.4%**.

**And at idle it costs nothing.** Matched idle windows:

| `ivh_ref_halt_correct` | mean err% | worst \|err\| |
|---|---:|---:|
| 0 | +0.35% | 1.68% |
| 2 | +0.31% | 0.93% |

**PROVEN: `ivh_ref_halt_correct=0` is strictly better tonight — −30% → −5% under load, no
measurable cost at idle.**

This settles an open question the source itself poses. `arch/x86/kernel/kvm.c:2465-2474` says of the
poll bucket: *"whether these cycles are lost to REF_TSC is an open question … Keeping the two buckets
separate is what makes `ivh_ref_halt_correct=1` vs `=2` a real experiment: if 2 is what restores
agreement with host steal time, TPAUSE does stop the counter; if 1 already suffices, it does not."*
**The experiment has now been run and the answer is neither: 2 over-corrects badly and 1 still
over-corrects.**

**INFERRED** (two candidate mechanisms, not separated):
- For the **poll** bucket: TPAUSE C0.2 apparently does *not* stop `CPU_CLK_UNHALTED.REF` enough to
  matter here (or the loop's non-TPAUSE portion dominates), so those cycles were never booked as
  steal in the first place and subtracting them is a plain double-subtraction.
- For the **HLT** bucket: `ivh_lock_halt_begin/end` brackets the *whole* halt, which includes the
  host's **resume latency** after the wake — and that latency is genuine steal, which the host counts
  and the correction removes.

Separating these needs one more counter (time from wake-IPI to resume), i.e. a rebuild. **UNKNOWN.**

## 4.5 Evaluating your structural argument — it is two-thirds true

> *"it seems like we can trust TSC since our capacity and last_preemption calculations are built the
> same way as the steal-page-derived numbers"*

Traced through the source, the premise splits three ways:

| quantity | actually fed by | TSC-built? |
|---|---|---|
| **capacity** (`vcap` → `rq->cpu_capacity`, and `rq->ivh_uc_capacity`) | `get_steal_and_preemptions()`, which at `ivh_steal_source=1` returns `rq->ivh_ref_steal_ns` (`kernel/sched/core.c:310-322`) | **YES** ✔ |
| **Gate 2's preemption events** at `ivh_preempt_event_source=2` (current) | Part C: `rq->ivh_vact_last_preempt_tsc`, `ivh_vact_last_active_c` (`core.c:1700-1707`) | **YES** ✔ |
| **`rq->last_preemption`, `rq->last_active_time`, `rq->preemptions`, `rq->max_latency`** | **`steal_account_process_time()` calls `paravirt_steal_clock()` DIRECTLY and unconditionally** (`kernel/sched/cputime.c:275-297`) — it never routes through the switchable accessor | **NO** ✘ |
| **`rq->clock_preempt`** (read by the kernel's *and* the BPF program's `is_cpu_preempted()`) | `this_rq()->clock_preempt = sched_clock();` at `cputime.c:522` — a liveness heartbeat, neither steal-page nor REF_TSC | **neither** |

**PROVEN.** So the reasoning holds for the two quantities you named it for, and **fails for a fourth
group you did not**. The practical consequence, which matters exactly for the confidential-computing
motivation: on a host that does **not** expose a steal page, `paravirt_steal_clock()` returns 0,
`steal_account_process_time()`'s `if (steal > 0)` never fires, and `rq->last_preemption`,
`rq->last_active_time`, `rq->preemptions` and `rq->max_latency` **freeze at their boot values,
silently**. Today that is masked because `ivh_time_left_source=1` + `ivh_preempt_event_source=2`
routes Gate 2 around them — but `GATE_BURST_ORDER`/`GATE_BURST_BUDGET` in the BPF program read
`select_rq->last_preemption` and `select_rq->ewma_act_ns`, and the `is_cpu_preempted()` used by the
destination scan reads `rq->clock_preempt`. Two of those are compiled out today; one is not.

## 4.6 So: can TSC be trusted?

**For ordering and for gating, yes — with a caveat that is now quantified.**

- **PROVEN:** at idle, ±1.6%. Better than the `ivh_ref_carry=1` regime documented in
  `core.c:822-829` ("63–74% below") by an order of magnitude.
- **PROVEN:** under sustained load, **−30% as shipped**, improvable to **−5% by a single sysctl
  write** (`ivh_ref_halt_correct=0`), with no idle-side cost measured.
- **PROVEN:** the error is **one-signed** (always an under-report) by construction — the two clamps
  at `core.c:790-791` only ever add a non-negative quantity. For a signal that *gates migrations*,
  under-reporting steal means under-triggering, which is the safe direction. That is a genuine
  argument for trusting it operationally even at −30%.
- **The thing that would break it** is not the magnitude but the **regime-dependence**: −0.7% at idle
  and −30% under load is a 30-point swing driven by guest demand, which is exactly the failure shape
  `ivh_uc_gate_recalibration_2026-08-03.md` §2.4 documented for `ivh_uc_capacity` and concluded was
  fatal to any *absolute* threshold. **The same conclusion applies here, for the same reason.**
  Consume this number ordinally (rank vCPUs), never as an absolute rate.

**Next steps for Q3:**
1. **Live, tonight, free:** set `ivh_ref_halt_correct=0` and watch `/proc/vcap_steal_compare` over a
   long mixed-load window. I have restored it to **2** (the value I found) rather than leave a
   behavioural change in place unattended — this is your call, not mine. §5.
2. **Rebuild-needed:** split the HLT bracket into "halted" and "resume latency" so the correction can
   subtract only the former. That is the measurement that distinguishes the two mechanisms in §4.4.
3. **Rebuild-needed:** route `steal_account_process_time()`'s writes through
   `get_steal_and_preemptions()` (or a Part-C equivalent) so `rq->last_preemption` and friends stop
   being a hidden hard dependency on the steal page.
4. `ivh_ref_method` is a solution to a problem this topology does not have. It is correct and
   well-founded for **lightly-stolen vCPUs**; keep it, keep it at 0 by default here, and re-enable it
   if the corunner ever goes back to hitting a subset of pCPUs.

---

# 5. State left behind

**Everything below was verified by direct read after the last experiment finished.**

## 5.1 Kernel

**Untouched.** No source edit, no rebuild, no reboot. `git status` shows only the three
already-modified files that were modified before this session started (`kernel/sched/core.c`,
`kernel/sched/fair.c`, `kernel/sched/sched.h`).

## 5.2 `tools/bpf/MY_ivh_atc.bpf.c`

**Restored to exactly the source I was handed, and the running daemon was rebuilt from it.**

```
md5sum:  91418dabe326e8608a8dbba39193b37b   MY_ivh_atc.bpf.c
         91418dabe326e8608a8dbba39193b37b   (baseline copy taken at 02:5x, before any edit)
git diff:  -#define IVH_CAP_HARDFLOOR  600
           +#define IVH_CAP_HARDFLOOR  950      <- the only change vs HEAD, as handed off
```

`MY_ivh_atc.bpf.o`, `MY_ivh_atc.skel.h` and the `MY_ivh_atc` binary were rebuilt (build artifacts,
untracked). Four experimental variants were built and loaded during the session
(`nocapstat`, `earlyexit`, `both`, and a `HARDFLOOR=600` build); **none of them survives.**

## 5.3 Daemons

```
MY_ivh_atc : 1 instance (rebuilt from the baseline source and restarted)
vcap       : 1 instance (never touched)
ivh_cfg[0] : 0        (cap_source = vcap, as handed off)
```

## 5.4 Sysctls — all restored to the values found at session start

Confirmed by full `for f in /proc/sys/kernel/ivh_*` dump after the last experiment. The ones I
**changed and put back**:

| sysctl | found | touched during | **left at** |
|---|---|---|---|
| `ivh_universal_eligible` | 1 | 0 | **1** |
| `ivh_capacity_threshold` | 1010 | 0 | **1010** |
| `ivh_eval_cooldown_ns` | 50000 | 1000000 / 5000000 | **50000** |
| `ivh_selection_trylock` | 0 | 1 | **0** |
| `ivh_cs_preempt_src` | 0 | 1, 2 | **0** |
| `ivh_cs_predicate_form` | 0 | 1, 2 | **0** |
| `ivh_lock_holder_enabled` | 0 | 1 | **0** |
| `ivh_ref_method` | 0 | 2 | **0** |
| `ivh_ref_halt_correct` | 2 | 0, 1 | **2** |

Everything else (`ivh_pv_preempt_src=2`, `ivh_pv_wait_mechanism=2`, `ivh_pv_kick_pure_ipi=1`,
`ivh_steal_source=1`, `ivh_preempt_event_source=2`, `ivh_cap_source=0`, `ivh_cs_beat_threshold=220000`,
`ivh_ref_carry=1`, `ivh_decision_shadow=0`, `ivh_uc_*`) is untouched.

## 5.5 Other

- `/sys/kernel/debug/tracing/tracing_on` = **1** (found at 1; briefly set to 0 for the `tracingoff`
  arm and restored).
- The ftrace ring buffer contains `ivh_selected:` records from the runs. It is a ring; nothing to
  clean up. If you want a clean baseline: `echo > /sys/kernel/debug/tracing/trace`.
- **Cumulative counters are polluted for this boot.** `ivh_migrations_done`, `reject_reasons`,
  `cap_sum`/`cap_cnt`, `ivh_lock_steals`, `ivh_cs_*`, `ivh_holder_*` all include this session's ~90
  hackbench runs, three `MY_ivh_atc` restarts (which zero the BPF maps), and the shadow-mode arms.
  Snapshot before/after any new window; do not read absolutes.
- Scratch scripts and raw logs (`matrix.log`, `matrix2.log`, `q1_ab.log`, `floor600.log`, perf data,
  the baseline BPF source copy) are in
  `/tmp/claude-1000/-home-nick-kernels-linux-6-17-rseqport/4ab2961e-3163-49c1-b957-e10d703a5ef1/scratchpad/`.
  Nothing outside that directory and this file was created.

## 5.6 Things I did NOT do, on purpose

- Did not implement any Q1 solution (you asked me not to).
- Did not leave `ivh_selection_trylock=1` on, even though it is the single best free win tonight —
  it changes IVH's behaviour (~40% fewer selection attempts), and that is a decision for you.
- Did not leave `ivh_ref_halt_correct=0` on, for the same reason.
- Did not leave the `scan_max` early exit applied, even though it recovered 80%.
- Did not touch `git`.

---

# 6. Open questions I could not close

1. **Do the Q4 fixes help when migrations actually fire?** Tonight's host never produced a
   destination set (§1.8). Needs a corunner topology with real cross-vCPU asymmetry.
2. **Why does the perf frame-pointer callgraph disagree with two kprobe measurements about
   `ivh_scan_stuck_waiters`?** (§1.5b). I discarded the perf reading; I did not explain it.
3. **HLT-bracket decomposition** (§4.4) — needs a resume-latency counter, i.e. a rebuild.
4. **Is the per-lock adaptive threshold (§2.6b) actually a better ROC curve, or only a different
   point on the same one?** Cannot be answered from the global histogram; needs per-lock hold-time
   state, i.e. a rebuild.
5. **`ivh_head_bail_loop_hist` under `src=2`** — I measured the distribution in shadow only. The
   authoritative arm's distribution is different (bails cascade, ~10×) and I did not capture its
   shape.
