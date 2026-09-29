# Why the IVH win halved: the G-LOCK-40 RCU guard (2026-09-29)

`ebizzy_mmap` was recorded at **+104.3%** in the September campaign and measures
**+43% to +54%** today. This document bisects that to a single commit and shows
the old number was obtained illegally.

## 1. The answer

**`3afd480f5621` -- "G-LOCK-40: do not migrate from inside an RCU read-side
critical section"** (2026-09-25) added one guard to `ivh_pre_lock()`:

```c
+	if (rcu_preempt_depth())
+		return;
```

From the commit message:

> With `CONFIG_PREEMPT_RCU=y`, `rcu_read_lock()` does NOT raise `preempt_count`,
> so `preemptible()` above is TRUE inside an RCU reader -- and an enormous share
> of `spin_lock()` callers (dcache, lockref, net, slab) hold one.
> `bpf_sched_pre_lock_migrate()` then does two things that are illegal there:
>   1. `alloc_cpumask_var(&saved_mask, GFP_KERNEL)` -- a sleeping,
>      reclaim-capable allocation;
>   2. `set_cpus_allowed_ptr() -> affine_move_task() -> wait_for_completion()`
>      -- a VOLUNTARY context switch. Preemption inside a preemptible-RCU reader
>      is legal; blocking is not, and it extends the grace period by the whole
>      migration latency.
>
> This build cannot warn about it: `CONFIG_DEBUG_ATOMIC_SLEEP` and
> `CONFIG_PROVE_LOCKING` are both off, so `might_sleep()` degrades to
> `might_resched()` and `rcu_sleep_check()` is a no-op. The symptom would be RCU
> stalls under memory pressure, not a splat.

**The pre-2026-09-25 numbers are inflated by migrations the kernel is not
allowed to perform.** The gain is not recoverable and should not be recovered.

## 2. The bisection

| date | kernel | pv | ivh | ratio |
|---|---|---|---|---|
| 09-15 | G-LOCK-30-csfast | 978.0 | 2008.5 | 2.05x |
| 09-24 | **G-LOCK-39-sampler** | 538/543 | 2145/2104 | **3.99x** |
| 09-25 | **G-LOCK-40-rcufix** | 933 | 1601 | **1.72x** |
| 09-29 | G-LOCK-48-skipcheck | 990 | ~1420 | 1.43x |

Sources: `campaign/run_main/results.csv`, `campaign/smoke_0924_073231/results.csv`,
`campaign/as_ranking_0925_074920.csv`, `whyless_0929-*.csv`. The ratio collapses
between G-LOCK-39 and G-LOCK-40, and G-LOCK-40 is a single-purpose commit.

## 3. The direct measurement

On G-LOCK-30 (no guard), probing every entry to `bpf_sched_pre_lock_migrate`
during an `ebizzy_mmap` run and reading
`((struct task_struct *)curtask)->rcu_read_lock_nesting`:

```
@total:          528,103
@in_rcu_reader:  528,103   (100%)
@not_in_rcu:           0
nesting depth    1: 86,251   2: 401,086   3: 2,021   4: 38,745
```

**Every migration candidate is inside an RCU reader**, mostly at depth 2.

Consistent with G-LOCK-48, where `ebizzy` logs ~8.5M `ivh_prelock_calls` and
~5,000 migrations -- **0.06% survival**. The guard removes essentially the whole
candidate population; what remains is the residue.

**Caveat.** `ivh_pre_lock()` is inlined and absent from `/proc/kallsyms`, so the
probe sits at `bpf_sched_pre_lock_migrate` instead. Some of that nesting could in
principle be entered by IVH's own code between the guard site and the probe
point. One probe point, not two. The magnitude and the workload's character
(`mmap_lock`, dcache, lockref -- all RCU readers) support the reading.

## 4. What was ruled out first

Each by measurement, no probes, n=4-5, at matched contention:

| hypothesis | verdict | evidence |
|---|---|---|
| adaptive spinning hurts ebizzy | refuted | mig_only 1399 / mig_t1 1419 / full 1377 -- within 0.3% |
| bpftrace probe tax | refuted | ebizzy 0.2pp (schbench 1.08pp, workload-dependent) |
| host contention | refuted | capacity 505 (campaign 456-475) still gave +48.8% |
| cs tracking / Gate 2 input | refuted | cs off 1401.5 vs cs on 1396.5 |
| migration engine changed | refuted | `fair.c` byte-identical G-LOCK-30..HEAD |
| BPF selector changed | refuted | `tools/bpf/` unchanged, tree clean |
| PV baseline drifted | refuted | 978 -> 989 |

`mig_only` is `adaptive_mode=0` + `universal_eligible=1`: migration on the stock
PV lock path. `ivh_pre_lock()` has no `adaptive_mode` gate, so this is valid and
it isolates the migration engine completely.

## 5. G-LOCK-30 is NOT reproducible from the repo

Building it (`/root/kernels/glock30`, worktree at `9be8425a00d2`) succeeds, but
the BPF selector cannot be rebuilt against it:

```
MY_ivh_atc.bpf.c:1059: error: no member named 'avg_latency' in 'struct rq'
```

`MY_ivh_atc.bpf.c` is **byte-identical at G-LOCK-30 and HEAD** (`git diff` is
empty for `tools/bpf/`), uses **no CO-RE** (0 hits for `BPF_CORE_READ` /
`__builtin_preserve_access_index`), and reads `rq->avg_latency`, which exists in
neither G-LOCK-30's nor G-LOCK-39's `struct rq`. So the committed selector could
never have compiled against either kernel: `tools/bpf` was updated in place
without per-commit capture.

Consequence: running the stale binary on G-LOCK-30 attaches cleanly (btf_id
66630 resolves) but computes garbage offsets. Traced:

```
@entered:          341,254    migration path entered
@selector_called:  105,121    BPF selector invoked
@reached_commit:         0    never passed fair.c:13972 (target -1 or same CPU)
```

Zero migrations, and the arm reads -1.33% vs PV. **Any pre-G-LOCK-40 kernel
measured with the current selector binary is invalid**, which is why this was
settled from recorded history plus a direct RCU probe rather than a reboot.

## 6. Consequences

1. **Every result recorded before 2026-09-25 needs re-measurement or an explicit
   caveat**, including the campaign's 19 confirmed wins. They were produced by a
   build that blocked inside RCU readers.
2. The current numbers are the defensible ones.
3. The gap is not a performance regression to fix. It is the cost of correctness,
   and it is worth stating as such -- a reviewer who spots the RCU issue in an
   uncorrected build would discard the whole evaluation.
4. `ivh_pre_lock()` should get a counter for the RCU bail, so the loss is
   measurable in-kernel instead of via a kprobe on an inlined function's callee.
