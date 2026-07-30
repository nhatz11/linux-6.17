# Final build plan: removing steal time (and vcap) from IVH's decision path

Date: 2026-07-29. Branch `kernel-43-clean`, repo
`/home/nick/kernels/linux-6.17-rseqport`, running kernel
`6.17.0-rseqport66-TSCfix+`. **Planning only — no source edited, nothing
built, nothing booted in this pass.**

Predecessors, in order:

- `ivh_tsc_replacement_consumers_2_3_design_2026-07-25.md` (feasibility)
- `ivh_tsc_heartbeat_refcycles_build_plans_2026-07-26.md` (Plans 1 and 2 —
  both now **shipped and validated**, this is the current baseline)
- `ivh_last_preemption_tsc_alt_design_2026-07-27.md` (the Part C seed idea;
  §0.3 below records where this plan **supersedes** it)

This document is organised around **kernel builds**, not around the abstract
Part A / Part B / Part C split used in planning, because the scarce resource
in this workflow is the full `bzImage` build + reboot cycle, not the design
work. §2 states the boot count and justifies each one. Every claim carried
over from planning was re-opened against the tree during this pass; §0.2 and
§0.3 list the places the planning record turns out to be **wrong**, and each
correction is applied in the plan body rather than only noted here.

---

## Summary, up front

**Mandatory kernel builds: 2. Expected: 3. Worst case: 4.**

| Build | Contains | Why it cannot be folded into an earlier build |
| --- | --- | --- |
| **1 — measure and arm** | Every new signal, both in shadow form and in authoritative form, all behind default-`0` sysctls. All comparators. | It is first. Nothing in it changes behaviour at default, so there is nothing to isolate. |
| **2 — corrections** | Whatever Boot 1's data proves wrong: CS-stamp write cadence, capacity correction terms, S1 reset details, holder-table sizing if it must become static. | It is **defined by** Boot 1's output. Its content is unknowable before Boot 1 runs. This is the one genuinely un-bundleable build. |
| **3 — decommission** | Delete vcap inputs, `rq->max_latency`, `rq->avg_latency`, the shadow scaffolding, the comparators. | Deleting the comparators before the replacement is authoritative *and stable* destroys the only evidence base. Foldable into Build 2 **only if** Boot 1 finds nothing needing correction. |
| **2A — Option A (conditional)** | `u32 holder_cpu` inside `struct qspinlock` behind `CONFIG_IVH_LOCK_HOLDER`. | A struct-size change cannot be sysctl-gated. Needed **only if** Boot 1 shows Option B's side table is unusable at every table size — which Boot 1 measures directly, by design. |

Everything else — every authoritative flip, every threshold sweep, every
table-size sweep, the whole `is_cs_preempted()` A/B, the whole Part C A/B —
happens by `sysctl` inside Boot 1, with no rebuild and no reboot.

The discipline being preserved is **not** "one change per build." It is:
*only one signal flips from shadow to authoritative at a time, and never
before its own shadow data has been read.* That is enforced here by giving
every new signal its own three-valued source knob (`0` off / `1` shadow /
`2` authoritative), copying the existing `ivh_pv_preempt_src` shape
(`arch/x86/include/asm/ivh_tsc_beat.h:56-75`). A sysctl flip is strictly
safer than a boot-gated flip because it is reversible in place.

---

## 0. Re-verification log

### 0.1 Confirmed against source

| Claim | Verified at |
| --- | --- |
| Per-CPU TSC heartbeat, one `u64 stamp`, own cacheline | `arch/x86/include/asm/ivh_tsc_beat.h:50-54` |
| `ivh_beat_age()` signed, one `rdtsc()`; `ivh_beat_stale()` is `age > threshold` | `ivh_tsc_beat.h:144-154` |
| Shadow agreement matrix + split log2 age histograms, the pattern to duplicate | `ivh_tsc_beat.h:103-111`, `qspinlock_paravirt.h:295-360` |
| `ivh_prev_is_preempted()` called only from `pv_wait_early()` | `qspinlock_paravirt.h:454` |
| `ivh_pv_preempt_src` 0/1/2 with a validating proc handler | `arch/x86/kernel/kvm.c:1238`, `:1345-1391`, table at `:1449` |
| Threshold calibrated at `late_initcall` from `tsc_khz` to 1500 µs | `kvm.c:1249-1277`, `IVH_BEAT_THRESHOLD_US` at `:1250` |
| `ivh_beat_publish_in_spin()` gate posture: `src` check first, one predicted branch at default | `qspinlock_paravirt.h:385-394` |
| Beat publish sites: tick, `pv_init_node`, both spin loops, three halt exits | `cputime.c:530`; `qspinlock_paravirt.h:490`, `:525`, `:755`; `kvm.c:1601`, `:1609`, `:1784` |
| REF_TSC inference + halt correction shipped, both knobs default 0 | `core.c:218`, `:245-254`; `struct ivh_lock_halt` at `ivh_tsc_beat.h:227-235` |
| `rq->last_preemption` / `last_active_time` / `preemptions` / `max_latency` written **only** from real `paravirt_steal_clock()`, unconditional on `ivh_steal_source` | `cputime.c:256-286` |
| `get_steal_and_preemptions()` switches the steal *magnitude* but returns `rq->preemptions` unswitched | `core.c:261-283` |
| Gate 2 default is the `last_active_time` formula | `bpf_sched.c:92` (`ivh_time_left_source = 1UL`), consumed at `fair.c:13243-13252` |
| Reverted `ivh_account_preemption_event()` / `ivh_prev_ref_steal_ns` fix: **no trace in the tree** | `grep -rn` over all `*.c`/`*.h` returns nothing |
| `cs_enter()` fires only at `lock_depth == 1` | `spinlock.c:352` |
| `cs_exit()` gated on `lock_depth == 0 && cs_start_ts` | `spinlock.c:369` |
| Both gated on `ivh_universal_eligible && !ivh_exclude` (or `ivh_observe`) | `spinlock.c:361-363`, `:393-399` |
| Exactly ten `cs_enter`/`cs_exit` wrapper call sites, all `!in_interrupt()`-gated, all with `lock` in scope | `spinlock.c:563-692` |
| **`_raw_spin_unlock_bh()` runs `cs_exit()` AFTER the real unlock**, with an in-tree comment saying so | `spinlock.c:684-689`, comment at `:685` |
| PV path skips the pending-bit section entirely at runtime | `qspinlock.c:216-217` (`if (pv_enabled()) goto pv_queue;`) |
| Queue head spins on the lock byte in `pv_wait_head_or_lock()`, `clear_pending()` immediately after the spin | `qspinlock_paravirt.h:713`, spin at `:745-757`, `clear_pending` at `:758` |
| Pending bit is the anti-steal flag; stealing only happens when it is clear | `qspinlock_paravirt.h:65-88`, `:100` |
| `USE_CMPXCHG_LOCKREF` requires `SPINLOCK_SIZE <= 4` | `include/linux/lockref.h:21-23`; `SPINLOCK_SIZE` defined `kernel/bounds.c:24` |
| `account_idle_ticks()` does **not** call `account_process_tick()` | `cputime.c:577-594` |
| `rq->last_idle_tp = sched_clock()` in `account_idle_time()` | `cputime.c:233` |
| `ivh_ref_accumulate()` called from the tick, right before the vtime early return | `cputime.c:547`; function at `core.c:590` |
| `rq->max_latency` read only by `get_max_latency()` → `/proc/vcap_info`; no in-kernel decision reader | write `cputime.c:278-279`; `core.c:337-340`; `custom_modules/vsched_module.c:336-340` |
| `rq->avg_latency` has **no in-kernel reader at all**, only `set_avg_latency()` | `core.c:343-346`, field at `sched.h:1421` |
| vcap forces `capacity_perc = 0.5` for banned CPUs, and that number does reach the kernel | `/home/nick/vsched_main/vcapacity/main.cpp:377-379`, written out at `:494` → `set_custom_capacity()` |
| `cfs_latency_select` is declared but **never called from the kernel** | declared `include/linux/sched_hook_defs.h:11`; zero call sites in `kernel/` |
| `cfs_select_run_cpu_spin` has exactly one call site | `fair.c:13403` |
| Loader attaches both `test3` and `test32` | `tools/bpf/MY_ivh_atc.c:261-293`, `:315` |
| Live gate config: `LOCKHOLDER=1 SPINNER=0 CAPACITY_LOW=1 NOT_BETTER=1 PREEMPTED=1 BURST_ORDER=0 BURST_BUDGET=0`, `IVH_CAP_FLOOR 850` | `MY_ivh_atc.bpf.c:274-289` |
| `average_capacity` is stored into `process_cpu()`'s ctx and **never read there** | declared `:300`, assigned `:660`; body reads only `ctx->source_capacity` (`:434`) and `IVH_CAP_FLOOR` (`:424`) |
| `average_capacity` and `rq->avg_latency` are used only inside `search_latency()`, behind `test32` | `:749`, `:765-768`; `SEC("sched/cfs_latency_select")` at `:777` |
| `NR_CPUS=8192 < 16K`, so `_Q_PENDING_BITS == 8` and the byte-wise pending helpers are the live ones | `.config`; `qspinlock_types.h:74-78` |
| `CONFIG_PARAVIRT_SPINLOCKS=y`, `CONFIG_HZ=1000`, `CONFIG_LOCK_EVENT_COUNTS` **not** set, no `CONFIG_INLINE_SPIN_*` set | `.config` |

### 0.2 Corrections to the planning record — read these before building

**0.2.1 Gate 1 reads `rq->cpu_capacity`, not `rq->cpu_capacity_custom`, and
the two are only synchronised at load-balance cadence.**

`ivh_steal_imminent()` compares `rq->cpu_capacity > ivh_capacity_threshold`
(`fair.c:13218`; same at `fair.c:13276` in the duplicated
`ivh_rq_capacity_and_timeleft_ok()`). The BPF gates likewise read
`select_rq->cpu_capacity` (`MY_ivh_atc.bpf.c:424`, `:434`).
`rq->cpu_capacity_custom` — the field vcap actually writes via
`set_custom_capacity()` (`core.c:357-360`) — reaches `rq->cpu_capacity` at
exactly one place:

```c
/* fair.c:10082-10086 */
rq->cpu_capacity = capacity;
if (rq->cpu_capacity_custom > 0) {
        rq->cpu_capacity = rq->cpu_capacity_custom;
        capacity = rq->cpu_capacity_custom;
}
```

`update_cpu_capacity()` is reached only from `update_group_capacity()`
(`fair.c:10106`), which is called from load balancing
(`fair.c:11122`, guarded by `time_after_eq(jiffies, sg->sgc->next_update)`)
and from `topology.c:1320`. `next_update` is set to
`jiffies + sd->balance_interval` (`fair.c:10101-10103`).

**Consequence for the plan:** a tick-rate capacity replacement that writes
`cpu_capacity_custom` would still only become visible to Gate 1 at
load-balance interval, throwing away its own resolution. Writing
`rq->cpu_capacity` directly would be overwritten on the next balance pass.
So the replacement gets its **own dedicated `struct rq` field**
(`rq->ivh_vact_capacity`), and Gate 1 plus the two BPF gates select between
`rq->cpu_capacity` and `rq->ivh_vact_capacity` via a sysctl. This also has
the pleasant property that the vcap path stays completely untouched and
therefore still usable as the comparator baseline.

**0.2.2 The memory-ordering rule R1 is stated on a false premise.** The
planning record asserts "all acquire sites are already `_acquire` variants,
so a plain `WRITE_ONCE` placed after needs no extra barrier — x86-TSO can't
hoist a store above a prior locked RMW." Three live ownership-transfer sites
are not RMWs at all:

| Site | Actual primitive |
| --- | --- |
| `set_locked()` — contended queue-head acquisition | `WRITE_ONCE(lock->locked, _Q_LOCKED_VAL)` (`kernel/locking/qspinlock.h:196-199`) |
| `clear_pending_set_locked()` — pending-bit acquisition, `_Q_PENDING_BITS==8` variant, the live one here | `WRITE_ONCE(lock->locked_pending, _Q_LOCKED_VAL)` (`qspinlock.h:99-102`) |
| queue-head uncontended acquisition | `atomic_try_cmpxchg_**relaxed**()` (`qspinlock.c:468`) |

The **conclusion** survives, but the reason must be restated correctly:
x86-TSO does not reorder store→store, so a `WRITE_ONCE(holder, cpu)` placed
after either plain store cannot become globally visible before it. Do not
write "the locked RMW fences it" in a code comment — it is false at two of
the sites and would mislead the next reader. R2, R3, R4 hold as stated; see
§3.3.4 for the one Option-B-specific addition to R4.

**0.2.3 `ivh_obs_cs_hist` is not a population sample of kernel CS hold
times.** It is incremented only under `if (current->ivh_observe)`
(`spinlock.c:413`, histogram at `:443-446`), i.e. only for tasks launched
under `ivh_exec -v`. Calibrating `is_cs_preempted()`'s threshold from its
p99.9 therefore calibrates against the *benchmark's* CS distribution, not
the kernel's. Build 1 fixes this with a second, `ivh_cs_preempt_src`-gated
histogram fed from every `cs_exit()` regardless of `ivh_observe` (§3.2.3),
and ships the threshold as a live sysctl so it can be swept without a
rebuild.

**0.2.4 `is_cs_preempted()` as specified does not measure preemption.**
This is the largest correction in this document and it changes what Build 1
must contain. See §1.2 — it is important enough to have its own section.

**0.2.5 "The pending-bit path doesn't exist under
`CONFIG_PARAVIRT_SPINLOCKS=y`" is imprecise.** It is compiled — `qspinlock.c`
is included twice, once natively and once as
`__pv_queued_spin_lock_slowpath` (`qspinlock.c:507-522`) — and it is skipped
at *runtime* by `if (pv_enabled()) goto pv_queue;` (`qspinlock.c:216-217`).
It becomes live again on a `nopvspin` boot. This matters for the holder
table: the pending-bit acquisition at `qspinlock.c:300` is a real
ownership-transfer site that must be stamped, or a `nopvspin` comparison run
will silently report every lock as "unknown holder". Since one edit to
`qspinlock.c` is compiled into both variants, covering it costs nothing.

**0.2.6 The 2026-07-26 doc's §0.3.0 note that the loaded `vsched_module`
lacks `/proc/vcap_preempted` is now stale.** The live guest has
`/proc/vcap_info`, `/proc/vcap_preempted`, `/proc/vcap_steal_compare`,
`/proc/vact_write`, `/proc/vcapacity_write` and `/proc/ivh_debug`. The
module has been rebuilt since. The two copies of `vsched_module.c`
(`linux-6.17/custom_modules/` vs `linux-6.17-rseqport/custom_modules/`)
still differ; before touching the module, diff them and decide which is
authoritative, as §0.3.0 of that document instructs.

### 0.3 Where this plan supersedes `ivh_last_preemption_tsc_alt_design_2026-07-27.md`

That note proposes detecting preemption events by checking the age of
`struct ivh_tsc_beat`'s stamp at its own tick publish site. That cannot work
as written, because `ivh_tsc_beat.stamp` is **not** tick-only: it is also
written from `pv_init_node()` (`qspinlock_paravirt.h:490`), from both
qspinlock spin loops (`:525`, `:755`), and from three halt-exit sites
(`kvm.c:1601`, `:1609`, `:1784`). A stamp refreshed from the spin loops has
no gap to detect — the very coverage that makes it a good *predecessor
liveness* signal destroys it as a *preemption event* detector.

The note's other two claims survive and are load-bearing here:

- The tick keeps running through a lock-path `safe_halt()` (mechanism 0/2
  halt with `IF=1`, so the LAPIC timer still fires; this is exactly why
  `ivh_lock_halt_flush()` exists on the REF_TSC side, `ivh_tsc_beat.h:287`).
  So a tick-published stamp needs **no** correction analogous to
  `ivh_ref_halt_correct`. This is the structural advantage of Part C over
  reusing `rq->ivh_ref_steal_ns`, and it is why this plan drops the reverted
  `ivh_account_preemption_event()` approach permanently rather than
  revisiting it.
- nohz idle is the one real gap, and `rq->last_idle_tp` already exists to
  explain it (`cputime.c:233`).

Hence Part C uses a **new, dedicated, tick-only** `rq->ivh_vact_stamp`
rather than the shared heartbeat. Its only sources of gaps are genuine host
preemption and genuine guest idle, which is precisely the property the
algorithm needs.

---

## 1. The two things that decide whether this works

### 1.1 The central open empirical question, stated plainly

**Whether a gated store on the qspinlock ownership-transfer path is
affordable, and what the real "unknown holder" and collision rates are under
this project's workloads, cannot be settled by any amount of further
analysis. It requires building the spike and measuring it.**

That is not a hedge, it is the design constraint that shapes Build 1. There
are two sub-questions and they have different shapes:

1. **Cost.** A `WRITE_ONCE` into a side table at every lock acquire and
   release, on a path where the *uncontended* case is currently a single
   `LOCK CMPXCHG` and a `MOV` (`asm-generic/qspinlock.h:111`, `:128`). This
   is answerable inside one boot by A/B'ing `ivh_lock_holder_enabled`
   between 0 and 1 on the same workload — the sysctl-off case is one
   predicted branch on a read-mostly global, the same posture
   `ivh_beat_publish_in_spin()` already has (`qspinlock_paravirt.h:387`).
2. **Accuracy.** How often does the queue head ask "who holds this lock?"
   and get "unknown"? Split into the two causes: the genuine handoff window
   (`ivh_holder_unknown_empty`) and side-table tag mismatch
   (`ivh_holder_unknown_collision`). Answerable inside one boot **only if
   the table size is sweepable at runtime**, which is why §3.3.3 allocates
   the table at maximum size and makes the effective index mask a sysctl.

Build 1 exists to answer both in a single boot. Every counter listed in
§3.7 is there because omitting it would force a "we need one more counter"
rebuild, and that is the specific failure this plan is designed to avoid.

### 1.2 `is_cs_preempted()` as specified measures CS *duration*, not preemption

The specification is: stamp a per-CPU `u64` at `cs_enter()`, clear it at
`cs_exit()`, and read `age > threshold ⇒ the holder is preempted`.

The stamp is written **once**, at the start of the hold, and is never
refreshed while the hold is in progress. So its age is *how long this
critical section has been running* — nothing more. A holder that is
executing perfectly happily but has a genuinely long critical section
crosses the threshold and reads as preempted. The predicate's actual
semantic content is:

> "This hold has been running longer than 99.9% of holds do, so *something*
> is wrong; host preemption is the most likely something."

That is a defensible heuristic, and it explains why the threshold has to
come from the observed CS-hold distribution rather than from the 1500 µs
value used for the wait stamp — the two numbers are answering different
questions. But it is emphatically **not** the same predicate as
`is_wait_preempted()`, whose stamp is refreshed at ≥1 kHz from the tick plus
per-spin-iteration from the spin loops, and whose age therefore really does
mean "time since this vCPU last proved it was executing guest code."

The consequence is that the plan must not assume the CS-stamp form works.
It must ship **two predicate forms** in Build 1 and let the data choose:

| `ivh_cs_predicate_form` | Definition | What it is really testing |
| --- | --- | --- |
| `0` (as specified) | `cs_stamp != 0 && (rdtsc() - cs_stamp) > ivh_cs_beat_threshold` | Is this hold in the long tail of hold durations? |
| `1` (recommended) | `cs_stamp != 0 && ivh_beat_stale(holder_cpu)` | Is the holder inside a CS **and** has its continuously-refreshed liveness heartbeat gone stale? |

Form 1 is strictly better founded: it uses the CS stamp only for the part it
can actually answer ("is this CPU inside a critical section right now") and
delegates "is it running" to the already-validated staleness predicate. The
tick is a hardirq and fires regardless of `preempt_disable()`, so
`ivh_tsc_beat.stamp` stays fresh on a running CPU even while it holds a
spinlock — form 1's second term is well-defined at exactly the moment it is
needed. Form 1 also composes the new, unproven piece (holder identity) with
a proven piece (staleness), which is the lower-risk composition.

Form 0 is kept because it is what was specified, because it is free to
compute once form 1's plumbing exists, and because the two forms'
disagreement rate is itself a finding: a large "form 0 fires, form 1 does
not" population is a direct measurement of how much of the CS-duration tail
is long-but-healthy rather than preempted.

The sign convention (`age > threshold`, matching `ivh_beat_stale()`,
`ivh_tsc_beat.h:151-154`) is asserted, not assumed: the split histograms in
§3.7 confirm it empirically. If `ivh_cs_age_hist_preempted[]` does **not**
sit above `ivh_cs_age_hist_running[]`, the convention is backwards and the
histograms say so directly, in one boot, without a rebuild.

---

## 2. The boot budget

### Boot 1 — "measure and arm"

**Why it is necessary:** it is first, and nothing precedes it.

**Why everything is bundled into it:** every addition is behaviourally inert
at its default. The costly mistake this project already made — the bimodal
phantom-steal bug — was making two *not-yet-individually-validated signals
authoritative at the same time (`ivh_pv_preempt_src=2` together with
`ivh_steal_source=1`, root-caused at `ivh_tsc_beat.h:156-225`). That is a
statement about *authority*, not about *compilation*. Code that computes and
logs but does not act cannot produce that failure mode, no matter how much
of it lands in one build. What must stay serialised is the flip to
authoritative, and §3.10 serialises it — by sysctl, inside this one boot.

**Second boot avoided by bundling:** every authoritative path is compiled in
here behind its own default-`0` sysctl. Without that, each of the four
authoritative flips (`ivh_cs_preempt_src=2`, `ivh_cap_source=2`,
`ivh_preempt_event_source=2`, and the head-bail side effect) would need its
own build. Bundling turns four builds into four `echo`s.

### Boot 2 — "corrections"

**Why it cannot be folded into Boot 1:** its content is *defined by Boot 1's
output*. The realistic candidates, in descending likelihood:

- The CS stamp's write cadence is wrong and needs an additional publish site
  (§4.1). This is new code at a new location; it cannot be pre-written
  because the location depends on where the histograms show the failure.
- Part C's capacity number diverges from vcap's in a structured way and
  needs a correction term (§4.2), exactly as REF_TSC needed
  `ivh_ref_halt_correct`. The shape of the term depends on the shape of the
  divergence.
- The post-idle S1 reset rule needs adjustment (§4.3).

If Boot 1's data shows none of these, Boot 2 and Boot 3 merge and the total
is 2.

### Boot 3 — "decommission"

**Why it cannot be folded earlier:** it deletes the vcap inputs, the dead
fields, and the comparators. The comparators are the evidence base for
whether the replacement is correct; removing them while the replacement is
still being trusted for the first time removes the ability to notice it
going wrong. Decommissioning is also the only irreversible step in the
plan — it is the one place where "we were wrong" costs a rebuild rather than
an `echo 0`.

### Boot 2A — Option A, conditional

**Why it needs its own build:** `u32 holder_cpu` inside `struct qspinlock`
changes `sizeof(spinlock_t)` from 4 to 8, which flips `USE_CMPXCHG_LOCKREF`
off (`lockref.h:21-23`) and changes dentry refcounting behaviour elsewhere
in the kernel. A struct-size change cannot be sysctl-gated, so it needs
`CONFIG_IVH_LOCK_HOLDER` and therefore its own build, and any A/B against it
needs two kernels.

**Why it is conditional:** Option A's only advantage over Option B is zero
collisions and zero unknown-by-collision. Boot 1 measures Option B's
collision rate across the whole table-size range directly. If it is
negligible at an affordable table size, Option A is never built. Blast
radius checked: `SPINLOCK_SIZE` is consumed in exactly two places outside
`bounds.c` — `lockref.h:23` (flips) and `mm_types_task.h:20`
(`ALLOC_SPLIT_PTLOCKS`, `SPINLOCK_SIZE > BITS_PER_LONG/8`, i.e. `> 8`, so
`4 → 8` does **not** flip it). `lockref` is the entire cost.

### What is free

Not a boot, and not even a build, for the record:

- `tools/bpf/MY_ivh_atc.bpf.c` — recompile and reload the BPF program.
- `custom_modules/vsched_module.c` — `rmmod`/`insmod`. Anything the module
  can compute from already-exported accessors is free; a **new** accessor
  is a kernel change and therefore is not. Build 1 must therefore export
  the accessors the comparators need, once, up front.
- `/home/nick/vsched_main/vcapacity/main.cpp` — userspace rebuild.

---

## 3. Build 1 — measure and arm

### 3.1 Files touched

| File | Change |
| --- | --- |
| `arch/x86/include/asm/ivh_tsc_beat.h` | `struct ivh_cs_beat`; `ivh_cs_beat_publish()`/`_clear()`/`ivh_cs_age()`; `is_cs_preempted()` both forms; holder-table types and API; all new counter declarations |
| `arch/x86/kernel/kvm.c` | definitions of the above; holder table allocation at `late_initcall`; `ivh_cs_beat_threshold` calibration; new entries in `ivh_pv_sysctls[]` (`:1449`ff) with validating handlers |
| `kernel/locking/qspinlock_paravirt.h` | rename `ivh_prev_is_preempted()` → `is_wait_preempted()`; new `ivh_cs_head_check()`; holder stamp/clear at the PV transfer sites |
| `kernel/locking/qspinlock.c` | holder stamp at the native/shared transfer sites (one edit, both compiled variants) |
| `include/asm-generic/qspinlock.h` | holder stamp in `queued_spin_lock()`/`queued_spin_trylock()`, clear in `queued_spin_unlock()` |
| `arch/x86/include/asm/qspinlock.h` | clear in `native_queued_spin_unlock()`; stamp in `virt_spin_lock()` |
| `kernel/locking/spinlock.c` | `cs_enter(lock)`/`cs_exit(lock)` gain the lock pointer; publish/clear `ivh_cs_beat`; unconditional CS-hold histogram |
| `kernel/sched/sched.h` | `rq->ivh_vact_*` block, immediately after the `ivh_ref_*` block ending at `:1458` |
| `kernel/sched/cputime.c` | tick-only stamp publish + jump detection in `account_process_tick()` after `:547`; `ivh_vact_idle_exit_tsc` beside `:233`; shadow preempt-event fields |
| `kernel/sched/fair.c` | `ivh_steal_imminent()` capacity/preempt-event source selection; the dual-evaluation comparator; `/proc/ivh_debug` additions (printer at `:13576`) |
| `kernel/sched/core.c` | new sysctl variables; new exported accessors for the module |
| `kernel/sched/bpf_sched.c` | new entries in the sched sysctl table (`:276`ff) |
| `custom_modules/vsched_module.c` | new `/proc/ivh_vact_compare` (module rebuild, no reboot) |
| `tools/bpf/MY_ivh_atc.bpf.c` | read `rq->ivh_vact_capacity` when the source sysctl selects it (BPF reload, no reboot) |

### 3.2 The CS stamp

#### 3.2.1 Structure

Next to `struct ivh_tsc_beat` in `ivh_tsc_beat.h`, own cacheline, same
one-writer/many-remote-readers shape and for the same reason documented at
`ivh_tsc_beat.h:39-48`:

```c
struct ivh_cs_beat {
        u64 stamp;      /* raw rdtsc() at cs_enter(); 0 == not in a CS */
} ____cacheline_aligned_in_smp;
```

`0` is the "not in a critical section" sentinel, which is why `cs_exit()`
must clear rather than merely leave a stale value.

#### 3.2.2 Write sites

Inside the existing `current->lock_depth == 1` block of `cs_enter()`
(`spinlock.c:352-364`), and in the `lock_depth == 0` block of `cs_exit()`
(`:369`). Gated on `ivh_cs_preempt_src != 0` so the default build path is
unchanged.

The four structural limitations of these sites are **accepted for the stamp**
and are the reason holder *identity* cannot live here (§3.3.1): outermost
holds only, non-interrupt context only, IVH-eligible tasks only, and the
`_raw_spin_unlock_bh()` late clear. For the stamp specifically, three of
those four are merely coverage gaps (an unstamped holder reads `stamp == 0`
⇒ "unknown, don't act", which is the safe direction). The fourth,
`_raw_spin_unlock_bh()`, is a *live wrong answer* rather than a gap, and it
is the one that must be handled: at `spinlock.c:684-689` the unlock happens
first, so between the release and the `cs_exit()` the CPU may already have
entered a *different* critical section, and clearing the stamp there wipes
the new hold's stamp. Mitigation, and it is cheap: `cs_exit()` clears the
stamp **only if the recorded lock pointer still matches**, which is exactly
why `cs_exit()` gains the lock argument. Add
`current->cs_beat_lock` (a `void *`, set in `cs_enter()`) and clear only on
match. Count the mismatch (`ivh_cs_clear_mismatch`) — that counter is the
direct measurement of how often the `unlock_bh` ordering actually bites.

#### 3.2.3 Threshold, and how it is calibrated

`ivh_cs_beat_threshold`, in raw TSC cycles, initialised at `late_initcall`
from `tsc_khz` exactly as `ivh_pv_beat_threshold` is (`kvm.c:1267-1277`), and
sysctl-writable so it can be swept live.

The initial value comes from the p99.9 of observed CS hold times converted
ns → cycles. Because `ivh_obs_cs_hist` only covers `ivh_observe` tasks
(§0.2.3), Build 1 adds a second histogram, `ivh_cs_hold_hist[32]`, fed from
every `cs_exit()` that reaches the `lock_depth == 0` block, gated only on
`ivh_cs_preempt_src != 0` — same log2 bucketing, same clamping at both ends,
same "percentiles are computed in userspace off a before/after delta"
contract as `ivh_obs_cs_hist` (`spinlock.c:422-446`). The initial threshold
is a *starting point for a sweep*, not a committed value; the authoritative
calibration comes from the split age histograms in §3.7 and the sweep in
§3.9.

#### 3.2.4 The rename

`ivh_prev_is_preempted()` → `is_wait_preempted()`, its one call site at
`qspinlock_paravirt.h:454`, and the two mentions in its own comment block.
Pure rename, zero behaviour change, and it exists so that the two predicates
read as siblings at their call sites. Do it in Build 1 and never again —
renaming later would make every diff between boots harder to read.

### 3.3 Holder identity

#### 3.3.1 Why not `cs_enter()`/`cs_exit()`, re-verified

All four disqualifiers hold against current source:

1. **Nested holders invisible.** `cs_enter()` runs its body only at
   `current->lock_depth == 1` (`spinlock.c:352`). The holder of an inner
   lock — which is exactly the holder a waiter on that inner lock needs to
   identify — is never recorded.
2. **Interrupt-context holders invisible.** All ten wrapper sites are gated
   on `!in_interrupt()` (`spinlock.c:566`, `:578`, `:594`, `:609`, `:623`,
   `:637`, `:648`, `:660`, `:672`, `:686`). A lock held from hardirq or
   softirq context has no holder record.
3. **`_raw_spin_unlock_bh()` clears after the release.** `spinlock.c:684-689`,
   with the in-tree comment at `:685` acknowledging it. A holder-clear here
   can wipe a *new* holder's identity after a fast handoff — a live wrong
   answer, not a stale one. This alone is disqualifying for identity, where
   §3.2.2's pointer-match trick is only enough for the stamp.
4. **IVH eligibility gating.** `spinlock.c:361-363` and `:393-399` gate on
   `ivh_universal_eligible && !current->ivh_exclude`. The holder that needs
   identifying is very often ineligible code — a kernel thread, an excluded
   process, anything not IVH-managed.

A fifth, found this pass: these wrappers are only reached through
`raw_spin_lock()` and friends. Direct users of `arch_spin_lock()` /
`queued_spin_lock()` — including the scheduler internals and the qspinlock
machinery itself — bypass them entirely. Conversely, a stamp at the
qspinlock layer catches **every** qspinlock acquisition in the kernel. That
is the positive argument for the layer choice, not merely the absence of
objections to it.

#### 3.3.2 The API

```c
void ivh_lock_set_holder(struct qspinlock *lock);   /* stamp: this CPU now owns @lock */
void ivh_lock_clear_holder(struct qspinlock *lock); /* clear, before the releasing store */
int  ivh_lock_holder_cpu(struct qspinlock *lock);   /* >= 0, or -1 for "unknown" */
```

Both writers early-return on `!READ_ONCE(ivh_lock_holder_enabled)`, the
read-mostly-global-first posture of `ivh_beat_publish_in_spin()`
(`qspinlock_paravirt.h:387`), so the sysctl-off cost is one predicted branch.

#### 3.3.3 Option B — the side table, and how it answers its own sizing question in one boot

```c
struct ivh_holder_slot {
        void *tag;              /* the qspinlock pointer this slot describes */
        u32   holder_cpu;       /* CPU + 1; 0 == empty */
} ____cacheline_aligned_in_smp;
```

Direct-mapped, indexed by `hash_ptr(lock, bits)`, lossy by design: on tag
mismatch, report unknown. No collision resolution, no locking, no `BUG()`.

The table is allocated **once at maximum size** at `late_initcall`
(`IVH_HOLDER_MAX_BITS`, suggest 16 ⇒ 65536 slots × 64 B = 4 MB, trivially
affordable on this research guest), and the *effective* index width is the
sysctl `ivh_holder_bits`. That single decision is what makes the collision
question answerable in one boot: sweep `ivh_holder_bits` from small to
`IVH_HOLDER_MAX_BITS` under a fixed workload, read
`ivh_holder_unknown_collision / ivh_holder_stamps` at each setting, and the
whole size-vs-accuracy curve falls out of one kernel. Without this, each
point on that curve is a rebuild.

Because both writers are sysctl-gated, one kernel also A/B's Option B
against a true baseline (`ivh_lock_holder_enabled=0`), which is what
answers the cost half of §1.1.

#### 3.3.4 Write sites, re-enumerated against current source

Ownership-transfer points only. `set_pending()`, `clear_pending()`,
`xchg_tail()` and `queued_fetch_set_pending_acquire()` are **not** transfer
points and are not stamped.

**Acquire — stamp immediately after:**

| # | Site | Primitive | Notes |
| --- | --- | --- | --- |
| A1 | `queued_spin_lock()` uncontended fastpath | `atomic_try_cmpxchg_acquire` | `asm-generic/qspinlock.h:111`. The hottest site in the kernel; this is the cost that matters. |
| A2 | `queued_spin_trylock()` | `atomic_try_cmpxchg_acquire` | `asm-generic/qspinlock.h:97`. Note that inside `qspinlock.c` this name is `#define`d to `pv_hybrid_queued_unfair_trylock` (`qspinlock_paravirt.h:89`), so A2 and A6 are distinct sites, not one. |
| A3 | pending-bit acquisition | `clear_pending_set_locked()` | `qspinlock.c:300`. Runtime-dead under PV (`:216-217`), live under `nopvspin`. Stamp it — §0.2.5. |
| A4 | queue head, uncontended | `atomic_try_cmpxchg_relaxed` | `qspinlock.c:468`. **Relaxed**, not acquire — see §0.2.2. |
| A5 | queue head, contended | `set_locked()` = plain `WRITE_ONCE` | `qspinlock.c:477`, helper at `qspinlock.h:196-199`. **Not an RMW** — §0.2.2. |
| A6 | PV lock stealing | `try_cmpxchg_acquire(&lock->locked, ...)` | `qspinlock_paravirt.h:101` |
| A7 | PV queue head acquires from the spin | `trylock_clear_pending()` | `qspinlock_paravirt.h:129-135` (`_Q_PENDING_BITS==8`, the live variant) / `:142-157`. Reached at `:746` → `goto gotlock`. |
| A8 | PV queue head finds the lock free while hashing | `xchg(&lock->locked, _Q_SLOW_VAL) == 0` | `qspinlock_paravirt.h:775-784` → `goto gotlock` |
| A9 | test-and-set virt fallback | `atomic_try_cmpxchg` | `arch/x86/include/asm/qspinlock.h:160`. Dead on this host (`virt_spin_lock_key` is disabled by `kvm_spinlock_init()` when PV spinlocks are active) but stamp it for completeness. |

The no-node fallback (`qspinlock.c:347`) and the post-init retry
(`qspinlock.c:375`) both acquire *through* A2/A6 and need no separate site.

**Release — clear immediately before:**

| # | Site | Primitive |
| --- | --- | --- |
| R1 | `queued_spin_unlock()` generic | `smp_store_release(&lock->locked, 0)` — `asm-generic/qspinlock.h:128` |
| R2 | `native_queued_spin_unlock()` | `smp_store_release(&lock->locked, 0)` — `arch/x86/include/asm/qspinlock.h:46` |
| R3 | `__pv_queued_spin_unlock()` fastpath | `try_cmpxchg_release(&lock->locked, &locked, 0)` — `qspinlock_paravirt.h:875` |
| R4 | `__pv_queued_spin_unlock_slowpath()` | `smp_store_release(&lock->locked, 0)` — `qspinlock_paravirt.h:852` |

R3 is a *conditional* release: on failure it falls through to R4. Clear
before the `try_cmpxchg_release` and let R4's clear be idempotent (clearing
an already-cleared slot is a no-op), rather than trying to clear only on
success — the latter would leave a window where the lock is released and the
holder is still published.

Because `qspinlock.c` is compiled twice (`:507-522`), one edit per site there
covers both the native and the `__pv_` variant. State this in a comment; a
future reader will otherwise "fix" the apparent omission.

#### 3.3.5 Memory ordering

- **Stamp strictly after the acquiring operation.** Stamping *before* would
  publish a holder for a lock not yet acquired, which is the dangerous
  direction: a reader would attribute a critical section to a CPU that never
  entered one. On x86, a plain `WRITE_ONCE` placed after any of A1–A9 needs
  no additional barrier — but the reason is **store→store ordering under
  x86-TSO**, not "the locked RMW fences it," because A4 is `relaxed` and A3
  and A5 are not RMWs at all (§0.2.2).
- **Clear strictly before the releasing store.** All four release sites are
  `smp_store_release` or `try_cmpxchg_release`, both of which order all
  prior stores ahead of the release, so a plain `WRITE_ONCE(slot, 0)`
  immediately above needs no extra barrier — but it must not be moved past
  the release, which the release semantics already guarantee.
- **The reader is two independent loads and can skew.** `holder_cpu` and the
  lock byte (or the liveness heartbeat) are read separately; no barrier
  fixes that, because it is a timing problem, not a reordering problem.
  Mitigation is read-verify-read: read `holder_cpu`, compute the verdict,
  re-read `holder_cpu`, and act only if it is unchanged and non-empty. Count
  the mismatch as `ivh_holder_raced`.
- **A departed holder's CPU must never remain readable.** With the above
  two rules this holds for the *slot*, but Option B adds a case the planning
  record missed: the same lock *address* can be freed and reallocated, so a
  stale slot whose `tag` still matches can hand back a departed holder.
  Therefore `ivh_lock_clear_holder()` must clear the **tag as well as** the
  holder. A slot with a cleared tag fails the tag check and reads "unknown",
  which is the safe direction. Do **not** assert that a reader seeing
  "locked" will see a matching `holder_cpu`; that is false by design, and
  read-verify-read is what bounds it.

### 3.4 The queue-head consult site, and its side effect

The queue head — the waiter spinning directly on the lock byte inside
`pv_wait_head_or_lock()` (`qspinlock_paravirt.h:745-757`) — is where
`is_cs_preempted()` is meant to be consulted, about **the current lock
holder**. Not the MCS-node path: `pv_wait_early()` already has the right
signal for its own question, because for an MCS node `prev->cpu` genuinely
is "the thing we are waiting on" (`qspinlock_paravirt.h:420-423`), whereas
the queue head is waiting on an owner it has no name for.

New helper, called from the spin loop beside the existing
`ivh_beat_publish_in_spin(loop)` at `:755`, at the same
`(loop & PV_PREV_CHECK_MASK) == 0` cadence `pv_wait_early()` uses
(`:405`, mask `0xff` at `:47`) — the point of that mask is not to pound the
remote cacheline, and it applies identically here:

```c
static inline bool ivh_cs_head_check(struct qspinlock *lock, int loop)
{
        unsigned long src = READ_ONCE(ivh_cs_preempt_src);
        int h1, h2, bucket;
        bool verdict, kvm;
        s64 age;

        if (likely(!src))
                return false;
        if (loop & PV_PREV_CHECK_MASK)
                return false;

        this_cpu_inc(ivh_cs_checks);

        h1 = ivh_lock_holder_cpu(lock);
        if (h1 < 0)
                return false;                   /* counted inside the lookup */
        if (h1 == raw_smp_processor_id()) {
                this_cpu_inc(ivh_holder_self); /* must be ~0; a real count is a bug */
                return false;
        }

        age     = ivh_cs_age(h1);               /* ONE rdtsc; see below */
        verdict = READ_ONCE(ivh_cs_predicate_form)
                        ? (age >= 0 && ivh_beat_stale(h1))
                        : (age >= 0 && age > (s64)READ_ONCE(ivh_cs_beat_threshold));
        kvm     = vcpu_is_preempted(h1);

        bucket = (age > 0) ? ilog2((u64)age) : 0;
        if (bucket >= IVH_CS_AGE_HIST_BUCKETS)
                bucket = IVH_CS_AGE_HIST_BUCKETS - 1;
        if (kvm) this_cpu_inc(ivh_cs_age_hist_preempted[bucket]);
        else     this_cpu_inc(ivh_cs_age_hist_running[bucket]);

        if (verdict == kvm)
                kvm ? this_cpu_inc(ivh_cs_agree_true)
                    : this_cpu_inc(ivh_cs_agree_false);
        else
                verdict ? this_cpu_inc(ivh_cs_false_pos)
                        : this_cpu_inc(ivh_cs_false_neg);

        h2 = ivh_lock_holder_cpu(lock);         /* R3 re-verify */
        if (h2 != h1) {
                this_cpu_inc(ivh_holder_raced);
                return false;
        }

        if (verdict) {
                this_cpu_inc(ivh_head_bail_early);
                bucket = ilog2((unsigned)loop | 1);
                if (bucket >= IVH_CS_LOOP_HIST_BUCKETS)
                        bucket = IVH_CS_LOOP_HIST_BUCKETS - 1;
                this_cpu_inc(ivh_head_bail_loop_hist[bucket]);
        }

        return (src == 2) && verdict;
}
```

Three things about this shape are deliberate and each mirrors existing
in-tree reasoning:

- **`ivh_cs_age()` is called once** and both the verdict and the histogram
  sample come from that single reading. Calling a separate `is_cs_preempted()`
  as well would take a second, later `rdtsc()` and the histogram would then
  describe a different event from the one the verdict was taken on — the
  exact point made at `ivh_tsc_beat.h:139-143` and `qspinlock_paravirt.h:289-293`.
- **`age >= 0` guards the `stamp == 0` sentinel.** A cleared stamp makes
  `rdtsc() - 0` an enormous positive, which would read as "preempted
  forever". Check `stamp != 0` inside `ivh_cs_age()` and return a negative
  sentinel; bucket 0 absorbs it, as it already absorbs small negative TSC
  skew (`qspinlock_paravirt.h:322-328`).
- **`src == 2` is the only thing that changes the return value.** At
  `src == 1` every counter and histogram is fed under the real workload and
  the function still returns `false`, so behaviour is unchanged. This is
  precisely the `ivh_prev_is_preempted()` posture (`:282-287`).

Call site:

```c
for (loop = SPIN_THRESHOLD; loop; loop--) {
        if (trylock_clear_pending(lock))
                goto gotlock;
        if (ivh_cs_head_check(lock, loop))
                break;                  /* NEW: falls through to clear_pending() */
        ivh_beat_publish_in_spin(loop);
        cpu_relax();
}
clear_pending(lock);
```

**The side effect, which must not be assumed harmless.** Breaking out of
this loop early runs `clear_pending(lock)` (`:758`) sooner than it otherwise
would. The pending bit is the anti-lock-stealing flag: `pv_hybrid_queued_unfair_trylock()`
steals only when `!(val & _Q_LOCKED_PENDING_MASK)` (`qspinlock_paravirt.h:100`),
and the whole starvation argument at `:73-87` rests on the queue head holding
that bit set while it spins. Clearing it earlier reopens the stealing window
and weakens that guarantee. This is categorically different from the
MCS-node call site, where `pv_wait_early()` returning true only changes
*when* a waiter moves from hot spin to backoff and the caller's loop
re-checks `node->locked` regardless (`:424-427`).

Hence `ivh_head_bail_early` and `ivh_head_bail_loop_hist[]`. The histogram
is the quantitative answer to "how much earlier": `SPIN_THRESHOLD` is
`1 << 15`, and the loop counts *down*, so a bail at loop `L` means the
pending bit is cleared `L` iterations early. A distribution concentrated
near `SPIN_THRESHOLD` means the bit is being dropped almost immediately and
the starvation guarantee is effectively gone; a distribution near 0 means
the bail is happening just before the loop would have exhausted anyway and
costs nothing. That distinction gets its own row in acceptance testing
(§3.9), and `lockevent_inc(pv_lock_stealing)` cannot substitute for it
because `CONFIG_LOCK_EVENT_COUNTS` is not set on this build
(`ivh_tsc_beat.h:80-83`) — so Build 1 must add a plain
`DEFINE_PER_CPU(u64, ivh_lock_steals)` beside the steal site at `:102` if
the steal rate is to be observable at all. It must be; add it.

### 3.5 Part C — the tick-only IVH stamp, at its now-reduced scope

#### 3.5.1 What Part C actually has to replace — the scope shrinks

The planning record's earlier audit claimed three live vcap outputs. Two of
them are not live in the current configuration. Verified this pass:

| vcap output | Live in the migration decision? | Evidence |
| --- | --- | --- |
| `rq->cpu_capacity` (from `cpu_capacity_custom`) | **YES** | Gate 1 at `fair.c:13218`; `GATE_CAPACITY_LOW` at `MY_ivh_atc.bpf.c:424`; `GATE_NOT_BETTER` at `:434` (against the *source* CPU's own capacity, not a system average) |
| `average_capacity_all` | **NO** | Passed to `bpf_sched_cfs_select_run_cpu_spin()` (`fair.c:13405`), stored into `process_cpu()`'s ctx (`:300`, `:660`), and never read in that function's body. Its only reads are at `:749` and `:765` inside `search_latency()`. |
| `rq->avg_latency` | **NO** | Only read at `MY_ivh_atc.bpf.c:766-768`, inside `search_latency()`. No in-kernel reader exists at all (`core.c:343-346` is the only toucher). |

`search_latency()` is reached only from `test32`, attached to
`SEC("sched/cfs_latency_select")` (`:777-779`). That hook is declared at
`include/linux/sched_hook_defs.h:11` and **called from nowhere in the
kernel** — grepping all of `kernel/` for `cfs_latency_select` returns the
declaration and nothing else. The loader does attach it
(`MY_ivh_atc.c:287-293`), so it is loaded, verified, and permanently idle.
`test32` is dead weight.

**Part C therefore replaces exactly one number.** `average_capacity_all` and
`rq->avg_latency` are out of scope — not "deferred", not "caveated": there
is no code path that consumes them. They come back into scope only if
`cfs_latency_select` is ever given a call site, and that is a separate
project.

Also confirmed still-dead, and safe to delete in Build 3: `rq->max_latency`
(`sched.h:1420`), written at `cputime.c:278-279`, read only by
`get_max_latency()` (`core.c:337-340`) for the `/proc/vcap_info` printer
(`vsched_module.c:329-340`). No in-kernel decision reader.

And confirmed **not** dead, which is the one thing Part C's shrinkage does
not simplify: `rq->last_preemption` and `rq->last_active_time` are Gate 2's
inputs at `fair.c:13243-13252` under the default `ivh_time_left_source=1`
(`bpf_sched.c:92`), and both are written purely from real
`paravirt_steal_clock()` at `cputime.c:268-277` regardless of
`ivh_steal_source`. `GATE_BURST_ORDER` and `GATE_BURST_BUDGET` — the two BPF
gates that would read them — are both `0` (`MY_ivh_atc.bpf.c:279-280`), so
the *only* live consumer is Gate 2. That is the whole of the known open gap,
and it is smaller than previously believed.

One further signal worth naming because it looks like it needs replacing and
does not: `rq->clock_preempt` (`sched.h:1408`) is written from
`sched_clock()` at the tick (`cputime.c:503`) and read by
`is_cpu_preempted()` (`cputime.c:288-293`), by `fair.c:214` and `:13541`,
and by the BPF `is_cpu_preempted()` behind `GATE_PREEMPTED`
(`MY_ivh_atc.bpf.c:185-186`). It is already a tick heartbeat with **zero
steal dependency**. Leave it alone.

#### 3.5.2 New `struct rq` fields

Placed immediately after the `ivh_ref_*` block that ends at `sched.h:1458`,
for the same access-pattern reason documented at `:1430-1437`: written only
by the owning CPU from the tick, read remotely at low rate, which is the
same shape as `preemptions`/`max_latency` and therefore correctly *not* on
its own cacheline (unlike the heartbeat).

| Field | Meaning |
| --- | --- |
| `u64 ivh_vact_stamp` | raw `rdtsc()` at the last **tick** publish. Tick-only, by construction. |
| `u64 ivh_vact_idle_exit_tsc` | raw `rdtsc()`, written beside `rq->last_idle_tp = sched_clock()` at `cputime.c:233` |
| `u64 ivh_vact_burst_start_tsc` | S1 — start of the current active burst |
| `u64 ivh_vact_last_preempt_tsc` | TSC of the last detected preemption event (reproduces `last_preemption`) |
| `u64 ivh_vact_last_active_c` | length of the last completed burst, in cycles (reproduces `last_active_time`) |
| `u64 ivh_vact_preemptions` | event count (reproduces `rq->preemptions`) |
| `u64 ivh_vact_win_start_tsc` | tumbling-window start |
| `u64 ivh_vact_win_used_c` / `_win_stolen_c` | window accumulators |
| `unsigned long ivh_vact_capacity` | **THE OUTPUT**, 0..1024, the `rq->cpu_capacity` replacement |
| `u64 ivh_vact_jumps` / `_idle_explained` | diagnostics: real events vs gaps explained by idle |

Everything is in raw TSC cycles. There is no nanosecond variant. The
planning record's original description compared raw cycles against a
nanosecond `sched_clock()` value, which is a literal unit-mismatch bug; the
user's stated preference is TSC-native ("doesn't matter as long as we don't
use steal, perhaps TSC so we can match"), and matching the heartbeat's units
is a real benefit. Conversion to ns for display happens in the `/proc`
printer via `tsc_khz`, nowhere else.

#### 3.5.3 The algorithm

At the tick, in `account_process_tick()` immediately after the existing
`ivh_ref_accumulate()` call (`cputime.c:547`) and before the
`vtime_accounting_enabled_this_cpu()` early return — same placement argument
as `clock_preempt` and `ivh_ref_accumulate()` already use (`:540-546`): the
sample must be taken on every tick on every CPU regardless of accounting
flavour.

```
now  = rdtsc();
old  = rq->ivh_vact_stamp;
rq->ivh_vact_stamp = now;                  /* the publish, always */

if (!old)                                  /* first tick, unseeded */
        { rq->ivh_vact_burst_start_tsc = now; goto window; }

age = (s64)(now - old);
if (age <= (s64)ivh_vact_jump_threshold)
        goto window;                       /* no gap: normal ticking */

/* A gap. Two possible explanations. */
if ((s64)(rq->ivh_vact_idle_exit_tsc - old) >= 0) {
        /* The CPU went idle after publishing `old` and came back at
         * idle_exit_tsc, so the gap is explained by idle, not preemption.
         * S1 resets to the idle EXIT, not to `now`. */
        rq->ivh_vact_burst_start_tsc = rq->ivh_vact_idle_exit_tsc;
        rq->ivh_vact_idle_explained++;
} else {
        /* Genuine host preemption. Close the burst, open a new one. */
        rq->ivh_vact_last_active_c   = old - rq->ivh_vact_burst_start_tsc;
        rq->ivh_vact_last_preempt_tsc = now;
        rq->ivh_vact_burst_start_tsc  = now;
        rq->ivh_vact_preemptions++;
        rq->ivh_vact_win_stolen_c    += age;
        rq->ivh_vact_jumps++;
}

window:
        rq->ivh_vact_win_used_c += <cycles since the previous tick, minus any
                                    part attributed to stolen above>;
        if ((s64)(now - rq->ivh_vact_win_start_tsc) >= ivh_vact_window_ns_in_cycles) {
                u64 tot = used + stolen;
                rq->ivh_vact_capacity = tot ? (used * 1024) / tot : 1024;
                /* tumble */
                rq->ivh_vact_win_start_tsc = now;
                rq->ivh_vact_win_used_c = rq->ivh_vact_win_stolen_c = 0;
        }
```

Four things to note, three of which are corrections to the informal
description:

- **The S1 reset after an idle-explained gap is `idle_exit_tsc`, not
  `now`.** `account_idle_ticks()` (`cputime.c:577-594`) does **not** call
  `account_process_tick()` — verified — so the stamp is not republished when
  idle time is accounted, and the first post-idle tick's gap-detection is
  comparing against the pre-idle stamp. Resetting S1 to `now` would discard
  the entire interval between idle exit and the first post-idle tick, which
  systematically undercounts every post-idle burst by up to one tick period.
  Resetting to `idle_exit_tsc` is correct, and it is the only reason
  `ivh_vact_idle_exit_tsc` needs to exist at all.
- **The idle test is `idle_exit_tsc >= old`, signed.** "The idle exit
  happened at or after the stamp we are about to overwrite" is exactly
  "the gap is explained by idle". Signed comparison so that a small
  cross-source skew reads as ordered rather than wrapping — the same
  discipline as `ivh_beat_age()` (`ivh_tsc_beat.h:130-143`).
- **`ivh_vact_jump_threshold` starts at the cycle equivalent of 1500 µs**,
  matching `IVH_BEAT_THRESHOLD_US` (`kvm.c:1250`) and the `> 1500000` ns in
  `is_cpu_preempted()` (`cputime.c:292`), for exactly the reason given at
  `ivh_tsc_beat.h:62-67`: it makes this a controlled comparison against the
  signal the tree already has, so a divergence is a bug rather than a new
  signal. Sysctl, sweepable.
- **No halt correction is needed.** A lock-path `safe_halt()` runs with
  `IF=1`, so the LAPIC timer fires during it and `account_process_tick()`
  runs, republishing the stamp. There is no gap for the jump detector to
  find, and therefore nothing analogous to `ivh_ref_halt_correct`
  (`core.c:245`) to build. This is Part C's structural advantage over
  reusing `rq->ivh_ref_steal_ns` and is why the reverted
  `ivh_account_preemption_event()` approach is abandoned rather than
  revisited. It is also a claim to *check*, not assume: `ivh_vact_jumps`
  should not correlate with `ivh_ref_hlt_ns` (`sched.h:1456`) rising.

#### 3.5.4 Consumption, all default-off

Gate 1 (`fair.c:13218`) and its no-side-effect twin
(`ivh_rq_capacity_and_timeleft_ok()`, `fair.c:13276` — the two bodies are
deliberately duplicated, see the comment at `:13256-13269`, and both must be
edited) become:

```c
unsigned long cap = READ_ONCE(ivh_cap_source) == 2
                        ? rq->ivh_vact_capacity : rq->cpu_capacity;
if (cap > ivh_capacity_threshold) { ...reject... }
```

Gate 2's `ivh_time_left_source == 1` formula (`fair.c:13243-13252`) selects
`rq->last_preemption`/`last_active_time` versus
`rq->ivh_vact_last_preempt_tsc`/`ivh_vact_last_active_c` (converted to ns at
the point of use, since the formula mixes them with `sched_clock()` values
and `current->last_cs_ns`) on `ivh_preempt_event_source == 2`.

The two BPF gates read `rq->ivh_vact_capacity` instead of
`rq->cpu_capacity` when the source sysctl selects it — a BPF reload, not a
kernel build. Note that `IVH_CAP_FLOOR` is `850` and `ivh_capacity_threshold`
is `1010` (`bpf_sched.c:23`) against a 1024 scale; the replacement must
produce numbers on the same scale or both thresholds silently change
meaning. Hence `(used * 1024) / (used + stolen)`, and hence
`ivh_vact_capacity` initialised to `1024` (perfectly healthy) rather than
`0`, so an unseeded CPU is not treated as fully stolen.

### 3.6 New sysctls — the complete list, and where each is registered

All default `0` unless noted. Three-valued knobs follow
`ivh_pv_preempt_src`'s established meaning: `0` off, `1` shadow (compute,
compare, log, do not act), `2` authoritative.

**In `ivh_pv_sysctls[]`, `arch/x86/kernel/kvm.c:1449`ff** (locking-side):

| Sysctl | Default | Meaning |
| --- | --- | --- |
| `ivh_cs_preempt_src` | `0` | `0`/`1`/`2` for the CS-stamp predicate. `2` is the only value that makes the queue head actually bail. Needs a validating handler refusing `> 2` and refusing `2` unless `ivh_lock_holder_enabled` is already `1` — otherwise `2` is "always unknown, never bail", a silent no-op. |
| `ivh_cs_predicate_form` | `1` | `0` = CS-stamp age; `1` = in-CS **and** liveness-heartbeat stale. §1.2. Defaults to the better-founded form. |
| `ivh_cs_beat_threshold` | calibrated | raw TSC cycles, from `tsc_khz` at `late_initcall` |
| `ivh_lock_holder_enabled` | `0` | `0` = no stamping at all (one predicted branch); `1` = stamp and clear. Separate from `ivh_cs_preempt_src` so the store's cost is measurable without the predicate, which is the §1.1 cost A/B. |
| `ivh_holder_bits` | `IVH_HOLDER_MAX_BITS` | effective index width; live-sweepable. Handler must clamp to `[6, IVH_HOLDER_MAX_BITS]` and zero the table on change, or the sweep reads stale tags from the previous geometry as collisions. |

**In the sched sysctl table, `kernel/sched/bpf_sched.c:276`ff:**

| Sysctl | Default | Meaning |
| --- | --- | --- |
| `ivh_cap_source` | `0` | `0` vcap's `rq->cpu_capacity`; `1` shadow-compare; `2` `rq->ivh_vact_capacity` authoritative |
| `ivh_preempt_event_source` | `0` | `0` real-steal `last_preemption`/`last_active_time`; `1` shadow; `2` `ivh_vact_*` authoritative |
| `ivh_vact_jump_threshold` | 1500 µs in cycles | tick-stamp staleness threshold |
| `ivh_vact_window_ns` | `100000000` (100 ms) | tumbling window for the capacity ratio. Start near vcap's own update period; check `main.cpp`'s loop period before committing. |
| `ivh_decision_shadow` | `0` | run the dual migration-decision evaluation (§3.8). Costs an `O(nr_cpus)` walk per evaluation, so measurement-window only. |

`ivh_cap_source` and `ivh_preempt_event_source` are deliberately **two**
knobs, not one. They are independent replacements with independent failure
modes, and the whole discipline this plan is built around is that they must
not go authoritative simultaneously.

### 3.7 Counters — the complete list, and why each one is here

All `DEFINE_PER_CPU(u64, ...)`, defined beside the existing `ivh_beat_*`
counters at `kvm.c:1253-1260`, summed and printed in `ivh_debug_show()`
(`fair.c:13576`, registered at `:13861`). Plain per-CPU `u64` rather than
`lockevent_*` for the reason already documented at `ivh_tsc_beat.h:80-83`:
`CONFIG_LOCK_EVENT_COUNTS` is not set, so every `lockevent_inc()` in
`qspinlock_paravirt.h` compiles to nothing and cannot carry these.

**CS predicate — mirrors the `ivh_beat_*` set exactly:**

`ivh_cs_checks`, `ivh_cs_publishes`, `ivh_cs_agree_true`,
`ivh_cs_agree_false`, `ivh_cs_false_pos`, `ivh_cs_false_neg`,
`ivh_cs_age_hist_running[32]`, `ivh_cs_age_hist_preempted[32]`.

*Why:* the split histograms **are** the threshold-calibration method and the
sign-convention check. The threshold is wherever the two distributions
separate (target: ≤1% of `_running` above it, ≥90% of `_preempted` above
it). If they do not separate, that is the answer, and the answer is that the
predicate does not work at this write cadence — which is a Build 2 finding,
not a Build 1 failure. Same reasoning as `ivh_tsc_beat.h:84-92`.

**CS stamp hygiene:** `ivh_cs_hold_hist[32]` (§3.2.3, the population-correct
hold-time distribution), `ivh_cs_clear_mismatch` (§3.2.2, how often the
`_raw_spin_unlock_bh` ordering actually bites).

**Holder identity:** `ivh_holder_stamps`, `ivh_holder_clears`,
`ivh_holder_unknown_empty`, `ivh_holder_unknown_collision`,
`ivh_holder_raced`, `ivh_holder_self`.

*Why each:* `stamps`/`clears` must track each other closely — a persistent
imbalance means a transfer site was missed, which is the failure mode that
corrupts holder identity silently. `unknown_empty` is the genuine handoff
window (old holder cleared and released, new holder acquired but has not
stamped yet) and is irreducible; `unknown_collision` is table-geometry and
is what the `ivh_holder_bits` sweep drives to zero; keeping them **separate**
is what makes "is the table big enough" answerable, since a single lumped
"unknown" counter cannot distinguish "physics" from "too small a table".
`raced` is the R3 skew rate. `self` should be ~0; a real count means either
a stale stamp or a genuine reentrancy bug and is worth a `pr_warn_once`.

**Head-bail side effect:** `ivh_head_bail_early`,
`ivh_head_bail_loop_hist[16]`, `ivh_lock_steals`.

*Why:* §3.4. `ivh_lock_steals` is a new plain counter at
`qspinlock_paravirt.h:102` because `lockevent_inc(pv_lock_stealing)` there
compiles to nothing on this config. Without it the "did the bail reopen the
stealing window" question is unanswerable and would force a rebuild — which
is exactly what constraint #2 forbids.

**Part C:** `ivh_vact_jumps`, `ivh_vact_idle_explained` (both per-rq, not
per-CPU-variable, since they live in `struct rq`), plus a per-CPU side-by-side
block in the `/proc` output:

```
# per cpu: real_last_preempt_ns  vact_last_preempt_ns  real_last_active_ns
#          vact_last_active_ns   real_preemptions      vact_preemptions
#          vcap_capacity         vact_capacity
ivh_vact_compare: 0 ...
```

*Why in `/proc/ivh_debug` and not only in the module:* the module can format
this for free, but it needs a kernel accessor to read it, and adding an
accessor is a kernel build. Ship the accessor **and** a raw dump in
`/proc/ivh_debug` in Build 1; then any reformatting afterwards is a module
rebuild, which costs nothing. This is the same lesson as
`get_inferred_steal()` / `get_real_steal()` (`core.c:294-318`), whose
existence is documented as necessary precisely because
`paravirt_steal_clock()` is not reachable from a module.

### 3.8 The decision-agreement comparator — constraint #2, discharged

Constraint #2 says: any boot containing a TSC-vs-real comparison must ship,
in that same build, whatever is needed to answer *"does IVH make the same
migration decisions with the TSC signal as with the real one?"* Counters on
the raw signals do not answer that. Two comparators do, and both go in
Build 1:

**(a) Gate 1+2 verdict agreement.** In `ivh_steal_imminent()`
(`fair.c:13214`), when `ivh_decision_shadow` is on, compute the verdict
twice — once from `(rq->cpu_capacity, rq->last_preemption,
rq->last_active_time)`, once from `(rq->ivh_vact_capacity,
ivh_vact_last_preempt_tsc, ivh_vact_last_active_c)` — and bin into a 2×2:

`ivh_dec_agree_go`, `ivh_dec_agree_nogo`, `ivh_dec_tsc_only_go`,
`ivh_dec_real_only_go`.

Return whichever verdict the source sysctls select. At
`ivh_cap_source=ivh_preempt_event_source=0` this is pure measurement.
Existing `ivh_steal_imminent_capacity_reject` /
`_time_left_reject` (`:13219`, `:13234`, `:13248`) stay untouched so their
meaning does not change — the comment at `:13256-13269` explains why that
matters and the same discipline applies to these new counters.

**(b) Destination-set agreement.** The full BPF scan cannot be run twice,
but the thing the capacity number actually decides can be: which CPUs pass
`GATE_CAPACITY_LOW` and `GATE_NOT_BETTER`. When `ivh_decision_shadow` is on,
`bpf_sched_pre_lock_migrate()` (`fair.c:13343`) walks the online CPUs once
before calling into BPF and counts, per evaluation:

`ivh_cap_pass_both`, `ivh_cap_pass_real_only`, `ivh_cap_pass_tsc_only`,
`ivh_cap_pass_neither`.

That is an `O(nr_cpus)` walk behind a default-off sysctl on a path that is
already doing `raw_spin_lock_irqsave` plus a BPF trampoline plus a full CPU
scan (`:13394-13408`), so it is affordable inside a measurement window. It
directly answers "would IVH have picked from the same candidate set", which
is the operational form of the question. If `pass_tsc_only` and
`pass_real_only` are both small relative to `pass_both`, the capacity
replacement is behaviourally equivalent and `ivh_cap_source=2` is safe. If
either is large, it is not, and the *direction* says which way the
replacement is biased.

For the CS predicate, the analogous comparator is already built into
`ivh_cs_head_check()`: the 2×2 against `vcpu_is_preempted(holder_cpu)` **is**
the "same decision as the real steal bit?" measurement, and
`ivh_head_bail_early` is the "how often would behaviour actually have
differed?" measurement. Nothing further is needed and nothing is deferred.

### 3.9 What must be reviewed before anything is flipped

Gather under both workloads this project uses — `hackbench` and the spinlock
workload — for at least 30 minutes each, per the acceptance discipline
already written down for `ivh_steal_source` at `core.c:204-211`. Remember
`ivh_exec -n`: the default 1:1 pinning gives `cpumask_weight == 1` and
Gate 3 (`fair.c:13367`) blocks every migration.

| # | Question | Read | Blocks |
| --- | --- | --- | --- |
| 1 | Does the CS-stamp age separate by host verdict? | `ivh_cs_age_hist_running[]` vs `_preempted[]` | `ivh_cs_preempt_src=2` |
| 2 | Is the sign convention right? | `_preempted` mass above `_running` mass | as above |
| 3 | Which predicate form? | 1 and 2 re-run at `ivh_cs_predicate_form` 0 and 1 | as above |
| 4 | Is the threshold right? | separation point vs `ivh_cs_hold_hist[]` p99.9; sweep `ivh_cs_beat_threshold` | as above |
| 5 | Is holder identity trustworthy? | `unknown_empty`, `unknown_collision`, `raced`, `self` as fractions of `ivh_cs_checks`; `stamps` vs `clears` balance | as above; also decides Build 2A |
| 6 | How big must the table be? | sweep `ivh_holder_bits`, plot `unknown_collision/stamps` | Build 2A |
| 7 | What does the holder store cost? | throughput at `ivh_lock_holder_enabled` 0 vs 1, same workload, same boot | whether any of this ships |
| 8 | How much earlier does the pending bit drop? | `ivh_head_bail_loop_hist[]`, `ivh_head_bail_early`, `ivh_lock_steals` at `src` 1 vs 2 | `ivh_cs_preempt_src=2` |
| 9 | Does the tick stamp reproduce the real preemption series? | per-CPU `vact_*` vs real columns; `ivh_vact_jumps` vs `rq->preemptions` | `ivh_preempt_event_source=2` |
| 10 | Is idle exclusion working? | `ivh_vact_idle_explained` non-zero on idle CPUs; no `ivh_vact_jumps` spike at idle→active | as above |
| 11 | Is the halt-immunity claim true? | `ivh_vact_jumps` uncorrelated with `rq->ivh_ref_hlt_ns` rising | as above |
| 12 | Does the capacity number track vcap's? | `vcap_capacity` vs `vact_capacity` per CPU, especially around 850 and 1010 | `ivh_cap_source=2` |
| 13 | Would the decision change? | `ivh_dec_*` 2×2 and `ivh_cap_pass_*` 2×2 | both `=2` flips |
| 14 | Is TSC comparability still holding? | `ivh_beat_min_age_percpu` over a multi-hour run — the drift check at `ivh_tsc_beat.h:93-101` now applies to two more consumers | everything |

Item 14 deserves emphasis. Cross-vCPU TSC drift is a **silent,
one-directional false-positive bias**: it never faults and never reads as a
correctness bug, it just quietly makes one vCPU look permanently preempted
to its neighbours. Adding `is_cs_preempted()` and the tick stamp doubles the
number of consumers exposed to it. Do not declare TSC comparability closed
until `ivh_beat_min_age_percpu` has been watched across hours, per the
instruction already in the tree.

### 3.10 Flip order inside Boot 1

One at a time, each reversible, each only after its own review items above
are green. Never two `=2` values simultaneously — that is the bimodal
phantom-steal lesson, and it is the entire reason these are four separate
knobs rather than one.

```
# Phase 0 — everything shadow. Gather. Review items 1-4, 9-12, 14.
ivh_lock_holder_enabled=1 ivh_cs_preempt_src=1 ivh_cap_source=1 \
  ivh_preempt_event_source=1 ivh_decision_shadow=1

# Phase 1 — cost only, no predicate. Review item 7.
ivh_lock_holder_enabled=1, everything else 0. A/B against 0.

# Phase 2 — Part C's capacity alone.       Review items 12, 13. Then revert.
ivh_cap_source=2

# Phase 3 — Part C's preempt events alone. Review items 9, 13. Then revert.
ivh_preempt_event_source=2

# Phase 4 — both Part C halves together.   The first combination. Watch for
#           the bimodal signature: capacity collapsing on every vCPU while
#           ivh_steal_imminent_capacity_reject stops firing.
ivh_cap_source=2 ivh_preempt_event_source=2

# Phase 5 — the CS predicate alone, on the Part C baseline of phase 0.
#           Review items 5, 6, 8.
ivh_cs_preempt_src=2

# Phase 6 — everything. Only if 4 and 5 were both clean in isolation.
```

Phases 2 and 3 before 4 is not ceremony. The phantom-steal bug was
diagnosable *only* because "either alone is fine, combined is broken" was
established as a fact — see the signature description at
`ivh_tsc_beat.h:196-201`. Establishing the same fact cheaply for these two
is worth the extra measurement windows, which cost minutes, not builds.

---

## 4. Build 2 — corrections

Its content is Boot 1's output. The three candidates that are worth
pre-thinking, so that Build 2 is one build and not two:

### 4.1 If the CS-stamp histograms do not separate

The write cadence is wrong: one stamp per hold is not enough resolution for
form 0. Options, in increasing cost:

1. **Ship form 1 only** and delete form 0. Free — it is already the
   default, and if item 3 of §3.9 shows form 1 works and form 0 does not,
   there is nothing to build.
2. **Refresh the CS stamp from inside the qspinlock spin loops** for the
   *holder's* benefit. This does not work: the holder is not spinning. There
   is no periodic hook inside a spinlock critical section, which is the
   fundamental reason form 0 cannot be fixed by adding write sites and the
   reason form 1 exists.
3. **Refresh from the tick.** `account_process_tick()` could republish the
   CS stamp when `current->lock_depth > 0`. That converts form 0 into
   something very close to form 1 while keeping the CS-scoped semantics.
   This is the real Build 2 change if one is needed.

### 4.2 If Part C's capacity diverges structurally from vcap's

Expect divergence in one specific direction and check for it explicitly:
vcap's `capacity_perc` is an EWMA over its own sampling loop
(`main.cpp:423-425`), whereas `ivh_vact_capacity` is a tumbling window.
Sweeping `ivh_vact_window_ns` is the first thing to try and costs nothing.
If a real correction term is needed, it goes here, and it should follow
`ivh_ref_halt_correct`'s pattern: a separate, independently selectable
term rather than folded into the main arithmetic
(`ivh_tsc_beat.h:180-187` explains why that split was worth it).

### 4.3 If the post-idle S1 reset is wrong

The specific failure to look for: `ivh_vact_idle_explained` firing on CPUs
that were not idle, or `ivh_vact_last_active_c` systematically shorter than
`rq->last_active_time` on CPUs that idle frequently. Both are visible in
Boot 1's per-CPU comparison block.

**Build 2 must re-ship every comparator from Build 1 unchanged.** The
temptation to "clean up now that we know the answer" is exactly wrong here:
Build 2 changes a signal, and a changed signal needs its comparator more
than an unchanged one does.

---

## 5. Build 2A — Option A, conditional

Build this **only** if §3.9 item 6 shows `unknown_collision/stamps` stays
unacceptable at `IVH_HOLDER_MAX_BITS`.

- Add `u32 holder_cpu` to `struct qspinlock`
  (`include/asm-generic/qspinlock_types.h:14-44`), inside
  `#ifdef CONFIG_IVH_LOCK_HOLDER`, after the existing union.
- `sizeof(spinlock_t)` goes 4 → 8. `USE_CMPXCHG_LOCKREF`
  (`lockref.h:21-23`) turns off, changing dentry refcounting behaviour
  kernel-wide. `ALLOC_SPLIT_PTLOCKS` (`mm_types_task.h:20`) does **not**
  change, since its test is `> BITS_PER_LONG/8` = `> 8`.
- `__ARCH_SPIN_LOCK_UNLOCKED` (`qspinlock_types.h:49`) needs a matching
  initialiser.
- The `ivh_lock_*_holder()` API is unchanged — only its implementation
  swaps. That is the whole point of having defined an API in Build 1 rather
  than open-coding the table.
- A/B against Option B requires two kernels, hence two boots. This is the
  only place in the plan where that is true, and it is why Option B is the
  default.

Everything else — the predicate, the counters, the call site, the flip
sequence — is unchanged, because Build 1 put them behind an API boundary.

---

## 6. Build 3 — decommission

Only after both Part C halves and the CS predicate have run authoritative
and stable across full acceptance windows.

**Delete, in this order:**

1. `rq->max_latency` (`sched.h:1420`), its write (`cputime.c:278-279`),
   `get_max_latency()` (`core.c:337-340`, `EXPORT_SYMBOL` at `:374`) and
   `reset_max_latency()` (`:349-355`, export at `:376`). Provably dead in
   the kernel. **But `/proc/vcap_info`'s wire format is frozen at exactly
   4 lines per CPU** (`vsched_module.c:318`, and a 5th field is recorded as
   having crashed `vcap` with `std::invalid_argument` on 2026-07-13), so
   removing the field means changing the module *and* `main.cpp` together.
   Sequence: module + userspace first, kernel field second.
2. `rq->avg_latency` (`sched.h:1421`), `set_avg_latency()`
   (`core.c:343-346`, export at `:375`), and its module caller
   (`vsched_module.c:207`). No kernel reader exists.
3. `average_capacity_all` (`core.c:197`), `get_average_capacity_all()`,
   `set_average_capacity_all()` (`:327-334`, exports at `:377-378`), the
   argument at `fair.c:13405`, and the `average_capacity` parameter of
   `cfs_select_run_cpu_spin` in `include/linux/sched_hook_defs.h:8`.
   Removing a hook parameter changes the BPF hook ABI, so `MY_ivh_atc.bpf.c`
   and its skeleton must be regenerated in lockstep. **If that lockstep is
   inconvenient, keep the parameter and pass a constant** — it is read by
   nothing, so the cost of leaving it is zero.
4. `test32` / `search_latency()` / `SEC("sched/cfs_latency_select")`
   (`MY_ivh_atc.bpf.c:706-829`) and its attach in the loader
   (`MY_ivh_atc.c:287-293`). BPF-only, no kernel build. Do this one early —
   it is free and it removes the only thing that makes items 2 and 3 look
   load-bearing.
5. `steal_account_process_time()`'s `rq->last_preemption` /
   `last_active_time` / `preemptions` writes (`cputime.c:268-280`). Keep
   `account_steal_time()` and `rq->prev_steal_time` — `/proc/stat`'s steal
   column is not IVH's to remove.
6. `set_custom_capacity()` (`core.c:357-360`), `rq->cpu_capacity_custom`
   (`sched.h:1416`) and the override at `fair.c:10083-10086`. **This is the
   irreversible one**, and it is the point past which vcap can no longer be
   the comparator. Do it last, in its own build if there is any doubt.
7. The shadow scaffolding: `ivh_decision_shadow` and its counters, the
   dual-evaluation paths, `ivh_cs_predicate_form` (collapse to whichever
   form won), and the losing branch of every source knob. Keep the
   histograms and the agreement counters — they cost nothing at
   `src == 0` and they are what makes a future regression diagnosable.

**Keep:** `rq->clock_preempt` and `is_cpu_preempted()` (already steal-free,
§3.5.1). `ivh_this_cpu_steal_ns()` in `cs_enter()`/`cs_exit()`
(`spinlock.c:363`, `:395`; implementation `kvm.c:434`). That last one is
deliberate and is restated because it is the easiest thing in this plan to
delete by accident: it is the **yardstick IVH is evaluated against** and must
never become an inferred number. `core.c:213-216` says so, and the scope
note at the head of `ivh_tsc_heartbeat_refcycles_build_plans_2026-07-26.md`
says so. It is exact host ground truth, unconditionally, forever.

### 6.1 The straggler-ban decision, which cannot be deferred

`vcap` forces `capacity_perc = 0.5` for every CPU on the topology banlist
(`/home/nick/vsched_main/vcapacity/main.cpp:377-379`), and that number
reaches the kernel through the capacity value itself
(`main.cpp:494` → `/proc/vcapacity_write` → `set_custom_capacity()`).
`0.5 * 1024 = 512`, which is below both `IVH_CAP_FLOOR` (850) and
`ivh_capacity_threshold` (1010). So a banned CPU today is (a) never selected
as a migration destination and (b) always considered "in danger" as a source.

`ivh_vact_capacity` is computed purely from this vCPU's own tick history and
knows nothing about the banlist. Turning off vcap therefore **silently
re-admits every banned CPU as a migration destination.** That is a real
behaviour change and it must be an explicit decision, not a side effect of
step 6 above. Three options:

1. **Reproduce the ban in the kernel.** A `cpumask` written through a new
   `/proc/ivh_banlist`, consulted in `process_cpu()` (or, better, as a
   kernel-side gate before the BPF call so the BPF program stays simpler).
   Costs a small kernel change, which means it belongs in **Build 1**, not
   Build 3 — adding it later is a whole extra boot for a `cpumask`.
2. **Fold the ban into the capacity number.** Clamp
   `ivh_vact_capacity` to 512 for banned CPUs. Preserves today's behaviour
   exactly, but reintroduces a userspace writer into the path Part C exists
   to remove, which defeats the point.
3. **Accept the removal** as an intentional behaviour change, and measure
   it: with `ivh_cap_source=2`, watch whether banned CPUs start appearing as
   `ivh_selected` destinations (the `trace_printk` at `fair.c:13410` already
   prints `dst`) and whether that costs a measured regression.

**Recommendation: ship option 1's `cpumask` in Build 1** — it is a few dozen
lines, it is inert until written, and it converts a Build-3 surprise into a
Build-1 knob. Then use option 3's measurement to decide whether to populate
it. This is the clearest instance in the plan of the general rule: anything
that *might* be needed later and costs a boot to add should go in Build 1
behind a default-off gate.

---

## 7. Honest statement of what this plan does and does not settle

**Settles on paper:** the site enumeration, the memory-ordering rules and
the two places the previous rules were wrong, the correct units for Part C,
the correct post-idle S1 reset, the reduced Part C scope (one number, not
three), which fields are genuinely dead, the fact that
`_raw_spin_unlock_bh()` disqualifies `cs_exit()` as a holder-identity site,
the fact that the specified `is_cs_preempted()` measures CS duration rather
than preemption and needs a second form, and the boot budget.

**Does not settle, and cannot:**

- Whether a gated store on the qspinlock ownership path is affordable under
  this project's workloads.
- What the real unknown-holder and collision rates are, and therefore
  whether Option B suffices or Option A must be built.
- Whether `is_cs_preempted()` — in either form — is a useful predicate at
  all, or whether the queue head's early bail costs more in reopened
  lock-stealing than it saves in avoided spinning.
- Whether the tick-only stamp reproduces the real preemption series closely
  enough for Gate 2, and the tumbling-window ratio closely enough for
  Gate 1's 1010-of-1024 threshold.

Every one of those is a measurement, and Build 1 is constructed so that all
of them are answerable from a **single boot**: each has its own counter or
histogram, each parameter that would otherwise need a rebuild to vary
(threshold, predicate form, table size, window length, capacity source,
preempt-event source) is a live sysctl, and each authoritative path is
already compiled in so that turning it on is an `echo` rather than a build.
If a follow-up boot turns out to be needed purely to add a counter, that is
a defect in this plan, not an unavoidable cost — the counter list in §3.7
is long on purpose.

---

## 8. Files referenced

**Kernel, to be modified:**
`arch/x86/include/asm/ivh_tsc_beat.h`,
`arch/x86/include/asm/qspinlock.h`,
`arch/x86/kernel/kvm.c`,
`include/asm-generic/qspinlock.h`,
`include/asm-generic/qspinlock_types.h` (Build 2A only),
`kernel/locking/qspinlock.c`,
`kernel/locking/qspinlock.h`,
`kernel/locking/qspinlock_paravirt.h`,
`kernel/locking/spinlock.c`,
`kernel/sched/bpf_sched.c`,
`kernel/sched/core.c`,
`kernel/sched/cputime.c`,
`kernel/sched/fair.c`,
`kernel/sched/sched.h`,
`include/linux/sched_hook_defs.h` (Build 3 only).

**Kernel, read-only context:**
`include/linux/lockref.h`, `include/linux/mm_types_task.h`,
`include/linux/bpf_sched.h`, `kernel/bounds.c`, `kernel/sched/topology.h`.

**Out-of-tree, no boot cost:**
`custom_modules/vsched_module.c` (and its divergent twin in
`/home/nick/kernels/linux-6.17/custom_modules/` — diff before touching),
`tools/bpf/MY_ivh_atc.bpf.c`, `tools/bpf/MY_ivh_atc.c`,
`/home/nick/vsched_main/vcapacity/main.cpp`,
`/home/nick/vsched_main/vtopology/main.cpp`.

**Live interfaces:**
`/proc/ivh_debug`, `/proc/vcap_info`, `/proc/vcap_preempted`,
`/proc/vcap_steal_compare`, `/proc/vact_write`, `/proc/vcapacity_write`,
`/proc/sys/kernel/ivh_*`.
