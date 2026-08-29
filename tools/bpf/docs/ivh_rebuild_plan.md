# IVH incremental rebuild plan — from vanilla 6.17 to full IVH + adaptive spinning

**Written:** 2026-08-25, overnight session, autonomous (user asleep). **Author:** Claude (this
session), synthesizing two full doc-audit sub-agents plus this session's own live investigation
of a real ~15-24% mmap/rwsem-contention throughput regression between the custom IVH kernel and
a byte-for-byte vanilla v6.17 built from the same `.config`.

**Purpose of this doc**: (1) a precise, sourced map of what in this project is real/shipped
versus abandoned experiment ("fluff"), (2) an incremental, buildable, git-trackable sequence
from pure vanilla 6.17 up to full production IVH, so each mechanism's cost can eventually be
measured in isolation, and (3) enough context that a **fresh Claude session with zero memory of
tonight** can pick this up and continue — that's the primary intended reader.

**What was and wasn't possible tonight**: this session could not reboot the machine (no one was
available to do it). So this doc plus the constructed git history/patches is the deliverable —
Step 0 is built, compile-verified, and its "accepted numbers" measured live. Steps 1+ are
specified precisely enough to build and boot-test, but were not booted by this session. See
"Status" markers on each step.

---

## 0. tl;dr for a fresh session

- Two kernels exist side by side on this machine: `/root/linux-6.17` (the custom, patched IVH
  tree, currently checked out as commit `6859e40fb` on branch `kernel-43-clean`, pushed to
  `github.com/nhatz11/linux-6.17`) and `/root/kernels/linux-6.17-vanilla` (a fresh shallow clone
  of upstream `torvalds/linux` tag `v6.17`, git identity `nhatz11`/`nickohatz@gmail.com`
  configured locally).
- `/root/kernels/linux-6.17-vanilla` now also has a branch `ivh-rebuild-main` with two commits so
  far: Step 0, tag `ivh-step-0-vanilla-base` (vanilla source + IVH's exact `.config`, byte-identical
  to the custom kernel's `.config` except `CONFIG_LOCALVERSION`), and Step 1, tag `6.17-GLOCK-1`
  (inert `BPF_PROG_TYPE_SCHED` infra, compile-verified, boot-and-benchmark still pending — see §4).
  Step tags from here on follow the `6.17-GLOCK-N` naming (not `ivh-step-N-...`, which was only
  used for Step 0 before this naming was set).
- The accepted baseline numbers (Section 3) were measured on that exact config, live, tonight,
  under **no corunner** (user turned sysbench off), preempt mode **voluntary**
  (`echo voluntary | sudo tee /sys/kernel/debug/sched/preempt`), kernel `6.17.0-vanilla617`.
- The four validation benchmarks, exact invocations, are in Section 3.1. Use these exact
  commands for every step — do not vary flags between steps, that's how this session lost hours
  to false leads earlier tonight.
- The real, if partial, answer to "why is custom IVH slower than vanilla" is already known from
  tonight's investigation and is **not** a single smoking gun — see Section 5.

---

## 1. What's actually real vs artifact — full classification

This section is the synthesis of two sub-agents that read all 31 dated docs in
`tools/bpf/docs/` (in the **custom** tree, `/root/linux-6.17/tools/bpf/docs/`) plus this
session's own direct source verification. Every claim below was cross-checked against live
source, not just relayed from a doc. File:line references are to the custom tree
(`/root/linux-6.17`) unless stated otherwise.

### 1.1 The foundational layer: BPF "sched hook" program type

This is **infrastructure everything else sits on**, and it predates IVH specifically — the
naming (`vsched`) suggests an earlier, more general framework IVH was built on top of.

- New BPF program type `BPF_PROG_TYPE_SCHED` (`include/uapi/linux/bpf.h`, `include/linux/
  bpf_types.h`), with trampoline support added to `kernel/bpf/{btf,syscall,trampoline,
  verifier}.c` (23/46/11/22 changed lines respectively — real but modest).
- `include/linux/sched_hook_defs.h` (new file) declares **14** hook points via a
  `BPF_SCHED_HOOK(...)` macro: `cfs_check_preempt_tick`, `cfs_check_preempt_wakeup`,
  `cfs_wakeup_preempt_entity`, `cfs_vcpu_capacity`, `cfs_sched_tick_end`,
  `cfs_get_polling_amount`, `cfs_select_run_cpu_spin`, `cfs_select_run_cpu`,
  `cfs_should_spinlock`, `cfs_latency_select`, `cfs_should_bias`, `cfs_latency_profile`,
  `cfs_correct_migration`, `cfs_spin_len`.
- **Verified live tonight (grep for `bpf_sched_<hook>(` call sites in `kernel/sched/*.c`,
  excluding the macro-expansion site itself): only 2 of the 14 are ever called from anywhere in
  the kernel.** `cfs_select_run_cpu_spin` (from inside `bpf_sched_pre_lock_migrate()`,
  `fair.c:13707` — this is the literal trampoline call into the loaded `MY_ivh_atc` BPF
  program's `process_cpu()` logic) and `cfs_spin_len` (`core.c:4317`, adaptive-spin duration
  tuning). **The other 12 are declared, exported, loaded by the BPF loader, verified, and
  permanently idle** — confirmed independently in-tree at `core.c:1755-1790`'s own audit
  comment: `cfs_latency_select` is "CALLED FROM NOWHERE IN THE KERNEL... loaded, verified, and
  permanently idle."
- `bpf_sched_pre_lock_migrate()` itself (`fair.c:13551`, exported `fair.c:13870`) is a
  **directly-coded C function**, not one of the 14 generic hooks — it's the gate/locking logic
  (`ivh_selection_trylock`, `my_spinlock`, `PRMPT_HELD_MASK`) that wraps the one real call into
  `cfs_select_run_cpu_spin`.

**Rebuild implication**: Step 1 should port the generic BPF_PROG_TYPE_SCHED infrastructure plus
all 14 hook *declarations* (for compile-compatibility with the real `MY_ivh_atc.bpf.c`, which
references the type even if most hooks are unused), but does not need any kernel-side call site
except the two real ones, which naturally arrive with Step 6 (the migration engine).

### 1.2 Lock-holder-identity tracking (qspinlock fast path)

`ivh_lock_set_holder()`/`ivh_lock_clear_holder()` (`include/linux/ivh_lock_holder.h`), compiled
into `queued_spin_lock()`/`queued_spin_unlock()` directly (`include/asm-generic/qspinlock.h`,
`arch/x86/include/asm/qspinlock.h` — 6 call sites total, confirmed earlier this session).
Gated at runtime by `ivh_lock_holder_enabled` (default **0**), which — confirmed by direct
`READ_ONCE` check inside the hook functions themselves in `include/linux/ivh_lock_holder.h` —
makes the cost "one `READ_ONCE` of a read-mostly global and one perfectly-predicted branch"
when off. **Not in `IVH_start.sh`'s sysctl set at all — stays at its compiled default of 0 in
production.** Real, load-bearing when enabled, genuinely near-zero when off (verified via this
session's own `wait_depth`-style dead-code hunt — this one has a real, if currently-inert,
purpose unlike `wait_depth`).

### 1.3 CS-hold timing (`cs_enter()`/`cs_exit()`)

`kernel/locking/spinlock.c`. Wraps every outermost `raw_spinlock_t` acquire/release kernel-wide.
Gated by `ivh_cs_track_enabled` (**this session's own addition**, not upstream IVH — added
tonight as a diagnostic 0/1/2/3 mode to isolate TSC-read cost from bookkeeping cost; the
original code had no such gate and always ran). **Confirmed real cost tonight**: two genuine
out-of-line `sched_clock()` calls per outermost critical section (objdump-verified against
vanilla's bare 11-instruction fast path), but **A/B tested multiple times tonight and found
insufficient alone to explain the full regression** — disabling it entirely closed roughly
10-17% of the gap, not all of it.

Two dead-code items were found and removed here tonight, confirmed zero live consumers (in
kernel and in the `MY_ivh_atc.bpf.c` `GATE_SPINNER` block, which is `#if 0`'d out and explicitly
documented as "measured WORSE on every axis... reverted to 0"):
- `cumulative_cs_time`/`cumulative_active_time` — dead task_struct accumulation, removed.
- `wait_depth` bookkeeping in `qspinlock.c`/`osq_lock.c` — dead, removed. (One near-miss: this
  field IS read by `MY_ivh_atc.bpf.c:595`, but only inside the `#if GATE_SPINNER` block, which
  is compiled out. Verify this stays true if `GATE_SPINNER` is ever flipped back on.)

`ivh_scan_stuck_waiters()` (`kernel/sched/fair.c`, called unconditionally from `sched_tick()`)
was also found and disabled tonight — an ungated diagnostic tracer that hammered a single
globally-shared raw spinlock 32x/tick/CPU (~512K acquisitions/sec across 16 vCPUs) for zero
information whenever no migration was in flight. User confirmed not used recently; call site
commented out rather than gated (see `core.c`, search "Disabled 2026-08-25").

### 1.4 PV wait/kick substitution — full detail from Agent A's audit

**Production values** (`cvm_setup/IVH_start.sh`): `ivh_pv_wait_mechanism=2`,
`ivh_pv_kick_pure_ipi=1`, `ivh_pv_preempt_src=2`.

Dependency chain, in build order:

1. **Scaffolding** (`arch/x86/kernel/kvm.c`): `kvm_spinlock_init()` (~line 2610) registers
   `pv_ops.lock.{queued_spin_lock_slowpath,queued_spin_unlock,wait,kick}` unconditionally
   whenever PV spinlocks are on and `KVM_HINTS_REALTIME` is absent — a deliberate deviation from
   upstream (which bails without `KVM_FEATURE_PV_UNHALT`). `ivh_pv_wait_mechanism` has its own
   sysctl table (`ivh_pv_sysctls[]`, `late_initcall`), separate from `bpf_sched.c`'s table, so
   it works even without `CONFIG_BPF_SYSCALL`.

2. **Mechanism 2 itself** (`ivh_pv_wait()`, `kvm.c:~2207`): "scoped halt + IPI wake." Full
   control flow documented in Agent A's report (reproduced verbatim in the appendix below).
   **Safety-critical detail, do not simplify**: mechanism 2 halts *only* on the
   `!irqs_disabled()` path; the IRQs-already-disabled case falls through to a bounded TPAUSE
   poll (`IVH_PV_ADAPTIVE_TSC`, ~1ms deadline). An earlier version halted unconditionally and
   caused the project's one confirmed hard freeze (2026-07-24) — a HLT taken with
   `RFLAGS.IF=0` cannot be un-halted by a maskable IPI, only by `KVM_HC_KICK_CPU`'s
   `pv_unhalted`. Full writeup in-tree at `kvm.c:2345-2412`.

3. **Two-stage IPI wake**: stage 1 at MCS handoff (`qspinlock_paravirt.h`, `pv_kick_node()`,
   after the `VCPU_HALTED→VCPU_HASHED` cmpxchg); stage 2 at release (`ivh_pv_kick()`,
   `kvm.c:~2492`). `ivh_pv_kick_pure_ipi=1` makes the kick 100% APIC IPI, zero hypercalls.
   Needs `#include <linux/smp.h>` + `#include <trace/events/ipi.h>` in **both** `kvm.c` and
   `qspinlock.c` (the latter because `qspinlock_paravirt.h` is textually included into it) —
   a real build-mechanics gotcha hit once already.

4. **Cross-knob safety validators** (`kvm.c:1706-2026`): reject `mechanism=0` + `pure_ipi=1`
   (and the reverse) in either order, atomically, so a concurrent `ivh_pv_kick()` never observes
   the unsafe combination transiently. **Mandatory** — these prevent a whole-VM freeze class,
   not optional hardening.

5. **Per-CPU TSC heartbeat** (`arch/x86/include/asm/ivh_tsc_beat.h` — must stay a separate
   header from `qspinlock.h`, `asm/tsc.h`'s include chain collides with the generic
   `vcpu_is_preempted(int)` fallback otherwise). `is_wait_preempted()`
   (`qspinlock_paravirt.h:338`) at `src==2` returns the heartbeat-derived `beat` value —
   **this session's own fix, added today**, removes the last unconditional
   `vcpu_is_preempted()` paravirt read from the wait path. Six publish sites (tick, queue-entry
   seed, two spin loops, three halt exits) — see Agent A's report for exact locations if
   rebuilding this from scratch.

   **Important open finding from tonight's doc audit, not previously known to this session's
   own investigation**: the shipped `ivh_pv_beat_threshold=3300000` (1.5ms) is measured
   **near-blind** — 0.46% sensitivity in one 5.1×10⁸-sample characterization; the real knee is
   around 131072 (~60µs). At production settings, the scoped-halt gate in `pv_wait_node()`
   fires almost entirely on the stock `prev->state != VCPU_RUNNING` term, not the TSC term.
   Worth a real sweep before claiming the heartbeat mechanism is doing real work.

6. **Gate 2's live PV dependency — the single most important open finding from tonight's audit,
   confirmed independently by both sub-agents and by this session's own source read.**
   `rq->last_preemption` / `rq->last_active_time` (consumed by the migration decision's Gate 2
   at the shipped `ivh_time_left_source=1`) are written **only** from real
   `paravirt_steal_clock()` in `steal_account_process_time()`
   (`kernel/sched/cputime.c:274-297`), **unconditional on `ivh_steal_source`**. The intended
   TSC-only replacements (`ivh_vact_last_preempt_tsc`/`ivh_vact_last_active_c`, requiring
   `ivh_preempt_event_source=2`) exist in code but **are never set by `IVH_start.sh`** — it
   leaves `ivh_preempt_event_source` at its default of 0.

   **Root cause, traced through the doc history (not just "it's unset"): this was a working,
   validated setting that fell out of the boot script when the investigation's focus moved on.**
   `ivh_preempt_event_source=2` was built and proven TSC-native by 2026-08-02/08-06
   (`ivh_tsc_final_state_report_2026-08-02.md`, `ivh_four_questions_report_2026-08-06.md` —
   ±1.6% error at idle, "current" in both sessions' live configs). By 2026-08-03 the
   vcap-retirement plan explicitly treats it as a settled precondition it doesn't need to
   re-touch: *"It does not touch Gate 2 (`ivh_preempt_event_source`), which is already TSC-native
   and working"* (`ivh_vcap_retirement_build_plan_2026-08-03.md:939`). From that point on, every
   later config audit (e.g. the Aug 8 `ivh_script_reproduction_audit_2026-08-08.md`, which
   line-by-line diffs the boot script against the day's validated 14-sysctl config) stops
   mentioning it at all — attention had shifted entirely to the Gate 1 / capacity / `uc` axis.
   Because `ivh_preempt_event_source` is a **runtime sysctl with no non-zero compiled default**
   (`kernel/sched/bpf_sched.c:320`, `unsigned long ivh_preempt_event_source = 0UL;`), it resets to
   0 on every reboot, and whoever last assembled `IVH_start.sh` evidently only listed the knobs
   actively being iterated on that day, not the "already decided, no longer touched" ones — so the
   line to re-set it never made it in. **This is a one-line regression, not an unsolved design
   problem**: the fix (`set_ivh_sysctl ivh_preempt_event_source 2` in `IVH_start.sh`) is already
   known and was already validated once, just needs re-confirming after the drift (the `is_cpu_
   preempted()` caveat in `ivh_four_questions_report_2026-08-06.md` sec 4.5 — `rq->clock_preempt`
   is fed by plain `sched_clock()` regardless, so it's unaffected either way).

   So: **the claim "no paravirt data enters any IVH decision" is currently false for the shipped
   config** — true for Gate 1 (capacity) and, as of today's fix, for consumer 1
   (`is_wait_preempted`'s heartbeat), but not for Gate 2's timing input, purely because of this
   script drift. If a genuinely PV-free build is the goal (which is explicitly what the user has
   been asking for across this session), **adding `ivh_preempt_event_source=2` to `IVH_start.sh`
   is a required, low-risk, previously-validated fix** — recommended as an immediate action
   independent of the rest of this rebuild plan.

### 1.5 Capacity + migration decision engine — full detail from Agent B's audit

**Production values**: `ivh_universal_eligible=1` (the sole migration gate, default 0 — omitting
this makes every other sysctl cosmetic, confirmed by a real historical incident), `ivh_cap_
source=3`, `ivh_steal_source=2`, `ivh_uc_used_source=0` (WALL, not ACCT — ACCT was tried,
measured better, then retracted the same day because it silently depends on PV via `kcpustat`).

The full dependency chain (see Agent B's report for exhaustive file:line detail, reproduced in
the appendix):

1. `nohz=off` (boot param, `/etc/default/grub`, already global/persistent on this machine —
   **confirmed active on vanilla too tonight**, `cat /proc/cmdline`) + `ivh_idle_ns()` kcpustat
   fallback — foundational, everything below depends on it.
2. `ivh_uc_capacity` — the in-kernel capacity signal replacing the old `vcap` daemon's own
   calculation. `struct rq` fields + `ivh_uc_tick()` (`core.c:2763`, called from
   `account_process_tick()`, `cputime.c:624`). WALL formula: `avail_c = elapsed - idle`,
   `used_c = avail_c - min(steal_c, avail_c)`, EMA in Q16 fixed point, **first window assigns
   rather than blends** (removes cold-start transient).
3. BPF `ivh_cfg` map (`MY_ivh_atc.bpf.c`) + `ivh_cap_of()` — source resolved once per scan,
   stashed in `task_ctx`, so a mid-scan sysctl flip can't produce a half-vcap/half-uc candidate
   set.
4. Reshaped, scale-free capacity gates (`IVH_CAP_TOPBAND=50`, `IVH_CAP_MARGIN=20`,
   `ivh_capacity_threshold=1010` — **these two must move together**, neither alone works,
   confirmed by ablation).
5. `ivh_steal_source=2` — the TSC tick-gap estimator, `ivh_tick_steal_accumulate()`
   (`core.c`, called from `cputime.c:587`). Monotonic-since-boot contract is load-bearing (every
   consumer deltas it; one backward step underflows a u64).
6. `ivh_tks_idle_sub=0` + `ivh_tks_phase_pct=100` — accuracy calibration, **validated
   throughput-neutral** (hackbench paired diff +0.013s, p≈0.73), brings measured accuracy from
   0.23-0.66x to 0.9999-1.14x of the real steal page across three regimes.
7. `vcap_probe -p 200 -s 200` (userspace) — a stripped probe-only daemon, computes/publishes
   nothing itself, exists purely to keep `ivh_uc_tick()` from going stale on idle vCPUs.
   **Load-bearing for throughput even with IVH fully off** — stopping it costs 41-60% throughput
   (holds physical cores against a corunner; without it vCPUs HLT and the host reclaims them).
8. Launch-script correctness requirements: `ivh_universal_eligible=1` explicitly set (silent
   no-op otherwise), `ivh_uc_min_steal_ns=500000` (non-default, pins destination population at
   1023), daemon `pkill` + poll before relaunch (never `sleep N` blindly), poll for the BPF map
   rather than assume it's ready.
9. EMA convergence precondition (not code, but real): ~130s half-life at idle publish cadence.
   Don't benchmark immediately after (re)starting IVH.

**One drift resolved, one still open**:
- `IVH_CAP_HARDFLOOR`: **RESOLVED 2026-08-26.** Tree has **700**; `IVH_start.sh` now asserts
  **700** too (was 850, which itself was a lowering from the docs' 880). User's call: 850/880
  were tuned on the prior non-confidential VM; under TDX the guest genuinely loses CPU time to
  in-VM memory encryption/decryption on every access, which the TSC-based steal/capacity pipeline
  reads exactly like host steal — so a CVM's vCPUs have a legitimately lower steady-state capacity
  floor than an equivalent non-confidential VM, and 850 was rejecting destinations that are
  healthy for this environment. 700 is now the intended, documented value for this host; both the
  tree and the script agree. (The prior guess that 700 was leftover sweep cruft was wrong — it's
  deliberate, environment-specific.)
- `vcap_probe -s 200`: **treated as settled 2026-08-26.** Earlier docs through 2026-08-09 used
  `-s 5000` (and one session separately measured an intermediate `-s 1000` as the *worst* config),
  but both predate the 2026-08-19 change and the reasoning behind it: `-s 5000` (5s between
  probes) was 26x longer than `ivh_uc_window_ns`'s 200ms window, so ~96% of window-close checks
  saw zero probe overlap and just extended instead of publishing — the actual mechanism behind
  the capacity collapse toward HARDFLOOR under load. `-s 200` matches the probe cadence to the
  window period (same as `-p 200`) and is corroborated by the script's own live comparison
  (persistent 30-50%+ slow-run rate before -> 0 slow runs across two 6-run batches after). Live
  script and tree already agree on `-s 200`; the `-s 5000`/`-s 1000` numbers in older docs are
  superseded, not contradicting evidence. No fresh independent A/B was re-run today — this is a
  documentation correction (stop flagging it as a live conflict), not a new measurement.

### 1.6 rseq extensions (userspace-kernel fast channel)

`include/{linux,uapi/linux}/rseq.h`, `kernel/rseq.c`, `kernel/entry/common.c`,
`kernel/sched/syscalls.c` (a `sched_yield()` hook gated on `current->rseq_sched_delay`, never
exercised by any of the 4 benchmarks). Publishes `RSEQ_SCHED_STATE_FLAG_IVH_DANGER` on
return-to-userspace so `NHextend3`'s `ivh_cs_enter_checked()` can skip the real migration
syscall when the local danger bit is clear. **Measured tonight (Agent-adjacent doc,
`ivh_syscall_skip_throughput_analysis`): removes 88-99.7% of syscalls but produces zero
throughput change** — it's a system-CPU-time optimization (-7.4% sys time), not a throughput
one. Needed for NHextend3 to run its intended code path at all, irrelevant to
hackbench/ebizzy/dbench (they don't use this rseq extension).

### 1.7 Complete artifact list — do not port these

From both agents' audits plus this session's own findings, everything confirmed dead, abandoned,
or explicitly-not-shipped:

| Item | Why dead |
|---|---|
| `ivh_pv_wait_mechanism=1` (TPAUSE) | Live-tested ~36% regression under real contention; TPAUSE structurally never vmexits, host never learns the vCPU is idle |
| Hot Threads (`ivh_hot_threads_enabled`, `ivh_hot_preempt_gate_enabled`) | Both default 0; live-tested regression when enabled (2%→12%+ host-preempted-CS) |
| `wait_depth` bookkeeping (qspinlock.c/osq_lock.c) | Zero live consumers (removed tonight) |
| `ivh_scan_stuck_waiters()` | Ungated global-lock diagnostic, user confirmed unused recently (disabled tonight) |
| `cumulative_cs_time`/`cumulative_active_time` | Zero live consumers (removed tonight) |
| 12 of 14 `sched_hook_defs.h` hooks | Never called from anywhere in the kernel (confirmed by grep + in-tree audit comment) |
| Original `vcap` daemon | Retired; `ivh_cap_source=0` now reads a flat 1024 on all vCPUs, not a working fallback |
| `IVH_CAP_FLOOR=850` (absolute gate) | Explicitly retired in-source, nothing reads it; replaced by the relative TOPBAND/MARGIN gates |
| Part C / `ivh_vact_capacity` (`ivh_cap_source=2`) | Attempted, measured regression, root-caused (scale compression), not shipped |
| `ivh_uc_used_source=1` (ACCT) | Selected on merit then retracted same day — secretly PV-dependent via kcpustat |
| `ivh_capacity_threshold=965` | Superseded by 1010 (paired with MARGIN=20) |
| `IVH_CAP_MARGIN=50` | Superseded by 20 |
| `IVH_CAP_MARGIN_REL` (relative margin) | Compiled out; measured to not reliably determine outcome vs starting state |
| `ivh_uc_gate_recalibration §8` steal/elapsed publish | Never implemented; later shown to be actively wrong for WALL (steal term is common-mode/inverted there) |
| Deadband as an accuracy lever | Ruled out 3 separate times; inert by construction at phase_pct=0, and no real threshold separates the distributions when phase_pct=100 |
| `ivh_ka_*` in-kernel idle keepalive | Fully built, default 0, never enabled; doc's own text says "do not ship on this evidence" |
| `ivh_ref_method` exit-overhead deadband | Corrects `ivh_steal_source=1` only, which production doesn't use |
| `ivh_uc_avgcap_enabled` | Feeds `average_capacity_all`, one of the 12 dead hook consumers |
| `ivh_uc_shadow`/`ivh_decision_shadow` | Diagnostic only; shadow comparator now compares against a constant (flat 1024) since vcap retirement |
| CS-stamp + holder-identity table (Build 1 of 07-29 plan) | Fully wired, large (9 stamp + 5 clear sites), but default-OFF, never enabled in production, predicate has a measured hard ceiling of 78.57% sensitivity |
| NHextend4-tuned sysctls (`ivh_eval_cooldown_ns=1000000` etc.) | +9.8% NHextend but ~+23% hackbench regression (evacuation-based mechanism, cooldown is a rate limiter on it) — deliberately not shipped |
| `bpf_schedCOPY.c` | Not referenced in `kernel/sched/Makefile`, never compiled, pure stray file |

---

## 2. Open issues to resolve before/during the rebuild

1. ~~`IVH_CAP_HARDFLOOR`: 700 vs 850.~~ **RESOLVED 2026-08-26**: 700 is correct for this CVM
   (TDX encryption/decryption overhead legitimately lowers steady-state capacity vs the old
   non-confidential VM 850/880 tuning); script now asserts 700, matching the tree. See §1.5.
2. ~~`vcap_probe -s 200` vs `-s 5000`~~ **RESOLVED (documentation) 2026-08-26**: `-s 200` is the
   settled, intended value — it matches probe cadence to `ivh_uc_window_ns`'s 200ms window, unlike
   the older `-s 5000`/`-s 1000` values which predate this reasoning. See §1.5.
3. **Gate 2's live PV dependency** (`ivh_preempt_event_source=0` in production, should be `2` if
   the goal is genuinely PV-free — a one-line script fix, previously validated and working as of
   2026-08-02/06, then simply dropped from `IVH_start.sh` when attention moved to the capacity
   axis around 2026-08-03; see §1.4 item 6 for the full traced history. Low risk, recommend
   applying and re-confirming independent of the rest of the rebuild plan).
4. **The unexplained bistability** (`ivh_wall_path_calibration_2026-08-09.md §3.2`): one boot's
   destination population plateaued at 1000-1015 instead of the usual 1022-1023, for 2 hours,
   cause unknown. Confidence: low. Nothing in the doc set fixes this.
5. **This session's own open question**: the ~15-24% mmap/rwsem-contention throughput gap
   between custom and vanilla kernels (Section 5) — not fully explained by anything found
   tonight, including everything in this doc.

---

## 3. Accepted baseline — vanilla 6.17 + IVH's exact `.config`

Measured live tonight, kernel `6.17.0-vanilla617`, preempt=voluntary, **no corunner** (user
turned sysbench off for this exercise), `nohz=off` confirmed active via `/proc/cmdline`.
Justification for treating this as "accepted": this session independently confirmed earlier
tonight that vanilla v6.17 and stock Ubuntu 6.14 (the pre-existing, long-trusted reference
kernel) produce statistically indistinguishable numbers on the same 4MB ebizzy metric — so
vanilla 6.17's own fresh numbers are a legitimate zero-point, not an assumption.

### 3.1 Exact invocations (use these verbatim for every step, do not vary)

```bash
# preempt mode, set EXPLICITLY once per boot before testing -- do not skip this
# and do not trust a bare `cat` of the file to tell you the current mode without
# reading carefully: /sys/kernel/debug/sched/preempt marks the ACTIVE mode with
# PARENTHESES, not brackets, e.g. `none (voluntary) full lazy`. Both "voluntary"
# and "lazy" appear as plain words in the string either way, so it is very easy
# to see "voluntary" in the text and assume it's active when "lazy" (the
# compiled-in default on a fresh boot, confirmed 2026-08-26) is actually the one
# in parens. This exact mistake produced an initially-alarming 15% NHextend3
# discrepancy between a Step 0 and Step 1 run before being caught -- see 3.2.1.
# Always WRITE voluntary explicitly, every boot, before trusting any number.
echo voluntary | sudo tee /sys/kernel/debug/sched/preempt

# NHextend3 -- the metric is the "Ran for N times" line (contention-loop
# iteration count), NOT any of the wait/CS-time stats also printed. Extract with:
#   /root/linux-6.17/NHextend3 -n 16 2>&1 | grep -oP '^Ran for \K[0-9]+'
# Standardized 2026-08-26 after the original baseline-collection script (which
# used some other, now-unrecoverable extraction -- it was in a since-cleaned
# scratchpad) turned out not to match this field; see 3.2's note.
/root/linux-6.17/NHextend3 -n 16

# hackbench
hackbench -T -g 1 -f 8 -l 400000

# dbench
dbench -F -t 12 16 -D /root/dbench_test

# ebizzy, mmap mode, 4MB chunks — THE ONLY ebizzy variant tracked going forward
# (decision 2026-08-27: dropped the 512KB malloc/mmap variants from the accepted
# suite -- they were noisy and redundant once 4MB was established as the clean
# metric; see run_baseline.sh, which now only runs this one)
/home/nick/Desktop/ebizzy -S 20 -t 16 -m -s 4194304
```

### 3.2 Numbers (SUPERSEDED for NHextend3/hackbench/dbench/ebizzy-4MB by §3.3, 2026-08-27 —
this table is now a historical record of the pre-host-restart host state; ebizzy malloc-512KB and
mmap-512KB were not re-measured on 2026-08-27 and remain the current reference for those two rows
only. Full suite originally completed 2026-08-26 07:40:32, kernel `6.17.0-vanilla617`, no corunner.
Raw log was at
`/tmp/claude-0/-root-linux-6-17/b98a4d93-d606-4bb7-bd13-7031a5eea896/scratchpad/baseline_vanilla617.log`
— that scratchpad may since have been cleaned by the environment; the table below is now the
durable record, transcribed directly from the completed run.)

| Benchmark | Mean | Range | n | Spread |
|---|---|---|---|---|
| NHextend3 -n16 (**RETIRED, do not use — wrong field, see 3.2.1/3.3**) | ~~2041.5~~ | ~~1965 - 2115~~ | 4 | 7.3% |
| hackbench -T -g1 -f8 -l400000 (**superseded, see 3.3**) | 90.81s | 76.365 - 101.318s | 4 | 27.5% |
| dbench -t 12 16 (**superseded, see 3.3**) | 326.82 MB/s | 306.559 - 352.302 MB/s | 4 | 14.0% |
| ebizzy malloc 512KB | 938910 rec/s | 934906 - 945849 | 4 | 1.2% |
| ebizzy mmap 512KB | 8114 rec/s | 7602 - 8490 | 4 | 10.9% |
| ebizzy mmap 4MB (**superseded, see 3.3**) | 1803.4 rec/s | 1783 - 1823 | 8 | 2.2% |

Raw per-rep values, for reference:
- NHextend3 (old/superseded metric, see 3.2.1): 1965, 2045, 2041, 2115
- hackbench: 76.365s, 98.292s, 87.281s, 101.318s
- dbench: 317.059, 306.559, 352.302, 331.358 MB/sec
- ebizzy-malloc-512k: 945849, 938805, 936081, 934906 rec/s
- ebizzy-mmap-512k: 8490, 7602, 8164, 8200 rec/s
- ebizzy-mmap-4MB: 1818, 1823, 1822, 1807, 1796, 1788, 1783, 1790 rec/s

**Known noise characteristics from tonight, important for interpreting future steps**: hackbench
on this specific host/VM is highly variable run-to-run (spans seen tonight: 44s to 101s on
nominally identical configs) — always use n≥4 and look at the full range, not a single number.
ebizzy at 512KB chunks is similarly noisy (13-40% spread observed); the 4MB chunk size is
reliably tight (3-6% spread) and should be the primary go/no-go metric for quick checks, with
the full 4-benchmark suite run for any step you're prepared to trust as a real checkpoint.

### 3.2.1 NHextend3 metric correction and re-baseline — 2026-08-26

**The 2041.5 NHextend3 number above is retired.** NHextend3 prints several numbers per run
(`Ran for N times`, wait-time stats, `Total contention`, and a `max: X (avg: Y)` CS-hold-time
line); the original overnight baseline-collection script extracted *some* field from this output,
but that script lived only in a session scratchpad, which the environment had already cleaned up
by the time this was checked — the exact extraction logic is unrecoverable. When Step 1
(`6.17-GLOCK-1`) was boot-validated against this baseline, the correctly-extracted `Ran for N
times` field came back 42% higher (2894 vs 2041.5, no range overlap: 2747-2986 vs 1965-2115) —
too large a gap to hand-wave as noise. User's working hypothesis, not fully provable but the best
available explanation: the original script most likely captured the `max: X (avg: Y)` line's `avg`
value (CS-hold-time average, ns) instead — a same-order-of-magnitude, plausible field that a hasty
extraction could have grabbed by mistake, and which reads notably closer to 2041.5 than `Ran for
N` does (a spot check during this same investigation, from the run whose `tail -1` bug accidentally
captured only that field, gave avg values 1659-1735 — 17% low rather than 42% high).

**Decision: standardize on `Ran for N times` going forward** (higher = more contention-loop
iterations completed in the run — the doc's own §1.6/rseq-extension framing already treats this as
the throughput-shaped NHextend3 metric, which fits `Ran for N` far better than a hold-time
average would). Extraction command now fixed in §3.1.

**New NHextend3 baseline, measured live on `6.17.0-GLOCK-1+`, corunner ON, preempt=voluntary,
2026-08-26** (this is also, incidentally, this session's Step 1 boot validation run — see §4):

| Benchmark | Mean | Range | n | Spread |
|---|---|---|---|---|
| NHextend3 -n16 (`Ran for N times`) | 2894 | 2747 - 2986 | 4 | 8.3% |

Raw per-rep values: 2986, 2869, 2974, 2747. (One earlier ad-hoc single run, not part of this
n=4 batch, read 3035 — consistent with the same range.)

**Caveat inherited from §1.6/Step 5**: NHextend3 numbers generally aren't valid for cross-kernel
comparison until the rseq extensions land (Step 5) — without them NHextend3 silently falls back to
a cheaper internal code path. This new baseline was measured on a Step 1 build, which also lacks
rseq extensions, so it's subject to the same caveat; treat it as provisional until Step 5 exists
and NHextend3 can be re-baselined on its real code path. It replaces 2041.5 as the working number
for now because it uses the correct field, not because the fallback-path caveat has been resolved.

**Cross-checked against Step 0 (`6.17.0-vanilla617`), same day**: first attempt read 3529, 3297,
3289, 3142 (mean 3314, +15% vs the Step 1 number above) — looked like a real gap given Step 1
adds zero call sites and should be scheduler-identical to Step 0. Root cause: preempt mode had
silently defaulted to `lazy` on this fresh boot (never explicitly set), not `voluntary` as the
protocol requires — see the parentheses gotcha now called out in §3.1. After explicitly setting
`voluntary` and re-running: **2862, 2987, 2942, 3082 (mean 2968.25, range 2862-3082, 7.4%
spread)** — only 2.6% above Step 1's 2894, well inside both samples' own noise. **Step 0 and Step
1 match cleanly on all four benchmarks now**, exactly as Step 1 predicted.

**Note on corunner state**: this new baseline was explicitly measured under corunner ON. The other
three benchmarks (hackbench, dbench, both ebizzy variants) were re-measured in the same Step 1
validation run and landed within or close to the original (nominally "no corunner") §3.2 bands —
consistent with, but not firm proof of, the original baseline also having had corunner on. That
broader question (whether to relabel the whole §3.2 baseline's corunner state) is separate from
this NHextend3 metric fix and is still open — raise it explicitly if it needs resolving before
trusting a future step's hackbench/dbench/ebizzy comparison too.

### 3.3 Host-restart re-baseline — 2026-08-27 (**CURRENT ACCEPTED BASELINE for NHextend3,
hackbench, dbench, ebizzy-mmap-4MB**)

The physical host this VM runs on was restarted 2026-08-27. A sanity-check pass on
`6.17.0-vanilla617` (unchanged kernel, no corunner, preempt explicitly set to `voluntary`,
`nohz=off` confirmed) run twice, two hours apart, came back consistently well above every §3.2
band: dbench +26-32%, ebizzy-mmap-4MB +21-45% (well outside its previously-tight 2.2% spread), and
NHextend3 landing in the same "higher" family as §3.2.1's 2894 (corunner ON) rather than anywhere
near the retired 2041.5 — even with corunner off this time. `vmstat`/`mpstat` both read **0%
steal**. Conclusion: the restart genuinely quieted the physical host (fewer/no noisy-neighbor
VMs contending for the same physical cores yet), so this VM gets more real CPU/IO time per
wall-clock second than it did when §3.2 was captured. This is a host-environment shift, not a
kernel change — nothing in the kernel or IVH differs from §3.2's run. Reproduced across two
independent n=2 passes (below), so treated as the new working baseline rather than a fluke.

**Per user decision (2026-08-27): the retired 2041.5 NHextend3 number is no longer
referenced at all going forward — only the higher-family value below is the NHextend3 baseline.**
This table also supersedes §3.2's hackbench/dbench/ebizzy-mmap-4MB rows for all future step
comparisons; ebizzy malloc-512KB and mmap-512KB were not re-measured and still use §3.2's numbers.

| Benchmark | Mean | Range | n | Spread |
|---|---|---|---|---|
| NHextend3 -n16 (`Ran for N times`) | 2796.25 | 2716 - 2878 | 4 | 5.8% |
| hackbench -T -g1 -f8 -l400000 | 84.52s | 58.791 - 107.331s | 4 | 57.4% |
| dbench -t 12 16 | 424.03 MB/s | 412.126 - 432.303 MB/s | 4 | 4.76% |
| ebizzy mmap 4MB | 2330.25 rec/s | 2183 - 2541 | 4 | 15.4% |

Raw per-rep values:
- NHextend3, pass 1: 2878, 2810; pass 2: 2781, 2716
- hackbench, pass 1: 107.331s, 91.975s; pass 2: 79.975s, 58.791s
- dbench, pass 1: 412.126, 432.303 MB/s; pass 2: 424.823, 426.85 MB/s
- ebizzy-mmap-4MB, pass 1: 2347, 2541 (+ one ad-hoc spot check: 2614); pass 2: 2250, 2183

**hackbench's spread widened sharply** (27.5% → 57.4%, floor dropped from 76.4s to 58.8s) — still
consistent with §3.2's existing "highly variable, always use n≥4" characterization, just more so on
a quieter host; not treated as a separate anomaly. **ebizzy-mmap-4MB's spread also widened**
(2.2% → 15.4%) — worth re-tightening with a larger n before trusting it as a single-metric go/no-go
check again; for now prefer the full 4-benchmark comparison over any one ebizzy number.

Since §3.2.1's corunner-ON GLOCK-1 number (2894, 2747-2986) was captured pre-restart and this
no-corunner vanilla617 number (2796.25, 2716-2878) was captured post-restart, and both land in the
same higher family with overlapping ranges, this is read as further confirmation that 2041.5 was
always the wrong field — not as evidence about corunner state one way or the other.

---

## 4. The incremental step plan

Each step is a git commit (soon: tag) on branch `ivh-rebuild-main` in
`/root/kernels/linux-6.17-vanilla`, building on the previous step. Each step should: apply its
patch set, `make olddefconfig` (should report "no change" every time, since `.config` doesn't
change), set a distinct `CONFIG_LOCALVERSION`, build, install, reboot, run the full 3.1 suite,
compare against Step 0 and the immediately-prior step.

### Step 0 — vanilla + config — **STATUS: DONE, tagged `ivh-step-0-vanilla-base`, numbers in §3.2
(historical) / §3.3 (current accepted baseline as of 2026-08-27)**

Pure upstream v6.17, IVH's exact `.config`. This is the floor everything else is measured
against.

### Step 1 — BPF sched-hook infrastructure (inert) — **STATUS: BUILT, committed + tagged `6.17-GLOCK-1`, 2026-08-26**

Files ported (from `/root/linux-6.17`, diffed against `/root/kernels/linux-6.17-vanilla`, all
diffs confirmed byte-identical after porting): `include/uapi/linux/bpf.h` (`BPF_PROG_TYPE_SCHED`,
`BPF_SCHED` attach type, 3 new kfunc IDs), `include/linux/bpf_types.h`,
`include/linux/sched_hook_defs.h` (new file, all 14 hook declarations, ported verbatim even
though 12 are dead, for compile-compatibility with `MY_ivh_atc.bpf.c` later),
`kernel/bpf/{btf,syscall,trampoline,verifier}.c`, `tools/include/uapi/linux/bpf.h` (userspace
mirror). Also required, and not originally called out in this section's file list until building
it surfaced the gap: a **minimal, hand-curated** `include/linux/bpf_sched.h` +
`kernel/sched/bpf_sched.c` (new files) and one `kernel/sched/Makefile` line
(`obj-$(CONFIG_BPF_SYSCALL) += bpf_sched.o`) — the production `bpf_sched.{h,c}` are ~500/~1060
lines each and almost entirely IVH-specific tunables (capacity gates, Hot Threads, PV, uc/tks —
all Step 6+ material); what Step 1 actually needs is only the ~150-line hook-registration/BTF-
verification/kfunc core (production `bpf_sched.c` lines 913-1061: `bpf_sched_enabled_key`, the
`BPF_SCHED_HOOK`-generated nop stubs, `bpf_sched_hooks` BTF set, `bpf_sched_verify_prog()`, the 3
`sched_entity_*` kfuncs, `bpf_sched_prog_ops`/`bpf_sched_verifier_ops`) — ported verbatim from
those exact lines, nothing else. Without this, `syscall.c`/`verifier.c`'s new `bpf_sched_inc/dec`
and `bpf_sched_verify_prog()` calls don't link.

**No kernel-side call sites yet** — every `bpf_sched_<hook>()` is a nop returning its `DEFAULT`,
and nothing in the scheduler calls any of them. This step adds zero new runtime behavior, just the
ability to register/load `BPF_PROG_TYPE_SCHED` programs. **Expected result: numbers should match
Step 0 within noise.** This is a genuinely useful checkpoint — it isolates "does the mere presence
of this program-type infrastructure cost anything" from everything that comes after.

**Build verification performed** (compile-only, per this session's no-reboot constraint):
`make olddefconfig` → "No change to .config" (only diff was `CONFIG_LOCALVERSION`); targeted build
of all 5 touched/new objects (`btf.o`, `syscall.o`, `trampoline.o`, `verifier.o`, `bpf_sched.o`) —
clean, no warnings; full `make -j16` → clean full build, `bzImage` produced, release string
`6.17.0-GLOCK-1+`; re-running `make -j16` immediately after confirmed no-op (nothing left dangling).
**Boot-and-benchmark: DONE, 2026-08-26.** User booted `6.17.0-GLOCK-1+`, confirmed corunner on,
preempt set to voluntary. Full 4-benchmark suite run, live-monitored per-rep against the §3.2
bands for an early-abort trigger (none fired — no crash/hang/extreme-outlier signatures). Result:
**prediction holds for hackbench, dbench, and both ebizzy variants** — all landed within or close
to the §3.2 bands (hackbench 87.93s vs 90.81s baseline; dbench 353.9 vs 326.8 MB/s, ~8% higher but
not extreme; ebizzy malloc/mmap-512k/mmap-4mb all within noise). NHextend3 initially looked like a
42% miss, but that turned out to be a metric-definition question, not a Step 1 regression — see
§3.2.1 for the full correction and re-baseline. **Step 1 is validated**: the inert BPF
infrastructure costs nothing measurable, as predicted.

**Re-confirmed after host restart, 2026-08-27**: host was restarted (see §3.3), machine rebooted
back into `6.17.0-GLOCK-1+`, full suite re-run with `run_baseline.sh` against the new §3.3
baseline. NHextend3 (2774.75 vs 2796.25), hackbench (86.80s vs 84.52s), and dbench (427.04 vs
424.03 MB/s) all matched cleanly. ebizzy-mmap-4MB initially came back with a 51.1% spread
(2114-3586) — well outside its usual band — but 6 follow-up reps taken right after read
2006-2290 (mean 2172.83, 13.1% spread), overlapping §3.3's range and consistent with the first
batch's outliers being a one-off host transient right after boot, not a Step 1 characteristic.
**Step 1 remains validated** across both the pre- and post-restart host states.

**STEP 1 CLOSED, 2026-08-27.**

### Step 2 — lock-holder-identity tracking (default OFF) — **STATUS: BUILT, committed + tagged
`6.17-GLOCK-2`, 2026-08-27**

Ported `include/linux/ivh_lock_holder.h` (new) and the 6 ownership-transfer/release call sites
(A1, A2, A9 set-holder; R1, R2, R2b clear-holder) in `include/asm-generic/qspinlock.h` +
`arch/x86/include/asm/qspinlock.h`, verbatim from production. `ivh_lock_holder_enabled` left at
its compiled default (0) — no sysctl wired up this step. **Expected: matches Step 0/1 within
noise** — this is the "does having the hooks compiled in, even off, cost anything" checkpoint,
directly informed by this session's own finding that the READ_ONCE+branch cost is negligible.

**Gap found and not in this section's original file list** (same category as Step 1's
`bpf_sched.h`/`.c` gap): the header declares `extern unsigned long ivh_lock_holder_enabled` /
`ivh_holder_bits` and 3 out-of-line functions (`__ivh_lock_set_holder`, `__ivh_lock_clear_holder`,
`ivh_lock_holder_cpu`) that must actually be DEFINED somewhere or the kernel fails to link — the
"just 2 files" framing above only covered the call sites, not their storage. In production these
live in `arch/x86/kernel/kvm.c`, but fused into `ivh_pv_beat_calibrate()`, a single late_initcall
that ALSO calibrates the PV heartbeat / CS-stamp / Part-C jump thresholds — all Step 3/4 material
with zero dependency on the holder table. Rather than pull that whole calibration function in
early (breaking the one-mechanism-per-step isolation), added a **new, Step-2-only file**
`arch/x86/kernel/ivh_lock_holder.c`: the holder side table, the 4 per-CPU counters the 3 functions
actually touch (`ivh_holder_stamps`/`_clears`/`_unknown_empty`/`_unknown_collision`; production's
neighboring `ivh_holder_raced`/`_self` are unused by these 3 functions and belong to a later step,
so not ported), and a minimal late_initcall that only allocates the table — extracted verbatim
from production's calibration function, calibration logic left out entirely. Wired into
`arch/x86/kernel/Makefile` gated on `CONFIG_KVM_GUEST && CONFIG_PARAVIRT_SPINLOCKS` (both `=y` in
this `.config`), matching the header's own `#if` guard for the real (non-stub) implementation.

**Build verification performed** (compile-only, no-reboot at build time): `make olddefconfig` →
"No change to .config" (only diff was `CONFIG_LOCALVERSION`); targeted build of
`ivh_lock_holder.o` and `kvm.o` → clean; full `make -j16` → clean, `bzImage` produced, release
`6.17.0-GLOCK-2+`; rerun confirmed idempotent (build #3 both times). Installed
(`modules_install`/`make install`/grub regenerated), one-time `grub-reboot` set to boot straight
into it next.

**Boot-and-benchmark: DONE, 2026-08-27.** Booted `6.17.0-GLOCK-2+`, no corunner, preempt=voluntary,
`run_baseline.sh 4` against §3.3. NHextend3 (2708.25, 2681-2746, 2.4% spread) and hackbench (85.83s,
76.0-97.6s) both matched §3.3 cleanly. dbench (369.41 MB/s, 335.4-392.7) and ebizzy-mmap-4MB
(2793.50, 2682-2876) both diverged from §3.3 with no range overlap — dbench ~11% low, ebizzy ~21%
high. Followed up with 4 more reps of each: dbench settled at 376.74 MB/s (n=8, 335.4-392.7, 15.2%
spread), ebizzy at 2819.6 (n=8, 2682-2880, 7.0% spread) — both held steady, not a fluke. Checked
`%steal` (0%, same as the §3.3 host-restart event) and for leftover D-state/benchmark processes
(none) — the elevated load average at the time was just decay from the dbench I/O itself, not live
host contention, so this isn't the same "host got quieter" story as §3.3's event.

**Read as boot-to-boot host drift, not a Step 2 regression**: NHextend3 is the benchmark most
sensitive to even a single extra READ_ONCE+branch on the hottest qspinlock fast path (it's a tight
userspace spin-loop hammering one lock 16-way), and it's the one that matched §3.3 most tightly of
all four. dbench and ebizzy are both I/O/memory-bandwidth-sensitive rather than lock-path-sensitive,
and diverged in OPPOSITE directions (one down, one up) — a pattern that doesn't fit a single
common-cause explanation (unlike §3.3's uniform steal-driven shift) and points at boot-to-boot
page-cache/memory-subsystem warmth variance rather than anything Step 2 touches. **Step 2 is
validated**: the compiled-in-but-off holder-identity infrastructure costs nothing measurable on
the metric best positioned to detect it.

**Re-confirmed with a proper control, 2026-08-27 (later same day).** A follow-up 2-rep spot check
on GLOCK-2 came back dramatically elevated across all four benchmarks simultaneously (NHextend3
~3465-3573, hackbench 37-45s vs. the usual 76-107s, dbench 409-423, ebizzy 3800-3954 vs. baseline's
~2330) — `%steal` still read 0%, ruling out host steal specifically, and a guest-side investigation
(CPU count/cgroup limits/workload-truncation check) found nothing: hackbench/dbench/ebizzy all
self-reported doing their full requested work. The uniform "everything faster" direction (unlike
the mixed-direction dbench/ebizzy divergence above) pointed at a transient host-level performance
window rather than a Step 2 effect, but this needed an actual control to confirm rather than being
asserted. **The control**: rebooted to `6.17.0-vanilla617` (fresh boot) and re-ran — vanilla came
back matching the ORIGINAL baseline closely (NHextend3 2861, hackbench 97.99s, dbench 402.13,
ebizzy 2252), not the elevated numbers, disproving "it's just host noise, any kernel would show
it" in its naive form. Rebooted back to GLOCK-2 (fresh boot, same time window as the vanilla test)
and re-ran once more: GLOCK-2 now ALSO matched the original baseline closely (NHextend3 2788,
hackbench 75.17s, dbench 407.01, ebizzy 2791.50) — the elevation was gone. This is the clean
result: the earlier elevated window was time-bound to whenever GLOCK-2 happened to be under test
that hour, not tied to which kernel was booted -- when both kernels are tested in the SAME time
window (this final pair), they agree. **Step 2 is confirmed clean** by an actual A/B control, not
just an inference from `%steal` reading zero.

### Step 3 — CS-hold timing (`cs_enter`/`cs_exit`, clean version) — **STATUS: BUILT, committed +
tagged `6.17-GLOCK-3`, 2026-08-27**

Ported `task_struct`'s `lock_depth`/`cs_start_ts`/`cs_wall_start_ts`/`last_cs_ns` fields, the
fork-time reset (`__sched_fork`), the `prepare_task_switch`/`finish_task_switch` pause-resume
pairing that keeps `cs_start_ts` correct across a preemption mid-hold, and `cs_enter()`/`cs_exit()`
plus their 10 wrapper call sites in `kernel/locking/spinlock.c` — verbatim from production, minus
the dead `cumulative_cs_time` accumulation and `wait_depth` (confirmed dead by this session's own
audit, commit `6859e40fb`: both fields are only ever reset to 0, never accumulated/incremented
anywhere in the tree).

**Deliberately excludes two other mechanisms production fuses into these same two functions**:
the CS-preemption-stamp system (`IVH_HAVE_CS_BEAT`/`ivh_cs_beat_publish`/`ivh_cs_preempt_src`,
`cs_beat_lock`) and the Hot Threads contention/preemption classifier (`ivh_pre_lock`,
`ivh_hot_note_wait_event`, `ivh_hot_preempt_update`, the `ivh_obs_*` observe-only counters) — both
are later-step material with no dependency on CS-hold timing itself, so neither task_struct fields
nor call sites for either were ported here.

Added `ivh_cs_track_enabled` as the 0/1/2/3 diagnostic sysctl, in a new minimal sysctl table in
`kernel/sched/bpf_sched.c` (production's equivalent table has dozens of unrelated entries for
later steps; ours has exactly one). Default 1 (real `sched_clock()` TSC reads, bit-for-bit the
original always-on behavior this step's mechanism replaces).

**Build verification performed**: `make olddefconfig` → "No change to .config"; targeted build of
`spinlock.o`/`core.o`/`bpf_sched.o` → clean, no warnings; full `make -j16` → clean, `bzImage`
produced, release `6.17.0-GLOCK-3+`; rerun confirmed idempotent (build #4 both times). Installed
(`modules_install`/`make install`/grub regenerated) — a new `6.17.0-GLOCK-3+` grub entry exists,
but no one-time `grub-reboot` has been set yet, so the machine will not boot into it on its own.

**Boot-and-benchmark: CONFIRMED, 2026-08-28.** Booted `6.17.0-GLOCK-3+`, preempt explicitly set to
`voluntary`, `%steal` 0%: NHextend3 2751.0 (2751-2751, n=2), hackbench 80.45s (78.9-82.0s, n=2) —
both landing right in the §3.3 baseline band (NHextend3 2796.25, hackbench 84.52s), not the
elevated ~3400-5300 range seen for most of this session (see §5's host-state-drift discussion —
the physical host appears to swing between "quiet" and "busy" windows on a timescale neither this
session nor the user's own host-side interventions control; a fragmenter/corunner reset with no
effect and a plain reboot with no host-side changes at all both preceded a swing back into the
slow band, which rules out anything either of us was doing as the cause). **Step 3 confirmed
clean at this host-state point** — matches baseline, no cost detected at the resolution this
sample size gives.

**Expected: some real, small cost vs Step 2** — this session's earlier (pre-rebuild) live
measurement found the always-on TSC-read + bookkeeping tax insufficient alone to explain the full
historical regression (~10-17% of it), but real; the rebuild's Step 2→3 comparison is the chance
to isolate that number cleanly, with everything before Step 3 already confirmed to cost nothing.
A larger-n rerun in the same host-state window would be needed to actually resolve a ~10-17%
effect against this benchmark suite's noise floor (hackbench alone showed 20%+ spread tonight) —
treat this as "no gross regression," not "cost precisely measured," until that's done.

### Step 4 — PV wait/kick substitution, mechanism=0 (stock-mimicking) — **STATUS: BUILT, committed +
tagged `6.17-GLOCK-4`, 2026-08-28**

Ported `arch/x86/kernel/kvm.c`'s PV scaffolding (§1.4 items 1-5: `kvm_spinlock_init()`
registration, mechanism 2's scoped-halt `ivh_pv_wait()`, the two-stage IPI wake in
`ivh_pv_kick()`/`pv_kick_node()`, the cross-knob safety validators, the per-CPU TSC heartbeat),
a new minimal `arch/x86/include/asm/ivh_tsc_beat.h`, and `kernel/locking/qspinlock_paravirt.h`
(`is_wait_preempted()`, `ivh_beat_publish_in_spin()`, the mechanism-2 scoped-halt gate in
`pv_wait_node()`). `ivh_pv_wait_mechanism=0` (compiled default, unchanged) — mechanism 0 is
byte-for-byte the pre-IVH `kvm_wait()`/`kvm_kick_cpu()` behavior, so this step tests whether
*registering* IVH's own PV substitute at all costs anything vs vanilla's own PV registration.

**Deliberately excludes three things production fuses into these same files, all confirmed
out-of-scope by §1.7's artifact list**: the CS-preemption-stamp predicate (`ivh_cs_beat`,
`ivh_cs_preempt_src`, `ivh_cs_head_check()` — "fully wired, large, but default-OFF, never enabled
in production, predicate has a measured hard ceiling of 78.57% sensitivity"); the lock-holder
ACQUIRE-side stamp call sites (A6/A7/A8 in `qspinlock.c`/`qspinlock_paravirt.h`) and
`ivh_lock_steals` (same §1.7 entry bundles these with the CS-stamp predicate — Step 2 already
ported the storage and the release-side clear sites R2/R2b inertly, so the table stays
permanently empty either way, matching production's own runtime behavior); and Part C
(`ivh_vact_capacity`/`ivh_vact_jump_threshold` plus the raw-TSC↔ns helpers that exist only to
feed it — "attempted, measured regression, root-caused, not shipped"). `struct ivh_lock_halt`
(HLT/poll cycle accounting) is ported since `ivh_pv_wait()` calls it unconditionally, but its
only consumer (phantom-steal correction) is Step 6/8 material and isn't ported, so its counters
accumulate unread for now — same posture as Step 2's holder-table counters. Defined locally in
`kvm.c` rather than `kernel/sched/core.c` (production's location) since nothing else in this
tree needs it there yet.

New minimal `ivh_pv_sysctls[]` table (6 entries: `wait_mechanism`, `wait_trace`,
`kick_pure_ipi`, `preempt_src`, `beat_threshold`, `beat_publish_mask`) — production's equivalent
table has 4 more entries for the excluded CS-beat/holder-identity knobs.

**Build verification performed**: `make olddefconfig` → "No change to .config" (only
`CONFIG_LOCALVERSION` hand-edited, `-GLOCK-3` → `-GLOCK-4`); targeted build of
`kvm.o`/`qspinlock.o` → clean, no warnings (confirmed via a forced rebuild + grep for
warning/error, not just absence from a truncated log); full `make -j16` → clean, `bzImage`
produced, release `6.17.0-GLOCK-4+`; forced rerun confirmed idempotent and still warning-free
(build #5 → #6). Installed (`modules_install`/`make install`/grub regenerated) — a new
`6.17.0-GLOCK-4+` grub entry exists, but no one-time `grub-reboot` has been set yet.

**Boot-and-benchmark: CONFIRMED, 2026-08-28.** Booted `6.17.0-GLOCK-4+`, preempt explicitly set to
`voluntary`, `%steal` 0%. dmesg confirmed clean registration: `"IVH: PV spinlock substitute
registered ... ivh_pv_wait_mechanism=0"` and `"IVH: TSC heartbeat threshold = 3300000 cycles"` at
boot. Every new sysctl sat at its compiled "do nothing different" default
(`ivh_pv_wait_mechanism=0`, `ivh_pv_preempt_src=0`, etc.) with no manual configuration needed —
mechanism 0 is designed to reproduce old behavior automatically. n=4: NHextend3 2814.5
(2760-2866, 3.8% spread), hackbench 85.81s (75.6-93.2s, 20.5% spread — consistent with this
benchmark's usual noise, not a new effect), dbench 399.45 MB/s (383-426, 10.8% spread), ebizzy-4MB
2386.75 (2324-2440, 4.9% spread). All four land in the same band as the §3.3 baseline and the
GLOCK-3 re-confirmation directly above, taken in the same host-state window. **Step 4 confirmed
clean**: registering IVH's PV substitute in mechanism=0 costs nothing measurable, exactly as
predicted.

**Expected: should be very close to vanilla's own PV numbers** (this session already confirmed
vanilla registers genuine PV spinlocks the same way, dmesg-verified) — mechanism 0's only real
addition over vanilla's own PV path is the `ivh_pv_wait_calls` counter increment and the
`ivh_lock_halt_begin`/`end()` cycle-accounting pair around each halt, both cheap. **Confirmed.**

### Step 5 — rseq extensions — **STATUS: BUILT, committed + tagged `6.17-GLOCK-5`, 2026-08-28**

Ported §1.6's files: the `RSEQ_SCHED_STATE_FLAG_IVH_DANGER` advisory bit (published to userspace
on every return-to-userspace via `rseq_update_cpu_node_id()`) plus its `struct rseq_sched_state`
registration path (`kernel/rseq.c`, `include/uapi/linux/rseq.h`, `include/linux/rseq.h`,
`include/linux/sched.h`), the `exit_to_user_mode_loop()` cooperative-yield hook
(`kernel/entry/common.c`), and the `sched_yield()` fast-path (`kernel/sched/syscalls.c`). Also
ported the `rseq_delay_resched*()` critical-section grace-period family that lives in the same
file, since splitting it out of `kernel/rseq.c` would have been more invasive than including it —
but **left it fully inert**: its only setter, `rseq_delay_resched_fini()`, is wired into
`include/linux/irq-entry-common.h` in production, a file outside §1.6's listed scope, so it's
compiled and declared here but never called. This matches the doc's own finding that this
sub-feature is "never exercised by any of the 4 benchmarks" — now doubly true in this build.

**Deliberately stubs `ivh_task_rq_in_danger()`** (declared in `include/linux/bpf_sched.h`, defined
in `kernel/sched/fair.c`) to always return `false` rather than porting production's real Gate 1+2
re-check, which depends entirely on the capacity/migration engine (`ivh_universal_eligible`,
`ivh_cap_source`, `ivh_gate_capacity()`, `ivh_gate_time_left_reject()`) — Step 6/8 material. Since
`ivh_universal_eligible` is always 0 until Step 8, production's own function would unconditionally
return `false` at this point in the rebuild anyway; the stub reproduces that directly. Same posture
as Step 4's `ivh_pv_wait_mechanism=0` default and Step 3's `ivh_cs_track_enabled`.

**Build verification performed**: `make olddefconfig` → "No change to .config"; targeted build of
all 5 touched `.c` files → clean on the first attempt, no warnings (confirmed via a forced rebuild
+ grep for warning/error); full `make -j16` → clean, `bzImage` produced, release
`6.17.0-GLOCK-5+`; rerun confirmed idempotent both before and after the `-GLOCK-4`→`-GLOCK-5`
version bump. Installed (`modules_install`/`make install`/grub regenerated) — a new
`6.17.0-GLOCK-5+` grub entry exists, but no one-time `grub-reboot` has been set yet.
**Boot-and-benchmark: pending.**

**Expected: no change on 3 of 4 benchmarks, NHextend3 numbers become meaningful** (they aren't
really valid on Steps 0-4 as a cross-kernel comparison, since NHextend3's rseq registration
silently falls back to a cheaper code path without this — a real confound found and retracted a
finding over earlier in this session).

**Post-break re-verification on GLOCK-4, 2026-08-29** (before proceeding to Step 5's own
boot-and-benchmark): after a multi-day pause, user asked for a sanity check that the system was
still behaving as expected. Ran `run_baseline.sh` four times across the session on
`6.17.0-GLOCK-4+`, preempt=voluntary, `%steal` 0% throughout:

| Run | NHextend3 | hackbench | dbench | ebizzy-4MB | Notes |
|---|---|---|---|---|---|
| §3.3 baseline | 2796.25 | 84.52s | 424.03 MB/s | 2330.25 | reference |
| 1 (n=2) | 2925 | 72.2s | 430.7 | 3456 | landed in the "fast/quiet" regime |
| 2 (n=2) | 2719.5 | 82.8s | 374.1 | 2089 | swung back to "slow" regime |
| 3 (n=3) | 2818 | 90.7s | 385.6 | 2022 | still "slow" regime |
| 4 (n=3, **after rebooting both the guest VM and the (host-external) sysbench corunner**) | 2810.7 | 88.3s | 387.2 | 2176 | matched runs 2/3 closely |

Guest-side health checked and ruled out as a cause: uptime was only ~22h (not "days" as first
worried), `/proc/buddyinfo` showed no fragmentation, `pgscan_kswapd`/`pgscan_direct` were 0 since
boot (never once under memory pressure), dirty pages ~0, no D-state/leftover processes, `%steal`
0% on all 16 vCPUs individually via `mpstat -P ALL`.

**Key finding**: rebooting both endpoints (run 4) did **not** move dbench/ebizzy back toward the
§3.3 baseline — they landed at essentially the same level as the pre-reboot runs 2/3 (dbench
~385-387, ebizzy ~2020-2176, both ~9-13% below §3.3). This argues *against* progressive
degradation (a true decay signal should partially reset after a full reboot of both sides) and
*for* a settled, reproducible new contention level — most likely the corunner's current
workload/intensity differing from whatever was running when §3.3 was recorded, given dbench/ebizzy
are the two memory-bandwidth/cache-sensitive benchmarks and NHextend3/hackbench (lock/scheduling-
bound) tracked baseline closely across all 4 runs. Same asymmetric-benchmark signature as the
GLOCK-2 §3.2.1 "boot-to-boot host drift" entry above.

**Resolved, 2026-08-29**: did a full vanilla→GLOCK-1→GLOCK-2→GLOCK-3→GLOCK-4→GLOCK-5
bisection, 2 reps each (3rd rep only for any benchmark whose first two didn't agree, and only
that benchmark re-run, not the whole suite). Every kernel landed in the same population — no step
showed a benchmark cleanly and consistently separating from the pack (NHextend3 stayed in
2770-2920, hackbench bounced 63-90s with no trend, dbench stayed remarkably flat at 397-413 across
all six kernels, ebizzy stayed in 2160-2580). **Conclusion: the dbench/ebizzy dip from earlier is
confirmed host/corunner-side, not introduced by any rebuild step 0-5.** No GLOCK-N.M micro-bisection
needed.

**Functional verification of the rseq extension itself, 2026-08-29** (the performance numbers
above don't move much yet since nothing reads the danger bit until Step 6/8 — this is a
correctness check that the plumbing is actually live, not a perf check). Two levels of evidence:

1. `NHextend3 -l` (its built-in `IVH_DANGER local pre-check` report):
   `ivh_sched_state_active` was true and it reported `Attempts: 2976 (skipped 2976, syscall made
   0) — Syscalls avoided: 100.00%`. On its own this is ambiguous — the danger bit is stubbed to
   always-false, so a dead/no-op write path would produce the exact same 100%-skipped reading,
   since NHextend3's thread-local `ivh_sched_state` starts zero-initialized.
2. **Decisive test, `cvm_setup/rseq_verify.c`** (new minimal standalone program, not tied to
   NHextend3's other logic): explicitly zeros its own `ivh_sched_state.state` in userspace, then
   registers the extended 72-byte rseq (confirming `AT_RSEQ_FEATURE_SIZE = 72` — the kernel is
   advertising the new struct size via the ELF auxv), then reads the field back with zero other
   code able to touch it. Result: `state=0x1` (`RSEQ_SCHED_STATE_FLAG_ON_CPU`) appears
   **immediately after the registering `rseq()` syscall returns** — the only possible source of
   that bit flip is the kernel's `rseq_update_cpu_node_id()` writing through the registered
   `sched_state_ptr` on that very return-to-userspace. `IVH_DANGER` correctly stayed clear (as
   expected — `ivh_task_rq_in_danger()` is still stubbed false). This closes the ambiguity from
   (1): the full round-trip (extended struct → registration → kernel write-back → userspace read)
   is confirmed genuinely live on GLOCK-5, not a zero-init artifact. Keep `rseq_verify.c` as the
   reusable smoke test for this on any future kernel in the chain.

### Step 6 — the migration engine (plumbing only, `ivh_universal_eligible=0`) — **STATUS: BUILT,
committed + tagged `6.17-GLOCK-6`, 2026-08-29**

Ported the real capacity/migration decision chain: `kernel/sched/bpf_sched.c`'s knobs
(`ivh_capacity_threshold`, `ivh_time_left_threshold_ns`, `ivh_migration_timeout_ns`,
`ivh_max_concurrent`, `ivh_sched_timeout_ms`, `ivh_eval_cooldown_ns`, `ivh_time_left_source`,
`ivh_selection_trylock`, `ivh_migrate_mechanism`, `ivh_universal_eligible`, `ivh_cap_source` +
validator, the full `ivh_uc_*` block) plus the extended sysctl table and `SYSCALL_DEFINE0
(ivh_cs_enter)`; `kernel/sched/core.c`'s `average_capacity_all`, `ivh_steal_source` (0/2 only),
`ivh_tks_*` calibration knobs, `ivh_idle_ns()`, `ivh_uc_steal_ns()`, `ivh_uc_ema()`,
`ivh_uc_close()`, `ivh_uc_maybe_close_window()`, `ivh_uc_tick()`, `ivh_tick_steal_accumulate()`;
`kernel/sched/cputime.c`'s `account_idle_time()`/`steal_account_process_time()` extensions and new
`is_cpu_preempted()`, plus the four Gate-2/heartbeat/uc/tks call sites in `account_process_tick()`;
`kernel/sched/fair.c`'s `bpf_sched_pre_lock_migrate()` (the real decision+dispatch function),
`ivh_steal_imminent()`, `ivh_gate_capacity()`, `ivh_gate_time_left_reject()`,
`ivh_rq_capacity_and_timeleft_ok()`, `ivh_eval_cooldown_ok()`, the `ivh_wait_register`/
`_unregister` registry, and `ivh_task_rq_in_danger()`'s real implementation (replacing Step 5's
stub); `kernel/locking/spinlock.c`'s `ivh_pre_lock()` + its 3 call sites
(`_raw_spin_lock`/`_irqsave`/`_irq`) — exactly the "later-step material" Step 3's own comment
flagged as excluded. `kernel/sched/sched.h`'s `struct rq` gains `prmpt_flags`/`clock_preempt`/
`last_idle_tp`/`last_preemption`/`ewma_act_ns`/`last_active_time`/`ivh_last_eval_ns`/
`preemptions`/`max_latency` plus the full `ivh_uc_*`/`ivh_tks_*` blocks. `ivh_universal_eligible`
stays at its compiled default of **0** throughout — nothing in this build migrates anything.

**Deliberately excludes** (see §1.7): Hot Threads (already excluded everywhere); Part C /
`ivh_vact_*` (`ivh_cap_source=2` folds into the default case in `ivh_gate_capacity()` rather than
referencing a nonexistent field; the tsc_pe branch of Gate 2 is dropped entirely since production
itself leaves `ivh_preempt_event_source=0`, so the real shipped kernel never takes that branch
either — omitting it reproduces production's *actual* behavior, not a simplification of it);
`ivh_uc_shadow`/`ivh_decision_shadow` (including the O(nr_cpus) destination-set comparator block
inside `bpf_sched_pre_lock_migrate()`); `ivh_uc_avgcap_enabled`; `ivh_uc_used_source=1` (ACCT);
the `ivh_ref_*` Plan-2 REF_TSC steal estimator and its dependent `ivh_ka_*` idle-keepalive
machinery (not in §1.5's dependency chain, and §1.7 confirms `ivh_ref_method` as a dead end) —
`ivh_steal_source`'s validator now only accepts 0 or 2; and the broadcast preempt-migrate
mechanism (`preempt_migrate_func()`, `custom_idle_poll()`, the `cfs_spin_len` hook call site) —
investigation showed this is a distinct, separately-dependent active-rescue mechanism entangled
with idle-poll/`stop_one_cpu_nowait` machinery outside this step's scope, deferred rather than
dragged in unscoped; `bpf_sched_cfs_spin_len()` stays Step 1's inert, uncalled stub.

**IMPORTANT CAVEAT on Part C's exclusion**: `ivh_vact_tick()` runs **unconditionally every tick**
in the real production kernel regardless of `ivh_cap_source`'s value (called from
`account_process_tick()` with no gate) — meaning production pays this real cost even in its
actual shipped configuration (`ivh_cap_source=3`, not 2). Since §1.5's dependency chain never
mentions Part C, this rebuild is deliberately *not* paying that unconditional per-tick cost. If
Step 6's eventual numbers land suspiciously cheap against §5's historical gap-hunt, this is a
known, documented reason why — not a surprise to re-investigate from scratch.

**Gaps found and not in this section's original file list** (same category every prior step has
hit): `clock_preempt`/`is_cpu_preempted()` — a foundational tick heartbeat
`bpf_sched_pre_lock_migrate()`'s target-health check depends on, not itself named in §1.5's list;
ported to `kernel/sched/cputime.c`, matching production's exact location. `ivh_raw_tsc()`/
`ivh_tsc_cycles_to_ns()`/`ivh_tsc_ns_to_cycles()` — originally left out of Step 4 under the Part-C
exclusion umbrella, but turn out to be shared low-level primitives `ivh_tick_steal_accumulate()`
also needs directly; added to `arch/x86/include/asm/ivh_tsc_beat.h`, Part C's own capacity
field/tick function/sysctls remain fully excluded. Syscall 470 (`ivh_cs_enter`) itself was never
registered in any prior step; NHextend3's rseq-fed userspace-CS path (Step 5) needs it to link —
added to `arch/x86/entry/syscalls/syscall_64.tbl`, free and matching production's assignment
exactly.

**SAFETY NOTE for Step 7/8**: Step 1's inert hook-stub macro makes
`bpf_sched_cfs_select_run_cpu_spin()` default to returning **0** (CPU 0), not -1 ("no target") —
confirmed via `sched_hook_defs.h`'s own `BPF_SCHED_HOOK(int, 0, cfs_select_run_cpu_spin, ...)`
declaration. Setting `ivh_universal_eligible=1` *before* Step 8 loads the real `MY_ivh_atc` BPF
program would make every eligible migration target CPU 0 — a thundering-herd bug, not a graceful
no-op. Harmless under this step's own posture (eligible stays 0); Step 8 must load the BPF
program before flipping the sysctl, matching production's own launch-script ordering requirement
(§1.5 item 8).

**`ivh_task_rq_in_danger()` decision**: replaced Step 5's always-false stub with production's real
Gate 1+2 re-check now that its dependencies exist. Verified this is behaviorally identical in
practice: `ivh_universal_eligible=0` makes the real function's own first check return false
unconditionally, same as the stub, but for the real reason now, not a placeholder one.

`tools/bpf/MY_ivh_atc.bpf.c` + `vmlinux.h`/`bpf_helpers.h`/`bpf_helper_defs.h` copied best-effort
into `tools/bpf/` per this section's own instruction — file presence only, **not loaded, not
wired to any userspace loader**.

**Build verification performed**: `make olddefconfig` → "No change to .config" (only
`CONFIG_LOCALVERSION`, `-GLOCK-5` → `-GLOCK-6`); targeted build of all 5 touched `.c` files →
clean on the first attempt, confirmed warning-free via a forced rebuild + grep; full `make -j16`
→ clean (one pre-existing, unrelated `drivers/char/random.c` frame-size warning), `bzImage`
produced, release `6.17.0-GLOCK-6+`, build #9; rerun confirmed idempotent (#9 both times, still
clean). Installed (`modules_install`/`make install`/grub regenerated) — a new `6.17.0-GLOCK-6+`
grub entry exists, but no one-time `grub-reboot` has been set.
**Boot-and-benchmark: pending.**

**Expected: this is the step most likely to show a real, measurable cost even with migration
off** — the tick-hook additions (`ivh_uc_tick`, `ivh_tick_steal_accumulate`) run unconditionally
once this code exists, regardless of the eligibility gate. Watch this one closely; per-tick
global-lock-shaped costs (like the already-found-and-fixed `ivh_scan_stuck_waiters`) are exactly
the kind of thing to hunt for here if numbers regress — though note the Part-C-tick-cost caveat
above cuts the other way (missing cost, not added cost), so a *clean* result here doesn't fully
resolve §5's gap-hunt either.

### Step 7 — enable full production PV (mechanism=2) — **STATUS: specified, sysctl-only, no rebuild needed**

Pure sysctl flip on top of Step 6's build: `ivh_pv_wait_mechanism=2`, `ivh_pv_kick_pure_ipi=1`,
`ivh_pv_preempt_src=2`. This isolates the real, validated adaptive-spinning benefit from
migration entirely. **This is the step this session's origin investigation (hours ago) already
showed a real win on** — expect hackbench/dbench/ebizzy to genuinely improve here.

### Step 8 — enable migration (full production IVH) — **STATUS: specified, sysctl + daemon**

`ivh_universal_eligible=1` + load `MY_ivh_atc` (built with `IVH_CAP_HARDFLOOR=700`, resolved
§1.5/§2) + start `vcap_probe -p 200 -s 200` (also resolved). This should reproduce the numbers this session's earlier
investigation established for the actual custom kernel tonight. If it doesn't, the discrepancy
*is* the answer to "what makes custom IVH slower than vanilla" — compare Step 8's numbers here
against the real custom-kernel numbers from earlier tonight's session log; any gap remaining at
this final step, after every mechanism was added back deliberately and individually validated,
is the true unexplained residual.

---

## 5. This session's own open finding — read before assuming Step 8 will fully explain things

Independent of everything above, this session spent hours tonight chasing a real, reproducible
~15-24% throughput gap between the *actual* custom kernel (`6.17.0-rseqport75/76/77+`, i.e. the
real `/root/linux-6.17` tree as it stood before tonight's cleanup) and vanilla v6.17, using the
4MB ebizzy metric with every known mechanism (migration, CS-tracking) disabled. Findings, in
case the incremental rebuild reproduces the same gap and someone needs a head start:

- **Not explained by**: Kconfig differences (exhaustively diffed, identical except
  `CONFIG_LOCALVERSION`), compiler flags (`CONFIG_CC_OPTIMIZE_FOR_PERFORMANCE`, LTO, stack
  protector — all identical), `mm/` (byte-identical to vanilla, zero IVH involvement),
  `arch/x86/` beyond `qspinlock.h`/`kvm.c` (both already accounted for above), `wait_depth`
  (removed, no measured effect), `ivh_scan_stuck_waiters` (removed, no measured effect — though
  this was tested under a noisy corunner-adjacent state, worth re-testing clean).
- **Genuinely explains part of it**: `cs_enter`/`cs_exit`'s two TSC reads per outermost CS
  (quantified via objdump, real, but only ~10-17% of the gap when isolated).
- **The best empirical lead, from live `bpftrace` profiling (PMU is blocked in this TDX guest,
  confirmed — perf-based profiling is not an option here, use kprobes/kretprobes)**: both
  vanilla and custom show `mmap_lock` (rwsem) write-side contention dominating wall-clock time
  in this benchmark (58.7% of thread-time on vanilla, 68.0% on custom, out of a 320-thread-
  second budget), concentrated almost 1:1 in `munmap` specifically on both kernels — so that
  *shape* is inherent to the workload, not an IVH artifact. But custom's proportion is
  consistently higher than vanilla's, suggesting the various small, individually-modest
  overheads found throughout the locking fast path (this doc's §1.2, §1.3, and the LH-identity
  hooks) **compound** under this benchmark's extreme lock-acquisition frequency (tens of
  thousands of rwsem slowpath entries, each internally hammering `osq_lock`/qspinlock
  repeatedly) rather than any single mechanism being solely responsible.
- **Practical implication for the rebuild**: don't expect a single step in Section 4 to show a
  dramatic jump that "explains" the whole gap. Expect small, real, individually-defensible costs
  at Steps 2, 3, and 6 that need to be *summed* to reproduce it. If any single step shows an
  outsized jump, that's a genuinely new finding worth chasing hard — but the prior, based on
  tonight's work, is "distributed cost," not "one bug."

---

## 6. How to continue this with a fresh Claude session

Tell the new session: read this file first
(`/root/linux-6.17/tools/bpf/docs/ivh_rebuild_plan.md`), then:

1. Confirm `/root/kernels/linux-6.17-vanilla` still exists with branch `ivh-rebuild-main` and
   tag `ivh-step-0-vanilla-base`. If the machine was reset and it doesn't:
   `git clone --depth 1 --branch v6.17 https://github.com/torvalds/linux.git
   /root/kernels/linux-6.17-vanilla` (takes ~20s), then recreate Step 0 by copying
   `/root/linux-6.17/.config`, changing only `CONFIG_LOCALVERSION`, and committing.
2. Build the next un-built step (start at Step 1 if this is a truly fresh continuation): apply
   the file list for that step from Section 4 (diff the named files from `/root/linux-6.17`
   against the current vanilla-rebuild tree, apply, resolve any conflicts by hand — these are
   curated subsets of a much larger diff, not clean upstream patches, so expect some manual
   work), `make olddefconfig`, `make -j$(nproc)`, `sudo make modules_install && sudo make
   install`, then either ask the user to reboot or (if this new session *can* reboot, unlike
   this one) do it directly.
3. Run the exact Section 3.1 commands, compare against Step 0's numbers and the immediately
   prior step's numbers, using n≥4 for hackbench/dbench/NHextend and n≥8 for the 4MB ebizzy
   metric (tighter signal, worth the extra time).
4. Record the result as a new commit message on that step's commit (include the raw numbers,
   not just a verdict), then move to the next step.
5. If a step's numbers show something unexpected, don't force a clean-win narrative — this whole
   project's house culture (visible throughout the doc set audited tonight) is "reproduce both
   arms, don't trust a single unreplicated result, and report honestly even when the honest
   answer is 'this didn't help.'" Keep that up.

Full sysctl reference for the final production state is in §1.5 and §1.4 above (also mirrored
verbatim in `cvm_setup/IVH_start.sh` in the custom tree — that script is ground truth, this doc
explains *why* each line is there).
