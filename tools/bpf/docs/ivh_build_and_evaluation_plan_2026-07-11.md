# IVH Build & Evaluation Plan — Hotlock, Time-Left, Health Page, Adaptive Spinning

**Status: final, ready to execute. Supersedes the informal plan this doc was derived from.**

---

## Execution status — overnight session, 2026-07-12

Everything below happened *before* the clean-kernel-build steps (the "next steps" list at the end of this section) — all of it on the current, uncommitted working tree, all rebuild-free (module/BPF/userspace only). Read this section first; it changes some of the plan body below in small, specific ways (flagged inline where relevant), but doesn't invalidate the Stage A/B structure.

### Checklist — what's actually done vs. still open

- [x] Audited committed (running) kernel vs. working tree — confirmed Hotlock, post-lock IVH, `ivh_migrate_self()`, and all qspinlock IVH code (incl. the freeze-yield fix) are working-tree-only. Also found the running kernel still uses the older `set_cpus_allowed_ptr()`+`schedule()` migration path, not `stop_one_cpu()`-based `ivh_migrate_self()`.
- [x] Built Checkpoint H (NHextend Hotlock-formula shim + `ivh_hotlock_observe` BPF tool, `tools/bpf/ivh_hotlock_observe.{c,bpf.c}`) — validated: uncontended 0.00% hot, contended 99.88% hot. Kernel-lock-side half (hackbench, daemons) still blocked on Stage B — `ivh_hotlock_note_waiter_enter()` isn't in a booted kernel yet.
- [x] Built Stage A5 (`process_cpu()` tier-2 abstain toggle, `ivh_tier2_fallback_enabled` in `MY_ivh_atc.bpf.c`) — tested both states, found tier-2 fallback *helps* in this asymmetric setup (contradicts the old findings-doc assumption that lateral migrations are pure cost — worth re-testing once other fixes below land, this result may not hold once the spurious-migration volume is fixed).
- [x] Built Stage A4 (adaptive-spinning NHextend prototype: holder-CPU-in-lock-word + `read_vcap_steal()`-based backoff) — validated alone (no `ivh_exec`): on-CPU wait burn 92-96% → ~53%, iterations up ~50-70%, host-preempted down. Real, reproducible, confirmed across 3 rounds.
- [x] Ran the 3-way comparison (no-opt / IVH-only / IVH+adaptive-spinning) — found IVH+adaptive-spinning regressed *worse* than IVH alone, with `ivh_migrations_done` exploding ~20x (1935-1951 → 35641-41223 over the same window).
- [x] **Investigated and root-caused the migration explosion (was the open "New, from tonight's finding" item — now closed).** Two independent Fable investigations, both confirmed live on the running system via `bpftrace` kstack attribution:
  - **Root cause**: the running/committed kernel's `ivh_pre_lock()` (`kernel/locking/spinlock.c:46-77`) is inlined into *every* real kernel spinlock acquisition (`_raw_spin_lock`, `_raw_spin_lock_irqsave`, etc.) for *any* `PF_IVH_ELIGIBLE` task. `ivh_exec` sets that flag on the whole process, not scoped to the one lock being tested — so every incidental kernel lock the process touches (file descriptor table locks from `open()`/`close()`, timer locks from `nanosleep()`) is *also* an IVH trigger, gated only by a 50µs/vCPU cooldown and a capacity gate that's currently very permissive (`ivh_capacity_threshold=1010` passes almost anything not at full 1024).
  - **Confirmed via kstack attribution** (`sudo bpftrace -e 'kprobe:bpf_sched_pre_lock_migrate /comm == "NHextend"/ { @[kstack(4)] = count(); }'`): with adaptive spinning off, `open`/`close` contribute 265 events in 5s vs. 4365 from the real syscall path. With it on, `open`/`close` (from `read_vcap_steal()`'s per-check `/proc/vcap_info` re-open) explode to ~500,000, dwarfing everything else 100-to-1.
  - **Fix applied and confirmed live tonight**: `read_vcap_steal()` (`NHextend.c`) now keeps a thread-local persistent fd and uses `pread(fd, buf, sz, 0)` instead of `open()`+`read()`+`close()` — confirmed empirically that `pread()` re-triggers the kernel's single-shot dump (the proc handler only blocks re-`read()`/`lseek()`, not `pread()`, since `FMODE_PREAD` survives `proc_reg_open()`'s `FMODE_LSEEK`-only clearing). **Result: migrations delta dropped from 35641-41223 to 2269 — ~94% reduction, confirmed live.**
  - **Residual, smaller, not yet fixed**: with the dominant `open`/`close` source gone, the kstack breakdown now shows `task_rq_lock`/`task_sched_runtime`/`cpu_clock_sample` (11,490 hits) as the new top contributor — this is `clock_gettime(CLOCK_THREAD_CPUTIME_ID)`'s internal implementation, which NHextend calls extensively for its own CS-timing measurements (including new calls added tonight for Stage A4's `wait_wall_ns`/`wait_oncpu_ns` tracking). Same class of problem, smaller magnitude, harder to remove since the timing measurement itself is load-bearing for the benchmark's own metrics — not addressed yet.
- [ ] **Not done, deliberately**: Stage A1 (`vsched_module.c` mmap page) — held back tonight out of caution (later found to be an unrelated benign labmate reboot, not a real risk signal — safe to proceed with Stage A1 whenever convenient, no longer blocked on this).
- [ ] **Not done**: Stage A3 sampler groundwork — time-boxed out, not blocking anything else.
- [ ] **New TODO, from tonight's `ivh_capacity_threshold` finding**: re-investigate the optimal value now that the spurious-trigger mechanism is understood. There are two competing pressures on this one sysctl: the original gate-permutation sweep found `1010` maximized real-migration catch rate (deliberately close to the 1022 healthy-pool ceiling); tonight's investigation found `1010` is also *why* Gate 1 barely rejects any incidental trigger, making the spurious-migration problem worse than it would be at the compiled default (`512`). Needs a real sweep, **on the fixed (`pread`) build**, to see whether a value exists that keeps most of the real catch-rate benefit while meaningfully cutting incidental-trigger volume — don't assume either extreme (1010 or 512) is actually optimal now that the tradeoff is understood; this is genuinely unmeasured. Suggested sweep points: 512, 700, 850, 950, 1010, re-run under the same asymmetric contention setup, reading both `ivh_migrations_done` deltas (total volume, a mix of real+incidental) *and* the host-preempted metric (the thing that actually matters) — the right value is whichever minimizes host-preempted, not whichever minimizes or maximizes raw migration count.

### Status of the clean-kernel-build "next steps" (the 6-item list from the last report)

1. Start from a clean checkout of `84f1e5fcc` — **not started**.
2. Port forward pre-lock gate chain (no change needed) + `ivh_virt_spin_lock()`/`ivh_spin_yield_enabled` (confirm default 0) + Hotlock wired only into observation — **not started**, but tonight's Checkpoint H work is the direct rehearsal for the observation-only Hotlock wiring this step calls for; the pattern is already proven, just needs porting onto the clean base.
3. Leave out post-lock entirely — **not started**, no change to this guidance.
4. Add Stage A1 as its own isolated change — **not started**; no longer blocked (reboot was benign/unrelated), but still worth doing deliberately rather than folding it into a bigger batch.
5. ~~Dig into the IVH+adaptive-spinning regression before the rebuild~~ — **done, superseded by the finding above.** The regression wasn't holder-CPU staleness (the originally-suspected mechanism) — it was the process-wide `PF_IVH_ELIGIBLE` scoping problem. This is now understood and partially fixed (`pread`), with a residual (`CLOCK_THREAD_CPUTIME_ID`) and a real open question (capacity threshold retuning) both tracked above. **This finding should directly inform step 2**: it's a live demonstration of exactly why Hotlock needs to be real (not just observed) before any kernel build relies on `PF_IVH_ELIGIBLE` scoping alone — reinforces that step 2's "observation only, no gating yet" discipline is the right call, not overcaution.
6. Boot-equivalence check after rebuild — **not started**, unchanged, still the right first step once a new kernel is booted.

**What's actually next, in order**: (a) the capacity-threshold re-sweep on the current fixed build (cheap, rebuild-free, directly answers an open question before it gets baked into a kernel build), (b) decide whether to also chase the residual `CLOCK_THREAD_CPUTIME_ID` source or accept it as a smaller, load-bearing cost, (c) Stage A1 (with you watching, per the plan's own caution — not because of the reboots, just because it's the first real kernel-module code change), (d) only then start on the clean-checkout steps 1-4/6 above.

---

## Changelog from the previous pass

This revision corrects and finalizes the adaptive-spinning item (formerly "item 4 / B4–B6") after direct source verification this session, and adds an explicit, unmissable validation checkpoint for Hotlock. Everything else — the Stage A/Stage B split, the shared health page, the time-left fix, the Stage B sysctl table, the post-boot validation sequence — is unchanged from the prior pass and is included here in full so this document stands alone.

1. **osq_lock (B5) — no complications, unchanged from original scope.** Verified: `grep -n "virt_spin_lock\|CONFIG_PARAVIRT\|static_branch" kernel/locking/osq_lock.c` returns zero matches. osq_lock has no virt-guest bypass in any config — it's always the native MCS queue. B5 stays a one-line swap of `vcpu_is_preempted()` for `is_cpu_preempted()` at `osq_lock.c:153`.
2. **Mutex adaptive spinning (B4) — dropped entirely.** User explicitly does not want it. Removed from scope.
3. **qspinlock (B6) — original design was structurally wrong, not just risky, and has been redesigned.** Verified via source: `queued_spin_lock_slowpath()` (`qspinlock.c:229–268`) unconditionally takes `ivh_virt_spin_lock()`'s TAS loop and returns before ever reaching the native pending-bit/MCS code below it, on this VM's config (`CONFIG_PARAVIRT=y`, `CONFIG_PARAVIRT_SPINLOCKS` off) — already kprobe-confirmed in the codebase's own comment (2.67M+ contended entries, zero hits on the native-path-only waiter counter). The original `nopv_spinlock` design (extending `struct qnode` with predecessor-checking wait/wake) targeted that unreached code and would never have run. Enabling `CONFIG_PARAVIRT_SPINLOCKS` was considered and **explicitly rejected**: verified `vcpu_is_preempted()` itself is defined only inside `#ifdef CONFIG_PARAVIRT_SPINLOCKS` (`arch/x86/include/asm/qspinlock.h:30-63`), so flipping that config would work, but it also silently pulls in real `pv_wait()`/`pv_kick()` hypercalls — directly contradicting this project's own stated "no hypervisor cooperation" positioning. The corrected design: a new sysctl-gated fall-through inside `ivh_virt_spin_lock()`'s call site that skips the TAS path and lets execution reach the native MCS code already sitting unused in the same file — zero config change, zero hypercalls, reversible via sysctl. The predecessor-checking design is then layered on top of that now-reachable native path.
4. **New, explicitly late-staged item: remove the `ivh_spin_yield_enabled` TAS-loop yield.** This mechanism fixes a real, already-confirmed VM freeze (a migrated confirmed holder landing behind an unyielding TAS spinner). It must not be removed until B6's fall-through is built, validated, and has become the actual default behavior for contended locks — until then the TAS path is still 100% of what real contended acquisitions take, and the fix is still live-critical. Added as the final milestone in this plan, explicitly gated on B6.
5. **Hotlock validation is promoted to a named, unmissable checkpoint** ("Checkpoint H") with an explicit pass/fail gate, rather than being one phase among several. No kernel-side gating on any Hotlock verdict is permitted to exist until Checkpoint H passes.

---

## Context (for anyone reading this doc cold)

IVH prevents lock-holder preemption (LHP) in a KVM guest by migrating a thread off a vCPU that looks likely to be host-stolen. Two independent gating mechanisms exist today, both documented in `hotlock_and_timeleft_reference_2026-07-11.md`:

- **Hotlock** — decides *which locks* are worth intervening on, via a per-lock live waiter count + decaying contention history (`kernel/locking/spinlock.c:189-320,396-417`). Already load-bearing for post-lock migration (locus c); optional and off-by-default for pre-lock (locus a).
- **`ivh_steal_imminent()` / time-left** — decides *whether the current vCPU* is in enough danger of an imminent steal to justify migrating at all (`kernel/sched/fair.c:13227-13264`). Two confirmed bugs (see Item 2 below).

The project's own history (`ivh_findings_2026-07-02.md`, `post_acquisition_reactive_migration_2026-07-02.md`) is the load-bearing precedent for two things this plan repeatedly leans on: (a) pre-lock, predictive IVH on kernel locks is a closed, negative result — don't re-litigate it; (b) the failure modes that matter here are not "doesn't work," they're **VM-wide freezes** from careless migration/spin interactions — every risky change in this plan gets an explicit freeze-precedent callout.

**Environment constraint driving the whole structure:** loadable-module (`custom_modules/vsched_module.c`) and BPF-program (`tools/bpf/*.bpf.c`) changes are cheap — rebuilt/reloaded in seconds, no reboot. Core-kernel changes (`kernel/locking/*.c`, `kernel/sched/*.c`) require a full rebuild and a manual reboot. Minimizing rebuild+reboot cycles is a primary optimization target.

---

## Plan structure: two stages, one rebuild+reboot cycle

- **Stage A (no rebuild, no reboot):** the shared health page (module), Hotlock observation + Checkpoint H (module/BPF/userspace only), time-left measurement and offline fix validation, the adaptive-spinning NHextend prototype, and the optional BPF abstain change. Every Stage A result either de-risks or *eliminates* a Stage B kernel change.
- **Stage B (exactly one rebuild+reboot):** the time-left kernel fix, osq_lock's one-line swap, the qspinlock fall-through mechanism — **every behavioral change individually behind its own new default-off sysctl**, so the freshly booted kernel is behaviorally identical to today's until switches are flipped one at a time, one soak-tested step apart.
- **Stage C (deliberately deferred, not part of the Stage B batch):** removing the TAS-loop yield fix, gated on B6 becoming the real default and soak-tested as such. This is its own later milestone by design — bundling it into Stage B would remove a live freeze fix while its replacement is still unproven.

Floor: **1 rebuild cycle** for everything that must ever touch core kernel source in this plan (time-left fix, osq swap, qspinlock fall-through). Stage C is a second, much later, much smaller cycle (a config/sysctl flip plus deleting now-dead code), not a second *design* cycle — nothing new is being invented at that point, only retiring a fix once its replacement has taken over.

---

## Stage A0 — housekeeping (do first)

Everything in `hotlock_and_timeleft_reference_2026-07-11.md`, `ivh_findings_2026-07-02.md`, and `post_acquisition_reactive_migration_2026-07-02.md` describes work that exists **only as uncommitted working-tree changes** on `rseq-port` (16 modified files, last real commit `84f1e5fcc`). Before anything else:

1. `git add -A && git commit` on `rseq-port` (user-run — do not let this plan's own edits be the first thing that touches an uncommitted tree). A stray `git checkout`/`reset` during this multi-week plan would otherwise be unrecoverable except via reflog.
2. Create `/home/nick/ivh_logs/` for all captures. **Never `/tmp`** — documented lesson from this project's own freeze-repro history: `/tmp` does not survive a hard VM reset, `/home/nick/...` does.
3. Restore runtime state per the reference doc's §5 checklist: load `vsched_module.ko` for the currently-booted kernel (explicit `uname -r`, not the tool's silent default), start exactly one `vcap` and one `MY_ivh_atc` (check with `ps aux | grep -E "vcap|MY_ivh"`, not `pgrep`, which hangs under load — duplicate `vcap` causes D-state hangs), confirm `/proc/vcap_info` and `/proc/ivh_debug` are live.

---

## Stage A1 — Shared health page (`vsched_module.c` extension)

**Why first:** this is the substrate every other Stage A item consumes (Hotlock's syscall-side prefilter, the time-left ground-truth sampler, and the adaptive-spinning prototype's health check).

**What:** a new proc entry (e.g. `/proc/vcap_page`) with a `.proc_mmap` handler mapping one kernel page read-only. A module hrtimer (period = module param, default ~250µs) fills a per-CPU record array:

```c
struct vcap_page_rec {            /* one per CPU, cacheline-aligned */
    u64 refresh_ns;                /* sched_clock() at last refresh */
    u32 preempted;                 /* is_cpu_preempted(cpu) -- already EXPORT_SYMBOL */
    u32 capacity;                  /* stashed from set_capacities(), already written by vcap */
    u64 preemptions;               /* via get_steal_and_preemptions() -- already exported */
    u64 steal_ns;                  /* ditto -- raw paravirt steal clock */
};
```

Design notes:
- **Capacity is free:** `vcap` already writes per-CPU capacities into this module via `/proc/vcapacity_write` → `set_capacities()` (`vsched_module.c:74-96`) — stash a copy into the page there, no new kernel symbol.
- `is_cpu_preempted(self)` reads false almost by construction while you're the one running (you need host time to execute the check at all — the PARM doc's "self-check paradox," `post_acquisition_reactive_migration_2026-07-02.md`). Therefore: the **self**-prefiltering consumer (Item 1's syscall-avoidance check) must key off `capacity`, not `preempted`; the `preempted` field is only meaningful for checking a **different** CPU (the lock-holder health check in the adaptive-spinning prototype).
- Allocation: `alloc_page()` + `vm_insert_page()` (refcounted); `try_module_get()`/`module_put()` in VMA open/close; free the page only in `module_exit` — this specifically closes an rmmod-while-mapped UAF, test it explicitly once.
- Plain `WRITE_ONCE` per field; aligned `u64` reads are atomic on x86-64; torn multi-field snapshots are acceptable (same tolerance the hotlock table itself already accepts).

**Evaluation:**
- *Correctness:* userspace checker mmaps the page and simultaneously reads `/proc/vcap_info` in a loop; assert `preemptions`/`steal_ns` agree within one refresh period. Reuse NHextend's `read_vcap_steal()` parser (`NHextend.c:36-68`) verbatim.
- *Freshness:* record `refresh_ns` deltas over 10s under hackbench load. Pass = p99 delta ≤ 2× the timer period.
- *Zero-syscall claim:* `strace -c` the checker's steady-state loop. Pass = zero syscalls after the initial mmap.
- *Overhead:* module loaded with timer at 250µs vs. module loaded with timer disabled (param), 5 interleaved paired hackbench rounds each. Pass = statistically indistinguishable delta.

**Risk:** module-only — worst case is `rmmod`/reboot recovery, no kernel-image risk. Specifically test rmmod-while-mapped once, deliberately, rather than discovering it accidentally.

---

## Checkpoint H — Hotlock observation validation (mandatory, unmissable gate)

**This is the single most important gate in the entire plan.** The user's specific, named concern is kernel panic / freeze risk from Hotlock "trying to move everything" if its verdicts are wired into a real gating decision before they're known-good. This checkpoint exists to make that risk visible and blockable, not to be one phase among several.

**Hard rule: no kernel-side code path may act on a Hotlock verdict — skip a migration, skip a syscall, alter scheduling behavior in any way — until Checkpoint H has explicitly passed.** Everything up to that point is observation-only, and observation-only is enforced by construction (nothing downstream of the classifier is wired to anything that changes behavior).

**Build (all Stage A, no rebuild):**
1. **Userspace waiter-count shim in NHextend** (same shape as the glibc patch): an atomic per-lock waiter counter incremented/decremented around the userspace spin-wait — the "pthread waiter counter" already scoped in `post_acquisition_reactive_migration_2026-07-02.md` as ~a day of work (glibc's `__nusers` is not a usable live waiter count; real futex waiters live in the kernel's internal hash).
2. NHextend replicates the kernel's exact hotness formula in userspace (EWMA with `k=3`, `hot = waiters>0 || history > HALF`, ported directly from `spinlock.c:263-320`), and passes `(lock_addr, waiters, verdict)` as **ignored arguments** to `syscall(470, ...)` (`sys_ivh_cs_enter` is `SYSCALL_DEFINE0` — it discards them; behavior is provably unchanged by this alone).
3. **Observation-only BPF program** (new small tool, built in seconds via `tools/bpf/Makefile`, same pattern as `lhp_cstime`): a kprobe on `__x64_sys_ivh_cs_enter` reads the ignored args from `pt_regs`, aggregates per-TGID and per-lock hot/cold counts into a map; the loader dumps a table on exit.
4. **Kernel-lock side of the same measurement:** kprobe `ivh_hotlock_note_waiter_enter()` (a real, non-inlined, already-`EXPORT_SYMBOL_GPL`'d function, `spinlock.c:396`) and count per-TGID contention events — no kernel code changes, this is purely observational instrumentation attached externally.

**Validation battery (must include both a "should be hot" and a "should be cold" population, run together, not separately):**
- Hot: `ivh_exec`-wrapped `hackbench -T -g1 -f8 -l400000`; `ivh_exec NHextend -n -l 16` (contended).
- Cold: `NHextend -n -l 1` (uncontended control), plus ≥60s of ordinary background activity — `snap`, `sshd`, an interactive shell.

**Explicit pass/fail:**
- **PASS** requires all of: contended-population lock acquisitions classified hot ≥ ~90% during contention windows; uncontended-control lock classified cold ≥ ~99%; background-daemon/shell TGIDs classified hot ≈ 0; and — the specific check that guards against silently vetoing real IVH wins — the set of acquisitions the classifier *would* mark cold does not overlap with the acquisitions that currently produce real `ivh_migrations_done` increments (cross-check the two counters over the same run).
- **FAIL on any of the above** → do not proceed to any gating change (Stage A phase-3 userspace skip, or the optional B3 kernel-side gate below). Return to classifier tuning (EWMA `k`, `IVH_HOTLOCK_HALF` threshold) and re-run the full battery. There is no partial credit — a classifier that's right 90% of the time but wrong on the wrong 10% (e.g., misclassifying a genuinely hot workload as cold) is exactly the failure mode this checkpoint exists to catch before it can affect real scheduling.

**Only after Checkpoint H passes:** implement the userspace-side skip (`if (!lock_is_hot(&l)) skip syscall`), flag-gated, default off, exactly mirroring the sysctl-staging discipline this project already uses elsewhere (`ivh_pre_lock_hotlock_enabled`, `ivh_post_lock_dispatch`, etc.). Because the shim already computes the verdict in userspace, skipping the syscall in userspace is strictly better than any kernel-side gate would be: it saves the full syscall-entry cost, needs no rebuild, and composes at exactly the same call site as Stage A1's health-page prefilter:

```c
if (lock_is_hot(&l) && vcap_page[my_rseq_cpu].capacity <= CAP_THRESHOLD)
    ivh_cs_enter();
```

**Pushback, restated as a recommendation:** a kernel-side gate (`sys_ivh_cs_enter` reading a lock-identity argument, or the rseq `wait_counter`) is optional infrastructure, not a requirement — include it in Stage B (as B3, below) only if the paper specifically needs "the kernel itself enforces the gate" as a claim. It never justifies its own rebuild cycle; it rides Stage B only because Stage B is already happening for other reasons.

**Risk:** Checkpoint H itself is observation-only — its only possible failure mode is producing misleading data, which the battery's pass/fail criteria are designed to catch. The risk this checkpoint exists to prevent (a bad Hotlock verdict causing a bad kernel-side action) is structurally impossible until the checkpoint passes and the gate is deliberately built afterward.

---

## Stage A3 — Time-left (`ivh_steal_imminent()`) measurement and offline fix validation

**Correction to the "new instrumentation needed" framing:** `rq->preemptions` and `rq->max_latency` are **already surfaced** — `/proc/vcap_info` (`get_info_read()` → `get_steal_and_preemptions()`, `kernel/sched/core.c:191`) already prints per-CPU `preemptions`, raw `paravirt_steal_clock()`, and `max_latency`. No kernel change is needed to get ground truth — only a higher-rate reader, which Stage A1's shared page already provides.

**Two confirmed bugs, unchanged from the original brief:**
- (a) Gate 2 subtracts raw `current->last_cs_ns` (whole wall-clock CS length) instead of the disruption-corrected `last_cs_ns - last_cs_oncpu_ns` that post-lock (locus c) already uses (`fair.c:13555-13561`) — this saturates the gate open on long synthetic critical sections.
- (b) `rq->last_active_time` stays 0 (Gate 2 permanently inert) until a >1ms steal has actually been detected once (`cputime.c:270-276`). The retired `ewma_act_ns` field is still computed and logged and could serve as a fallback in that window.

**Note (Finding, this session):** the syscall path (`sys_ivh_cs_enter`) has no lock identity and, more importantly for this item, evaluates `current->last_cs_ns`/`last_cs_oncpu_ns` — fields written only by the **kernel** `cs_exit()` (`spinlock.c:142-152`). On the userspace-lock path, Gate 2 is therefore looking at the caller's last *kernel* CS, not its userspace one. The userspace CS duration is already available in the extended rseq area (`last_cs_overall_ns`/`last_cs_active_ns`, `NHextend.c:95-96`). Decide during Stage A3 whether the kernel-side fix should read the rseq fields for the syscall path specifically, or accept the kernel-CS proxy as good enough — this decision should be made with data, not assumed.

**Measurement plan (no rebuild):**
1. **Ground-truth sampler:** mmaps the Stage A1 page, logs `(monotonic_ts, cpu, preemptions, steal_ns)` at ~1ms resolution to `/home/nick/ivh_logs/`. Fallback if A1 slips: poll `/proc/vcap_info` at 10–20ms (coarser, NHextend-style).
2. **Trace capture that survives the documented volume problem** (`ivh_hotlock:` and `ivh_time_left:` currently share one sysctl and one ring buffer; ~97% loss measured under real contention, per the reference doc §3): bump `/sys/kernel/debug/tracing/buffer_size_kb` (per-CPU) substantially, drain via one reader per `per_cpu/cpuN/trace_pipe` instead of the single serializing global `trace_pipe`, keep capture windows short (10–20s), enable `ivh_trace_enabled=1` only inside the window. Pass for the capture method itself = <5% `LOST .* EVENTS` (vs. the ~97% baseline).
3. **Offline scorer (Python):** each `ivh_time_left:` line already contains both `ewma_act_ns` and `last_active_time`. Label each sample positive iff that CPU's `steal_ns`/`preemptions` (from the Stage A1 sampler) increments within N ms of the sample (sweep N = 1, 5, 10ms). Score proceed/reject precision-recall under: (i) current formula, (ii) `ewma_act_ns` substituted whenever `last_active_time==0`, (iii) threshold sweeps around `ivh_time_left_threshold_ns`. Use the conditional-precision framing already established in the PARM doc rather than rederiving a metric from scratch.
4. **Quantify both bugs directly, before fixing either:**
   - Bug (b) magnitude: fraction of live samples where `last_active_time==0` (Gate 2 provably inert during that fraction) — directly measurable now from the trace.
   - Bug (a) magnitude: the wall-vs-oncpu inflation of `last_cs_ns` under real steal — measurable today via NHextend's existing `overall_ns` vs `active_ns` per-thread stats (the 27.6ms-wall vs 4.1ms-active example already recorded in this project's session history is exactly this signal). Full offline replay of the fix waits on Stage B adding `last_cs_oncpu_ns` to the trace line, but the bug's magnitude is boundable now.

**Deliverable exiting Stage A3:** a short numbers summary that pins down the exact Stage B change — subtract `last_cs_oncpu_ns` instead of `last_cs_ns`; fall back to `rq->ewma_act_ns` when `last_active_time==0`; chosen threshold values — plus an explicit decision on the syscall-path rseq-field question above.

**Risk:** this stage is measurement-only; its only failure mode is an inconclusive or misleading dataset, caught by insisting on the <5% trace-loss bar before trusting any conclusion drawn from it.

---

## Stage A4 — Adaptive-spinning prototype, entirely in NHextend (no rebuild)

Prototype the mechanism in userspace before touching any core-kernel locking code, exactly as originally scoped — this piece is unchanged by this session's corrections (the corrections apply only to the kernel-side port, Stage B).

1. **Holder identity in the lock word:** `cmpxchg(&data->lock, 0, my_cpu + 1)` (CPU sourced from the already-mapped `rseq_map->cpu_id` — free) instead of `0 → 1`. The waiter loop (`grab_lock()`, `NHextend.c:349-368`) decodes the holder's CPU from the observed lock value.
2. **Health-aware spin:** while spinning, poll `vcap_page[holder_cpu].preempted` from Stage A1's shared page — this is the case where `is_cpu_preempted()` is actually meaningful, since it's checking a *different* CPU, not self. If preempted: back off (`sched_yield()`, then escalating short `nanosleep`) instead of burning the vCPU; resume spinning once the flag clears or the lock word changes.
3. **Known, accepted staleness:** the embedded CPU can go stale if the holder is migrated mid-CS by the ordinary load balancer (note: IVH pre-lock migration happens *before* acquisition, so it does not cause this specific case). A stale CPU degrades to blind spinning — it never corrupts anything. Measure the staleness rate; don't try to eliminate it in the prototype.
4. **New per-thread counters:** backoffs taken, spin-loop wall-vs-on-CPU time during waits (`CLOCK_THREAD_CPUTIME_ID` bracketing, same pattern already used for the existing CS stats).

**Evaluation:** reuse the confirmed-stable asymmetric contention setup from `ivh_findings_2026-07-02.md` (1:1 `virsh vcpupin` mirroring the co-tenant VM's own pinning scheme — the exact recipe, including the root-cause diagnosis of why an 8-thread reduction alone didn't produce a stable split, is written down there). ≥10 interleaved paired rounds, adaptive-spin on vs. off.

**Pass:** waiter on-CPU burn during waits drops materially — this mechanism's honest, defensible claim is *saved compute*, not *wall-clock improvement* (per the professor-feedback framing in `post_acquisition_reactive_migration_2026-07-02.md`: adaptive spinning and PARM solve different halves of the same aggregate cost) — **while** `total_wait`/`max_wait` are not worse, and the result **survives the outlier-removal robustness check** that invalidated two earlier "wins" in this project's history (remove the single largest-magnitude round; the sign of the result must not flip). Also verify backoff-event count tracks the ground-truth sampler's steal-event count — it should fire when steal is real, not constantly.

**Risk:** pure userspace — worst case is a slower benchmark, fully recoverable, no reboot. The one subtle failure mode to watch for is an over-backoff livelock shape (many threads sleeping simultaneously while the lock is actually free); detect via the backoff counter spiking disproportionately to the sampler's steal-event count.

---

## Stage A5 (optional) — BPF abstain in `MY_ivh_atc.bpf.c`

Cheap, and composes naturally with Stage A4's "IVH gives up, adaptive spinning takes over" handoff. The tier-2 fallback in `process_cpu()` (`MY_ivh_atc.bpf.c:487-493`) currently records an `nr_running==1` destination when no genuinely idle CPU exists — a lateral, mediocre migration. Add a loader-settable `const volatile` toggle: when set, skip recording tier-2 entirely, so the selector returns `-1` and both loci cleanly no-op. This mostly changes the pre-lock/syscall path's behavior specifically (post-lock's `dest_busy` check at `fair.c:13713` already rejects these destinations independently).

**Evaluation:** `ivh_post_lock_no_target`/`ACC_TIER2_NR1` reason-counter shifts, plus one paired NHextend batch. This doubles as a live re-test of the findings-doc thesis that lateral migrations were pure cost with no upside.

**Risk:** BPF-only, reload in seconds, no reboot, no kernel risk.

---

## Stage B — the single rebuild+reboot batch

**Preconditions before starting Stage B edits:**
- Checkpoint H has passed.
- Stage A3's numbers summary exists and pins the exact time-left fix.
- Stage A4's prototype has a verdict (materially reduced spin-burn, surviving the outlier check).
- Commit the working tree again immediately before starting B edits.

**Contents — each item independently developed/reviewed, all landing in one build, every behavioral delta behind its own new default-off sysctl:**

| # | Change | Files | Gate (default 0) |
|---|---|---|---|
| B1 | Split `ivh_time_left:` trace onto its own switch (currently shares one sysctl with the much noisier `ivh_hotlock:` trace — ~97% loss measured) | `fair.c`, `bpf_sched.c` | `ivh_time_left_trace_enabled` |
| B2 | Gate-2 v2: subtract `last_cs_oncpu_ns` instead of raw `last_cs_ns`; fall back to `rq->ewma_act_ns` when `last_active_time==0`; add `last_cs_oncpu_ns` to the trace line; (per A3's decision) optionally read rseq CS fields on the syscall path | `fair.c` (`ivh_steal_imminent()`, ~13227-13264) | `ivh_time_left_v2` |
| B3 | *(optional, only if Checkpoint H's paper needs a kernel-enforced claim)* syscall-side Hotlock gate consuming a real lock-identity argument, plus per-TGID `/proc/ivh_debug` counters | `bpf_sched.c` (`sys_ivh_cs_enter`) | `ivh_cs_enter_hotgate` |
| B4 | ~~mutex adaptive spin~~ — **dropped**, out of scope per explicit user decision | — | — |
| B5 | osq_lock: swap `vcpu_is_preempted(node_cpu(node->prev))` for `is_cpu_preempted(node_cpu(node->prev))`. Confirmed one-line, no dependency on B6 — osq has no virt bypass in any config | `kernel/locking/osq_lock.c:153` | `ivh_adaptive_osq` |
| B6 | qspinlock native-path fall-through (see design below) | `kernel/locking/qspinlock.c`, `kernel/locking/qspinlock.h` | `ivh_qspin_native_fallthrough` |
| B7 | Reject-reason / event counters for B2, B5, B6 (per-CPU, surfaced via `/proc/ivh_debug`) | same files | — (counters, inert by construction) |

### B6 — qspinlock native-path fall-through, corrected design

**Do not touch `CONFIG_PARAVIRT_SPINLOCKS`.** That was considered and rejected: `vcpu_is_preempted()` is defined only under that config (`arch/x86/include/asm/qspinlock.h:30-63`), so flipping it would work functionally, but it also silently activates real `pv_wait()`/`pv_kick()` hypercalls to the host — a direct contradiction of this project's own stated "no hypervisor cooperation" positioning (`post_acquisition_reactive_migration_2026-07-02.md`, "Why this isn't PV spinlock"). **This rejection is deliberate and should not be silently reconsidered** — if a future session revisits it, that section is the reasoning to re-read first.

**The corrected mechanism, in two layers:**

*Layer 1 — reachability (the actual B6 landed in this rebuild):* add a new sysctl, default 0. When set, `ivh_virt_spin_lock()`'s call site (`queued_spin_lock_slowpath()`, `qspinlock.c:229-268`) skips its own TAS loop and falls through into the native pending-bit/MCS code already present in the same file — code that exists today and is simply structurally unreached on this VM's config. No PV involvement anywhere in this layer; purely "use the code that's already there instead of the TAS loop." Ship this, flip it, and soak-test it *before* building Layer 2 — it is independently useful (it's the precondition for the native waiter-count-driven Hotlock signal the qspinlock/MCS structural-findings section of `post_acquisition_reactive_migration_2026-07-02.md` already identified as missing) and independently risky enough to deserve its own soak window inside Stage B's flip-one-at-a-time sequence.

*Layer 2 — the predecessor-health check (the original `nopv_spinlock` idea, now targeting reachable code):* once Layer 1 has native MCS actually running, extend `struct qnode` (`kernel/locking/qspinlock.h:40-45` — note the existing `reserved[2]` padding is itself gated `#ifdef CONFIG_PARAVIRT_SPINLOCKS`, so new fields must be added unconditionally, not inside that same guard) with `cpu`/`task`/`state` fields. A waiter checks `is_cpu_preempted()` on its immediate predecessor's CPU during its wait; on unhealthy-predecessor detection, sleep via plain `schedule()` and wake via plain `wake_up_process()` — reusing PV spinlock's *shape* (predecessor tracking, sleep/wake instead of pure spin) without its *mechanism* (no hypercall anywhere in this path). This is less new code than either the original from-scratch design or a full PV route, because the native queueing/handoff/ordering logic Layer 2 builds on is already implemented and tested in-tree.

**Sequencing within Stage B's post-boot flip order (below): Layer 1 flips and soaks fully before Layer 2 is even attempted, and Layer 2 is itself a separate, later flip from Layer 1 — do not flip both in the same soak window.**

**Freeze-precedent risk for B6 specifically:** this is the highest-risk single item in this plan, because it changes which code path 100% of contended kernel-spinlock acquisitions take on this VM. The project's own history contains a real, root-caused freeze from exactly this class of interaction (a migrated confirmed holder landing on a CPU that's uninterruptibly spinning with `preempt_count()==1` and no yield — see the `ivh_spin_yield_enabled` comment history at `qspinlock.c:112-153`). That specific fix stays in place and untouched through all of B6 — see Stage C below for why removing it now would be actively dangerous.

### Why one rebuild batch is safe (explicit argument)

Every behavioral delta above is behind its own new, default-off sysctl. B1 and B7 are pure instrumentation (inert until read). B2 preserves the current formula bit-for-bit at `ivh_time_left_v2=0`. Therefore the freshly booted Stage B kernel, at all-defaults, is semantically identical to the currently-running kernel — the reboot itself carries zero behavioral risk, and every mechanism still gets its own real A/B comparison *at runtime*, one sysctl flip at a time, not conflated by being bundled into one boot. The reboot cost is amortized across independently-developed, independently-reviewed changes; the risk is not.

### Post-boot validation sequence (stop at first anomaly; keep an out-of-band `virsh`/second-SSH session open throughout — this project's documented failure mode is a full VM freeze, and the detection plan is literally "watch from outside")

1. Rebuild `vsched_module.ko` for the new `uname -r` (explicit version string — `install_module.sh`'s no-arg default silently targets whatever `include/generated/utsrelease.h` says, which can point at the wrong kernel), `insmod`, start exactly one `vcap` and one `MY_ivh_atc`. Confirm `/proc/vcap_info`, `/proc/vcap_page`, `/proc/ivh_debug` are all live.
2. **Boot-equivalence check:** all new sysctls at default (0), 3 interleaved hackbench + NHextend rounds vs. the pre-reboot Stage A baseline. Pass = statistically flat — proves the reboot itself changed nothing.
3. Flip **exactly one** new sysctl at a time. After each flip: run its targeted eval (below), snapshot `/proc/ivh_debug`, and run a 60s stability soak under the freeze-repro-shaped load (`tools/bpf/scripts/demo_freeze_repro.sh` is the existing canary template for this). Do not flip the next sysctl until the current one has soaked clean.
4. **Flip order** (cheapest/lowest-risk first, highest-risk last, matching this plan's risk gradient):
   - B1 (trace split only — zero behavioral risk, unblocks re-scoring).
   - B2 (rerun Stage A3's offline scorer against the now-complete trace line; pass = v2 precision ≥ v1 at equal recall, and the `time_left_reject` counter now visibly moves on workloads where it was previously provably inert).
   - B5 (osq swap — low risk, isolated to osq_lock's wait loop; paired runs under Stage A4's asymmetric setup; B7's new counters must show aborts correlating with sampler-observed steal, near-zero without steal).
   - B6 Layer 1 (native-path fall-through) — its own full soak window before Layer 2 is attempted.
   - B6 Layer 2 (predecessor health check) — separate soak window from Layer 1.
   - B3, if built at all.

**Stage B risk notes, per item:**
- **B2** failure mode: Gate 2 rejects too much (IVH stops firing) — visible immediately as `ivh_steal_imminent_time_left_reject` dominating while `ivh_migrations_done` stays flat. Recovery: sysctl back to 0, no reboot needed.
- **B5** failure mode: over-eager wait abandonment causing spurious osq requeue churn — the paired-run control specifically includes a no-steal baseline to catch a throughput regression that isn't steal-related.
- **B6** failure mode (the one that matters most): a repeat of the documented freeze class, now via the native path instead of the TAS path. This is why B6 gets flip-one-layer-at-a-time treatment and its own soak windows, and why the existing TAS-loop yield fix is explicitly retained (not removed) through all of Stage B — see Stage C.

---

## Stage C — remove the TAS-loop yield fix (final milestone, deliberately late)

**Do not schedule this early, and do not bundle it into Stage B.**

`ivh_virt_spin_lock()`'s existing `ivh_spin_yield_enabled` sysctl (added 2026-07-06, `qspinlock.c:112-153`) fixes a specific, already-confirmed, root-caused VM freeze: a migrated confirmed lock-holder landing on a CPU where another thread is spinning via the TAS path with `preempt_count()==1` and no yield point, permanently starving the holder (priority-blind veto on preemption under every preemption model — the holder can never run, the spinner can never stop, and nothing else on that CPU can intervene). This fix stays in place, unmodified, through the entirety of Stage B, including B6's Layer 1 and Layer 2 — because until B6 has actually flipped to being the real default for contended locks, the TAS path this yield protects is still 100% of what runs, and removing the yield now would re-expose a freeze bug that is still live.

**Preconditions to even schedule this milestone:**
1. B6 Layer 1 (native fall-through) has landed and been individually soak-tested per Stage B's sequence.
2. B6 Layer 2 (predecessor health check) has landed and been individually soak-tested.
3. The `ivh_qspin_native_fallthrough` sysctl (or its successor) has been flipped to **default 1** — i.e., the native MCS path, not the TAS path, is now what real contended qspinlock acquisitions on this VM actually take.
4. That new default has itself been soak-tested under the freeze-repro-shaped load for a meaningfully longer window than the per-flip soaks above (this is the change that retires a known freeze fix — the bar for "confident" here is deliberately higher than for any single sysctl flip).

**Once all four hold:** remove `ivh_spin_yield_enabled` and its TAS-loop yield logic as dead code (the TAS path itself may still exist as a fallback for configs without the native path reachable, but the yield workaround specifically is no longer protecting anything once TAS is no longer the common case). This is a small, mechanical change at that point — a deletion, not a design task — but it still touches core kernel locking code and therefore rides its own (much smaller) rebuild+reboot cycle, kept deliberately separate from Stage B so that Stage B's own soak results are never confounded by simultaneously removing a safety net.

**Risk if done early (why this ordering is load-bearing, not just tidy):** removing the yield before B6's native path is the actual default re-exposes exactly the freeze class this fix was built to close, while B6 itself is still new and unproven — the worst possible combination (untested new path, no safety net for the old path it's supposed to be replacing). Removing it only after Stage C's four preconditions hold means the safety net is retired only once it has been made structurally unnecessary, not just optimistically assumed to be unnecessary.

---

## Summary timeline

```
Stage A0  housekeeping (commit tree, log dir, restore runtime)         [~30 min]
Stage A1  shared health page (module)                                   [module-only]
Checkpoint H  Hotlock observation validation -- HARD GATE                [module/BPF/userspace-only]
Stage A3  time-left measurement + offline fix validation                [module/BPF/userspace-only]
Stage A4  adaptive-spinning NHextend prototype                          [userspace-only]
Stage A5  (optional) BPF abstain toggle                                 [BPF-only]
   |
   v  (single rebuild + reboot)
Stage B   time-left fix (B1/B2[/B3]), osq swap (B5), qspinlock          [ONE reboot]
          native fall-through Layers 1+2 (B6), all sysctl-gated,
          flipped one at a time with soak windows between each
   |
   v  (much later, separate small rebuild + reboot, gated on B6
   |   becoming the proven default)
Stage C   remove the now-obsolete TAS-loop yield fix                    [second, smaller reboot]
```

---

## Critical files for implementation

- `/home/nick/kernels/linux-6.17-rseqport/custom_modules/vsched_module.c` — Stage A1 shared health page (substrate for Checkpoint H, A3, A4)
- `/home/nick/kernels/linux-6.17-rseqport/NHextend.c` — Checkpoint H's userspace shim, Stage A4's prototype, most of the evaluation harness patterns this plan reuses
- `/home/nick/kernels/linux-6.17-rseqport/kernel/sched/fair.c` — `ivh_steal_imminent()` fix (B2), trace split (B1), `/proc/ivh_debug` counters (B7)
- `/home/nick/kernels/linux-6.17-rseqport/kernel/sched/bpf_sched.c` — `sys_ivh_cs_enter`, every new Stage B sysctl declaration
- `/home/nick/kernels/linux-6.17-rseqport/kernel/locking/qspinlock.c` and `/home/nick/kernels/linux-6.17-rseqport/kernel/locking/qspinlock.h` — B6's fall-through mechanism (Layer 1) and `struct qnode` extension (Layer 2); also where the `ivh_spin_yield_enabled` code being retired in Stage C lives
- `/home/nick/kernels/linux-6.17-rseqport/kernel/locking/osq_lock.c` — B5's one-line swap
- `/home/nick/kernels/linux-6.17-rseqport/tools/bpf/MY_ivh_atc.bpf.c` — Stage A5's abstain toggle; reference pattern for Checkpoint H's observation-only BPF tool

---

## Addendum (2026-07-13 early AM) — SUPERSEDED, see 2026-07-13 evening entry below

The section that used to be here concluded "no further fix needed, invariant holds" based on an IVH-only vs. IVH+adaptive comparison that was run **without `ivh_exec`** — meaning `PF_IVH_ELIGIBLE` was never set and neither arm was actually exercising IVH at all. That bug was caught later the same day (see below) and invalidates every number and conclusion in that section. Left the history out of the doc; treat this whole addendum as void.

## Addendum (2026-07-13 evening) — real root cause found: two sysctl defaults were wrong, plus a from-scratch kernel rebuild

Long day. Summary of what actually happened, in order:

1. **Methodology bug found and fixed.** All "IVH-only" comparisons up to this point had been run as plain `sudo ./NHextend`, not `sudo ./ivh_exec ./NHextend` — `PF_IVH_ELIGIBLE` (gates both `sys_ivh_cs_enter` and the kernel `ivh_pre_lock()` path) was never set, so every prior "IVH vs no-opt" comparison this session was actually no-op vs no-op. Corrected: `ivh_exec` is required for any IVH-enabled run from here on.

2. **Fable found and fixed two real bugs in Stage A4's adaptive-spinning prototype** (`NHextend.c`): (a) the per-wait health snapshot wasn't tagged to the CPU it was taken for, so a lock handoff mid-wait compared two unrelated CPUs' cumulative steal counters — degenerate, near-random backoff decisions (~65-70k backoffs/5s observed vs. ~1.7-3k expected; fixed via a `snapshot_cpu` tag, confirmed the backoff count collapsed to ~3k as predicted); (b) the lock word's advertised holder CPU (`my_lock_val`) was computed once before the (often long, unpinned) wait rather than fresh at each acquisition attempt, so it could go stale for an entire tenure — fixed by recomputing it inside the acquisition loop.

3. **Did a full kernel rebuild+reboot** (`84f1e5fcc` "rseqport34+" → "rseqport40+"): kept locus (a) (`bpf_sched_pre_lock_migrate()`) unchanged, kept the qspinlock freeze-yield fix, kept Hotlock in shadow/observation mode (`ivh_post_lock()` samples the table on every real spinlock acquisition but no longer dispatches any real migration), removed locus (c) (`bpf_sched_post_lock_migrate()`) entirely along with everything that existed only to serve it, and added a minimal `ivh_get_vcpu_preempted()` export in `core.c` for Stage A1's future module-only mmap page. Booted clean.

4. **Discovered the IVH pipeline was silently 100% inert after reboot**, on both the new kernel and (when briefly rebooting back to confirm) the old one: the BPF loader (`MY_ivh_atc`) and the capacity daemon (`vcap`, feeds `/proc/vcapacity_write` → `rq->cpu_capacity_custom`) don't survive a reboot and were never restarted, so `bpf_sched_enabled()` was false and/or Gate 1 (`rq->cpu_capacity > ivh_capacity_threshold`) rejected 100% of evaluations. **Fix: `/home/nick/IVH` is the canonical activation script** (`setup.sh` + `vcap &` + `MY_ivh_atc &`) — run it after every reboot, before trusting any IVH measurement. (Killed any stray `MY_ivh_atc`/`vcap` first, per the existing duplicate-instance protocol, before relaunching.)

5. **Even with the pipeline confirmed genuinely active** (real `ivh_migrations_done` counting up, real gate rejections), IVH-only measured *worse* than no-opt across many rounds, on both kernels, at both the old (512) and previously-"good" (1010) `ivh_capacity_threshold` values. Dispatched Fable with the full picture (real migration latency, veto stats, gate-chain code); ruled out (via cheap, zero/low-code experiments) both "incidental kernel-lock-triggered migrations" and "migrations landing mid-CS" as the dominant cause. Root cause looked structural (stale target-health signal under volatile load) until —

6. **The user found the actual fix by testing on an old VM: `ivh_time_left_threshold_ns` also needed to change, not just `ivh_capacity_threshold`.** The compiled defaults (`ivh_time_left_threshold_ns=500000` i.e. 500µs, `ivh_capacity_threshold=512`) were themselves the problem — too tight to admit real, worthwhile migrations, so IVH was paying migration overhead for little/no real benefit. Setting **`ivh_time_left_threshold_ns=4000000`** (4ms) **and `ivh_capacity_threshold=1010`** together immediately restored a large, real, reproducible benefit:
   - Under a deliberate all-16-core sysbench stress test (extreme host contention, confirmed via `mpstat` at 30-50% real steal, independent of any of this project's own instrumentation): no-opt 29.77%/24.50% host-preempted → IVH-only 14.09%/3.77% (roughly 2-6.5x reduction).
   - Back to sysbench's normal asymmetric 8-core pattern, at `loop_spin=350000` (the historical short-CS baseline): no-opt ~4.7-5.15% → IVH-only 0.76-4.57% across 4 rounds — consistently better, though noisier than the stress-test case.
   - **Verified via per-CPU `/proc/vcap_info` deltas that the steal-time ground truth correctly attributes contention to only the specific CPUs sysbench is actually hitting** (~150-300x higher accumulation rate on the busy cores vs. idle ones) — the measurement mechanism itself is trustworthy.

7. **IVH+adaptive-spinning on top of IVH-only, at `loop_spin=350000`, is genuinely inconclusive right now** — 4 interleaved rounds: 0.16%, 3.37%, 2.23%, 3.78% (vs. IVH-only's 3.97%, 1.21%, 4.57%, 0.76% same rounds). Averages land close (adaptive 2.39% vs. IVH-only 2.63%), but the round-to-round swing is large in both directions. Given how much host-load volatility this whole session has demonstrated (steal has been observed swinging from ~0.03% to ~50% within minutes on this same box), the working hypothesis is that this noise is mostly load bleeding through a short (10s) sample window rather than a real property of adaptive spinning — but this is not yet confirmed, and Fable's two Stage-A4 bug fixes (item 2 above) landed at the same time as everything else, so a clean re-read specifically of adaptive-spinning's marginal benefit (not just IVH-only's) hasn't happened yet under the corrected thresholds.

**Bottom line: IVH's core benefit is real, confirmed, and intact.** Nothing was broken by the kernel rebuild — the exact same "IVH-only worse than no-opt" symptom reproduced on the untouched old kernel too, which is what pointed away from the rebuild and toward the sysctl defaults. The two corrected thresholds (`ivh_time_left_threshold_ns=4000000`, `ivh_capacity_threshold=1010`) should be treated as the new working defaults for all further testing, and are strong candidates to become the compiled defaults in `bpf_sched.c` once confirmed stable across more sessions/conditions.

### Updated next-steps checklist

- [x] Fix `PF_IVH_ELIGIBLE`/`ivh_exec` methodology bug.
- [x] Fix Stage A4's snapshot-CPU-tagging bug and stale-`my_lock_val` bug (Fable, `NHextend.c`).
- [x] Clean kernel rebuild: pre-lock gate chain ported forward unchanged, freeze-yield kept (runtime-toggleable, default off), Hotlock kept in shadow/observation mode, post-lock removed entirely, Stage A1's kernel-side export (`ivh_get_vcpu_preempted()`) added. Booted and confirmed (`6.17.0-rseqport40+`).
- [x] Root-caused and fixed the "IVH-only worse than no-opt" regression: two sysctl defaults (`ivh_time_left_threshold_ns`, `ivh_capacity_threshold`) were wrong, not a kernel or code regression. Confirmed fix across multiple load conditions (extreme stress test and normal asymmetric contention).
- [ ] **Re-run the IVH-only vs. IVH+adaptive-spinning comparison** (`loop_spin=350000` and other values) specifically with the corrected thresholds, enough rounds (6-10+) to get past the current noise, ideally logging `uptime`/a load snapshot alongside each round so load-driven variance can be distinguished from real signal after the fact.
- [ ] **Promote the two corrected thresholds** (`ivh_time_left_threshold_ns=4000000`, `ivh_capacity_threshold=1010`) into `/home/nick/IVH` (or a new setup step) so every future session starts from the known-good values instead of the compiled defaults, and consider changing the compiled defaults in `bpf_sched.c` once these are confirmed stable.
- [ ] Re-run the original `ivh_capacity_threshold` sweep (512/700/850/950/1010) now paired with the corrected `ivh_time_left_threshold_ns=4000000`, since the earlier sweep (and tonight's re-tests) were both done with only one of the two knobs corrected at a time — the two may interact.
- [ ] Stage A1 (`vsched_module.c` shared mmap health page + live-preempted-bit consumer, using the new `ivh_get_vcpu_preempted()` export) — still not started; module-only, no further kernel rebuild needed for this specific piece.
- [ ] Stage A3 sampler groundwork — still not started, not blocking anything else.
- [ ] Checkpoint H's kernel-lock-side half (hackbench, daemons touching `ivh_hotlock_note_waiter_enter()`) — now unblocked (Hotlock is live in the booted kernel, shadow mode via `ivh_post_lock_enabled=1`), not yet exercised.
- [ ] Remember: after any future reboot, run `/home/nick/IVH` and re-set the two corrected sysctls before trusting any measurement — both are silent-failure modes (pipeline looks like it's running, numbers just quietly look like no-opt).
</content>
