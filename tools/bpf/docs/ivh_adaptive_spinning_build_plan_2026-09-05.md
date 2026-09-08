# IVH adaptive spinning: decisive test results + incremental build plans, 2026-09-05

Status: **one hypothesis tested and falsified as originally scoped (with one untested variant
still open), two designs staged into buildable, debug-instrumented, independently-testable
patches. Nothing in this doc has been implemented yet.**

> **2026-09-06 update — read this before anything below.** Several things in this doc are now
> stale or closed out by the mode-collapse rebuild (`ivh_adaptive_mode` 0/1/2) and a full session
> of follow-up investigation. Summary of what changed, full detail in new §§5-8:
> - **§1 (Fix 1) is CLOSED, not just "no-go as scoped."** It required restoring the hypercall
>   alongside the IPI in modes 1/2. Under the mode collapse, modes PURE_IPI/ADAPTIVE are defined
>   to never send `KVM_HC_KICK_CPU` at all — restoring it isn't a tuning option anymore, it's a
>   design-constraint violation. Do not revive it.
> - **The premise that the 5.7-5.8x per-halt gap is mostly "wake-retirement/ISR cost" (§1's own
>   framing) turned out to be an overreach.** A same-session re-analysis found it's more likely a
>   mix of a measurement population mismatch (mode 0 halts on both IF=0 and IF=1 paths; modes 1/2
>   only halt at IF=1) and lost/late wakes rescued by the tick — not pure servicing cost. See §5.
> - **A live disassembly + read-only MSR-timing investigation confirmed there is no further
>   optimization available in the wake mechanism's own cost, full stop** — the entire guest-side
>   budget is <50ns against a gap measured in hundreds of µs; the rest is an architecturally
>   mandated host-side VM exit, not guest code. See §5. This makes §§2-3 (Idea 2, Idea 3) *more*
>   load-bearing, not less: they're the only remaining levers.
> - **New Idea 4** (§6): recover the ~30-37% of waiters currently excluded from halting at all
>   because they enter with IRQs already disabled.
> - **New §7**: cross-hypervisor portability findings (Hyper-V/Xen/KVM all inherited from generic
>   `safe_halt()`+IPI cleanly except two specific guest sub-modes) and the framing that resolves
>   this project's "zero PV" language into something precise enough to defend to a reviewer.
> - **New §8**: a live-testing safety rule, added after a background investigation agent crashed
>   this guest by synthesizing interrupts on a vector the kernel's own SMP machinery was using.
>   Read before anyone runs another live experiment on this box.

This closes out the 2026-09-04/05 investigation (`ivh_two_enhancement_designs_2026-09-04.md`,
`ivh_adaptive_spinning_glock13_findings_2026-09-03.md`) into IVH's persistent ~5-8% wall-clock
regression vs stock PV spinlock, and answers directly: **which of the proposed fixes should
actually be built, in what order, and how do we know each stage is doing what it's supposed to
before trusting a wall-clock number.**

---

## 0. The decisive test that just ran

**Hypothesis under test ("sticky vs lossy wake")**: stock's wake (`KVM_HC_KICK_CPU`) sets a
latching flag (`pv_unhalted`) — a kick that arrives before the target halts is remembered and
the next HLT returns immediately. Mechanism 2's wake is a plain `smp_send_reschedule()` IPI —
not latching — so an early kick is lost and the halt is only rescued by the next scheduler tick
(bounded ~1ms on this `nohz=off`, `HZ=1000` guest). If this were the dominant cost, restoring the
latching hypercall (`ivh_pv_kick_pure_ipi=0`, already the kernel's own default — see correction
below) should collapse mechanism 2's per-halt cost toward stock's.

**Instrumentation added to test it** (`cvm_setup/hackbench_tier2_isolation.sh`,
`cvm_setup/hackbench_tier2_stats.py`, `scratchpad/cycle_snapshot_v3.bt`): per-arm delta of
`ivh_lock_halt.hlt_cycles`/`hlt_events` (`arch/x86/include/asm/ivh_tsc_beat.h`), converted to
mean microseconds per real HLT at 2200MHz TSC. This is the correct metric — wall-clock is far too
noisy (~5.4% CI) to resolve a per-halt-cost effect directly.

**Result, 5 clean paired rounds, `hackbench -T -g 1 -f 8 -l 400000`:**

| arm | us/halt | vs D |
|---|---|---|
| D (stock) | **128.5** | — |
| G0 (mech2, tier-2 off) | 738.0 | 5.7x |
| H1 (mech2, tier-2 @100us) | 741.5 | 5.8x |
| **G0K** (G0 + latching hypercall restored) | **594.6** | **4.6x** |
| H1K (H1 + latching hypercall restored) | 611.4 | 4.75x |
| G0N (G0 + node-unlock IPI restored — different knob, not a sticky-wake test) | 741.2 | 5.8x |

Paired deltas: G0K−G0 = **−19.4%** (t=−29.6, 5/5 rounds negative — real, not noise), H1K−H1 =
**−17.5%** (t=−8.8, 5/5 negative). G0N−G0 ≈ 0 (confirms it tests something else, per the Fix 1
build plan's correction below).

**Verdict: hypothesis falsified as originally scoped.** The go/no-go bar (Fix 1 build plan,
§1 below) was "G0K within 20% of D, no-go above 1.5x D." G0K landed at 4.6x D — nowhere close.
Restoring the latching wake recovers a real, reproducible ~17-19%, not the collapse that would
close most of the gap. **One variant remains untested and is the immediate next cheap check**:
hypercall-*only* (`ivh_pv_kick_unlock_ipi=0` in addition to `kick_pure_ipi=0` — call this arm
`G0KX`), which removes the IPI's interrupt-entry/ISR/EOI/IRET cost entirely instead of paying it
on top of the hypercall as G0K/H1K do. Until G0KX is run, Fix 1 is "mostly falsified, not fully
closed out."

**What this means for priority**: idea 2 (capped at ~11.7% of halts) and idea 3 (capped at
~1-3 regression points) were sized against an assumption that most of the gap was wake-vehicle
cost. That assumption just took a hit. All three are still worth building — none of them are
mutually exclusive, and the user has asked to build all three — but expectations should reset:
**no single one of these is likely to close the gap alone; the honest plan is to build, measure,
and stack all three, in the cheapest-first order below.**

---

## 1. Fix 1: Sticky (latching) wake for mechanism 2

### Code reality check (read before anything else)

Two corrections to the original framing, from actual source
(`/root/kernels/linux-6.17-vanilla`):

1. **`ivh_pv_kick_pure_ipi=0` is already "hypercall AND IPI," not "instead of."**
   `ivh_pv_kick()` (`arch/x86/kernel/kvm.c:1717`) gates the two vehicles independently: the
   hypercall fires at `kvm.c:1789-1793` when `PV_UNHALT && (!pure_ipi || !unlock_ipi)`; the IPI
   fires separately at `kvm.c:1806-1809` when `unlock_ipi`. With the **kernel default**
   `pure_ipi=0` (`kvm.c:1135`) and `unlock_ipi=1` (`kvm.c:1163`), both already fire. So
   `G0K`/`H1K` are not "restore the hypercall instead of the IPI" — they are "stop suppressing
   the hypercall." The suppression is injected by the *harness*, at
   `cvm_setup/hackbench_tier2_isolation.sh:248` (`configure_mech2_base` sets `kick_pure_ipi 1`)
   — every G0/G1/H1/H2 arm run throughout this whole investigation has had the hypercall
   deliberately suppressed.
2. **`G0N` does not test sticky-wake at all** (confirmed by today's G0N−G0 ≈ 0 result). It sets
   `ivh_pv_kick_node_ipi=1` (harness:283-286), which controls the *node-site* IPI in
   `pv_kick_node()` (`kernel/locking/qspinlock_paravirt.h:770-771`) — a site that sends **only**
   `smp_send_reschedule()`, never a hypercall.

The naive lost-wake race is *already* closed in the existing code: `pv_kick_node()` publishes
`VCPU_HASHED` before its IPI, the unlock slowpath clears `lock->locked` before `pv_kick()`, and
`ivh_pv_wait()` rechecks `*ptr` with IRQs off before `safe_halt()` (`kvm.c:1660-1670`). Today's
result confirms the measured 5.7-5.8x per-halt gap is mostly **wake-retirement cost** (IPI = full
interrupt entry/ISR/EOI/IRET on the target; the hypercall retires the HLT with no ISR at all) and
only ~19% of it is the lost-edge/tick-rescue tail this fix targets.

### Stage 0 — decision gate (RESOLVED by today's test)

Go criterion was G0K within 20% of D, reproduced x2. **Result: 4.6x D. No-go**, as scoped. One
variant remains: test `G0KX` (hypercall-only, no IPI at all) before fully closing this out —
see below.

### Stage 1 — minimal permanent fix (still worth doing, scope reduced)

Even at only ~19%, this is a free, already-safe, real win with zero new risk. Ship it:

1. **Harness/config**: delete `set_sysctl ivh_pv_kick_pure_ipi 1` from `configure_mech2_base`
   (harness:248); mech-2 arms inherit the kernel default 0. `G0K`/`H1K` become redundant with
   `G0`/`H1` and should be dropped from the arm list.
2. **Kernel**: reword `ivh_pv_kick_pure_ipi`'s comment (`kvm.c:1128-1135`) from "opt-out" to
   "diagnostic-only; the shipped mechanism-2 wake is hypercall + IPI." No default value change
   needed — the kernel default was already correct; only the test harness was overriding it.
3. **Test the stronger, untested form**: add arm **G0KX** = `pure_ipi=0, unlock_ipi=0`
   (`ivh_pv_proc_kick_unlock_ipi()`, `kvm.c:1327-1345`, already permits this whenever `PV_UNHALT`
   is advertised — no new sysctl needed). This removes the ISR cost entirely rather than paying
   it alongside the hypercall. **Run this before writing anything else in this document** — it's
   a one-line harness change (`configure_G0KX() { configure_G0; set_sysctl ivh_pv_kick_unlock_ipi 0; }`)
   and the same `hlt_cycles`/`hlt_events` metric already wired up. If G0KX collapses toward D,
   ship `ivh_pv_kick_unlock_ipi` default 0 (`kvm.c:1163`) — a one-token patch — and mechanism 2's
   wake becomes byte-identical to stock's. If it doesn't, Fix 1 is fully closed out at "ship the
   free 19%," and the remaining gap genuinely lives elsewhere.

Do **not** add a hypercall to `pv_kick_node()`: that site is on the acquirer's own critical path
(`qspinlock_paravirt.h:757-768`) and stock sends nothing there.

### Stage 1 debug instrumentation

Per-CPU counters (no printk — `ivh_pv_wait_trace`, `kvm.c:1100-1115`, is reserved for the IF=0
freeze class):

- `ivh_kick_node_hashed` / `ivh_kick_node_running` — either side of the
  `try_cmpxchg_relaxed` at `qspinlock_paravirt.h:716`. Separates "target already halted" from
  "target still running" at kick time.
- `ivh_wake_before_halt` — in `ivh_pv_wait()`'s recheck-failed branch (`kvm.c:1668`, "condition
  cleared before halt"). This is the population the fix actually saves.
- `ivh_halt_tick_rescued` — in `ivh_lock_halt_end()` (`ivh_tsc_beat.h:283-304`), increment when a
  HLT-bucket `delta` exceeds ~500µs (half a tick). The residual failure mode after the fix.
  Ship a small log2 histogram (`ivh_halt_dur_hist[]`) alongside so the ~1ms mode is visible
  directly rather than inferred.

### Stage 2 — validation

Re-run the harness with **D, G0, G0KX, H1** (drop G0K/H1K once G0KX's result is in), ≥10 paired
rounds, no co-tenant VM on the host. Success: `H1 − D` wall-clock delta shrinks and
`ivh_halt_tick_rescued/hlt_events < 1%`.

### Stage 3 — regression check

Watch for the hypercall's own cost landing on the *releaser* instead — `ivh_pv_hypercall_kick()`
(`kvm.c:1527`) is a TDX VMCALL, not free. If G0KX's wall-clock is worse than G0's despite a lower
per-halt cost, the hypercall is costing the releaser more than the ISR cost the waiter — check by
comparing `ivh_pv_wait_calls`-normalized wall-clock across G0/G0K/G0KX in the same run (no new
instrumentation needed), and watch `/proc/interrupts` RES drop toward stock's baseline.

---

## 2. Idea 2: Head-role takeover

**Setup**: holder → W1 (queue head, `pv_wait_head_or_lock()`, `qspinlock_paravirt.h:782`) → W2
(first MCS node, `pv_wait_node()`, `:504`). W2 already detects W1 looks stale via the existing
tier-2 TSC-heartbeat check. Today it can only wait longer. The fix: W2 atomically takes over the
head role via a CAS on a new dedicated control word on W1's node when the real lock is
observably free; W1, if superseded on waking, simply re-queues from scratch. Safe because a
node's *predecessor* is always safe to dereference (nothing else is still waiting to hand it
anything), while its *successor* is not — this asymmetry is why a naive mid-queue swap (rejected
earlier this session) doesn't work but a head-only version does.

**Ceiling, already measured**: head halts are ~11.7% of all halts across mechanism-2 arms
(`ivh_halt_from_head` vs `ivh_halt_from_node`, both split by arm in today's harness). Even total
elimination of head halts can't plausibly move wall-clock past this rig's ~5.4% noise floor
*on its own* — the realistic case for this idea is stacked with Fix 1, not standalone.

### Field layout (fixed once, Stage 0)

Exactly 8 spare bytes exist in `struct pv_node`/`struct qnode` (`qspinlock_paravirt.h:59-63`,
`qspinlock.h:40-45`, confirmed via the `BUILD_BUG_ON` at `:474`) — room for exactly one `u64`,
no more:

```c
/* qspinlock_paravirt.h, in struct pv_node after `u8 state;` (:62) */
	u64	head_ctl;	/* {gen:32 | yields:16 | state:16} */
#define HEAD_IDLE 0
#define HEAD_ARMED 1
#define HEAD_YIELDED 2
#define HC(gen, y, st) (((u64)(gen)<<32) | ((u64)(y)<<16) | (st))
```

`yields` is carried in the word because the Stage-3 starvation cap is enforced by **W2**, which
cannot otherwise see W1's yield streak. The generation field is what makes the CAS immune to a
W1 that woke, requeued, and re-armed before W2's attempt lands — this is exactly why `pn->state`
(which already cycles HASHED/RUNNING/HASHED) was rejected as the CAS target in the original
design pass.

**Load-bearing invariant, found during this build-plan pass**: `__pv_queued_spin_unlock_slowpath()`
calls `pv_unhash(lock)` (`:924`) **before** `smp_store_release(&lock->locked, 0)` (`:930`). So
W2's `!lock->locked` test isn't just "is takeover useful" — **observing `locked == 0` is the
proof that W1's hash entry has already been retired**, which is what makes it safe for W1 to
skip the `*lp` unwind on a yielded wake. The freeness test and the hash-safety proof are the same
check; say so in the patch comment.

### Stage 0 — instrumentation only, zero logic change

New per-CPU counters (`kvm.c`, declared in `ivh_tsc_beat.h:131-132`): `ivh_head_arm`,
`ivh_head_yield_try`, `ivh_head_yield_ok`, `ivh_head_yield_nolock`, `ivh_head_yield_caslost`,
`ivh_head_woke_yielded`, `ivh_head_woke_moot`, `ivh_head_requeue`.

- `pv_init_node()` (`:472-500`): `pn->head_ctl = HC(0,0,HEAD_IDLE);`
- `pv_wait_head_or_lock()`, right before `pv_wait()` at `:868`: bump generation, set `HEAD_ARMED`,
  `this_cpu_inc(ivh_head_arm)`. Right after `:868` returns: if state is `HEAD_YIELDED`, count
  `ivh_head_woke_yielded`; else reset to `HEAD_IDLE` and count `ivh_head_woke_moot`.
- `pv_wait_node()`, between `:645` and `:647`, **observe-only**: if tier-2 fired and W1's
  `head_ctl` is `HEAD_ARMED`, check (don't act on) `lock->locked` and count
  `ivh_head_yield_try`/`ivh_head_yield_ok`/`ivh_head_yield_nolock` accordingly.
- This already requires the `lock` argument on `pv_wait_node()` — change its signature
  (`:504`), the native stub (`qspinlock.c:89-90`), and the call site (`qspinlock.c:291`). This
  3-line generic-file change is independently buildable and boots fine under
  `CONFIG_PARAVIRT_SPINLOCKS=n` too.

**Stage 0 acceptance**: `ivh_head_arm ≈ ivh_halt_from_head` (same site — a >0.1% deviation means
the arm is misplaced). `ivh_head_woke_yielded == 0` (nothing sets it yet). `ivh_head_yield_ok`
is the **opportunity rate** — how often the mechanism would fire. Wall-clock must be
indistinguishable from H1 (Stage 0 changes no behavior).

### Stage 1 — enable the takeover

New sysctl `ivh_head_yield_max` (mirrors `ivh_pv_rearm_max`'s pattern exactly): `0` = off
(Stage-0 behavior), `ULONG_MAX` = uncapped, finite N = Stage-3 cap. Replace the observe-only
block with the acting version: on tier-2 fire against an armed, lock-free head, try the full CAS
(carrying the whole word — see the ABA note above); on success, `smp_store_release(&node->locked, 1)`
and return (this hands control to the *existing* generic code path that already promotes "the
node whose `->locked` got set" to head — no new promotion plumbing needed).

`pv_wait_head_or_lock()`'s `HEAD_YIELDED` branch returns a new sentinel `_Q_REQUEUE_VAL`
(`#define` near `:33`, chosen so it can never collide with a genuine lock value — a real
acquisition always ORs in `_Q_LOCKED_VAL`). **Do not touch `*lp`** on this path — see the
invariant above.

Generic `qspinlock.c`: a `pv_requeue:` label right after `grab_mcs_node()` (after the node-count
increment, before node re-init), and one new branch after the `pv_wait_head_or_lock()` call site
(`:326`) that jumps there on `_Q_REQUEUE_VAL`. This is the **second and last** generic-file
touch (~2 lines total across both stages).

**Stage 1 validation — required counter ratios, not wall-clock**:

- `ivh_head_yield_ok ≤ ivh_head_yield_try ≤ ivh_head_arm ≈ ivh_halt_from_head`
- `ivh_head_yield_ok == ivh_head_woke_yielded == ivh_head_requeue`, cluster-wide, within one
  in-flight event per CPU — these three counters are written by three *different* vCPUs for the
  same logical event, so a persistent gap means a lost wakeup or a double-yield.
- `ivh_head_woke_moot / ivh_head_arm` — report this in every run. It's the confound guard that
  keeps idea 2's effect separable from Fix 1 and idea 3 once more than one is built.
- **Corruption sentinels that must never fire** (already free): `pv_hash()`'s `BUG()` (`:239`)
  and `pv_unhash()`'s `BUG()` (`:270`) are exactly the duplicate/lost-hash-entry detectors. Add
  one new `WARN_ON_ONCE` at the arm site: arming over an already-`HEAD_YIELDED` word means two
  W2s claimed one head.

### Stage 2 — requeue-path stress test

hackbench spreads across too many locks to exercise the requeue path hard. Use a synthetic
driver instead: one `DEFINE_SPINLOCK` in a debug module, `2 × nr_vcpus` kthreads, ~200ns critical
section, pinned, 60s, `ivh_pv_beat_threshold` lowered to force a high tier-2 fire rate. Target
`ivh_head_requeue > 10^5`. Add two more `WARN_ONCE`s outside the halt path: in `pv_kick_node()`,
a kick landing on an already-yielded head; at `pv_requeue:`, a non-NULL `node->next` (would mean
someone linked in *after* the yield — a direct violation of the backward-safe/forward-unsafe
asymmetry this design depends on). Give the driver its own userspace-visible progress-counter
watchdog — a lost-wakeup bug here manifests as a hang, not a WARN.

### Stage 3 — starvation cap

Ship `ivh_head_yield_max` finite by default once tuned (every yield sends W1 to the queue tail;
FIFO is qspinlock's only starvation guarantee, `qspinlock_paravirt.h:76-87`). Add a yield-count
histogram (clone of `ivh_node_rearm_hist`) and a running per-CPU max streak. Sweep
N ∈ {1, 2, 4, ULONG_MAX}; pick the smallest N retaining ≥90% of the uncapped arm's
`ivh_head_yield_ok` volume.

### Stage 4 — performance validation

New harness arms, control is **H1 itself** (`ivh_head_yield_max=0`) so the paired-round
randomization does the statistical work:

```sh
configure_J1()  { configure_H1; set_sysctl ivh_head_yield_max 18446744073709551615; }
configure_J1C() { configure_H1; set_sysctl ivh_head_yield_max 4; }
```

Pre-registered endpoints, given the known 11.7% ceiling:

- **Primary (well-powered)**: `head_halts` in J1 must fall ≥50% vs H1 — a ~6% shift in total
  halts, far outside counter noise.
- **Secondary**: `node_halts + head_halts` combined must not *rise* (a yield only helps if W2
  would have halted anyway).
- **Tertiary (wall-clock)**: >5.4% paired median difference across ≥9 rounds to count as a real
  win; anything smaller should be reported honestly as "mechanism confirmed, wall-clock effect
  below noise floor," not oversold. **The realistic wall-clock case for this idea requires
  stacking with Fix 1** — report `ivh_head_woke_moot` every time to keep that honest.

---

## 3. Idea 3: Adaptive self-wake

**What it does**: a halted mechanism-2 waiter today wakes only via IPI or the ~1ms tick backstop.
Idea 3 arms a shorter, self-programmed one-shot deadline before halting, sized from a learned
per-vCPU estimate of "how long does my own preemption episode usually last." Ceiling already
established: D and G0 spend roughly equal total vCPU-time halted, so this caps out around
**1-3 of the 5-8 regression points** even if it works perfectly — a real but partial win, and
today's Fix-1 result (structural per-event cost, not mostly lost wakes) doesn't change that
ceiling math, since idea 3 attacks wait *duration* by self-terminating early, a different lever
than either Fix 1 or idea 2.

### Corrections found during this build-plan pass

- **`lapic_next_event()` is not directly callable as originally proposed.** It's `static`
  (`arch/x86/kernel/apic/apic.c:413`) and takes raw APIC-timer ticks via `apic_write`, not
  ns/TSC. If this guest runs TSC-deadline mode, the installed op is actually
  `lapic_next_deadline()` (`apic.c:420`, an absolute-TSC `wrmsrq`) — check
  `/proc/timer_list`'s `set_next_event:` symbol to confirm which. **Use the real, lock-free
  public entry point instead**: `clockevents_program_event(dev, expires, force)`
  (`kernel/time/clockevents.c:303`) — no locks taken, but it calls `ktime_get()`, clamps to
  `min_delta_ns`/`max_delta_ns`, and is what makes the "self-healing on spurious fire" property
  actually true (because `hrtimer_interrupt` reprograms on entry). Get `dev` via
  `this_cpu_ptr(&tick_cpu_device)->evtdev`; bail if `!clockevent_state_oneshot(dev)`.
- **`struct ivh_tsc_beat` genuinely has 56 free bytes** (a single `u64 stamp` in a
  `____cacheline_aligned_in_smp` line, `ivh_tsc_beat.h:63-65`) — adding `gap_ema_q` costs no
  cacheline growth, unlike `struct pv_node`'s tight 8-byte budget used by idea 2. Confirmed, not
  assumed.
- Alpha `868/65536` is `ivh_uc_ema_alpha_q16` (`kernel/sched/bpf_sched.c:103`), tied to a fixed
  200ms window (`ivh_uc_window_ns`) — genuinely the wrong constant to reuse for an
  event-triggered signal; use a dedicated, faster `ivh_pv_gap_alpha_q16` (default 8192 = 1/8).
  Cold-start precedent: `ivh_uc_close()`'s assign-don't-blend on first sample
  (`kernel/sched/core.c:388-393`).
- 2026-07-24 IF=0-HLT invariant: `kvm.c:1103-1114`, `:1622-1650`. GLOCK-9 "never early-`continue`
  past the state store" bug: `qspinlock_paravirt.h:601-620`. Both must be respected exactly as
  documented — this design does not touch either invariant, it only adds an arm call between the
  existing IRQ-off recheck and `safe_halt()`.

### Stage 0 — measure the one blocking unknown FIRST (before any wait-path code)

A small kernel module, run on this TDX guest specifically (not assumed from docs): on an
isolated CPU with IRQs off, time N=10^5 calls to
`clockevents_program_event(evtdev, ktime_add_ns(ktime_get(), 10ms), false)` with the deadline
deliberately far beyond the tick so nothing ever fires; report min/median/p99 cycles. This
answers whether the WRMSR is vmexit-class under TDX's MSR mediation — the single fact this whole
idea's cost/benefit hinges on.

**Go/no-go**, against numbers already established this session (`IVH_PV_TPAUSE_CYCLES=512`,
`min_delta_ns=9604ns`≈21,000 cycles, tsc≈2.2GHz):

- **< 5,000 cycles**: GO — negligible next to the ~1.1M-cycle tick rescue being shortened.
- **5,000-20,000**: marginal — proceed only if Stage 1 shows most real gaps are ≫100µs.
- **> 21,000 cycles**: NO-GO — the arm would cost more than the shortest deadline it can even
  program; abandon, don't tune.

### Stage 1 — EWMA signal only, no wait-path change

Add `u64 gap_ema_q` to `struct ivh_tsc_beat` (`:63`). In `ivh_tsc_beat_publish()`, compute the
gap since this CPU's own last stamp, fold it into the EMA only if it exceeds a new
`ivh_pv_gap_min_cycles` gate (seeded ~2x the tick period, since ordinary tick-cadence publishes
must not be misread as preemption episodes), using the exact Q16 form `ivh_uc_ema()` already
uses but with the new, independent alpha. `gap_ema_q == 0` is the untouched cold sentinel.

Debug: per-CPU min/max/sample-count plus a log2 histogram matching
`ivh_beat_age_hist_*`'s existing bucketing, dumped via a `kaddr()`-based bpftrace script in the
same style as `scratchpad/cycle_snapshot_v3.bt`. Sanity gate: values should land in the same
range as real preemption episodes (`ivh_beat_age_hist_preempted`'s buckets) — if the EMA tracks
~1ms, the min-gap gate is leaking ordinary tick cadence and needs raising.

### Stage 2 — plumbing only, no behavior change

Stash the predecessor's CPU in a per-cpu scratch var immediately before `pv_wait_node()`'s halt
call (`:653`), cleared after (`:661`) and before the head's own `pv_wait()` call (`:868`, which
has no predecessor and must fall back to its own CPU's EMA or the plain tick). Do not widen
`pv_wait()`'s fixed pvops signature.

In the mechanism-2 halt branch (`kvm.c:1661`), after IRQs are disabled and the lost-wakeup
recheck passes, compute what X *would* be from the predecessor's EMA — but arm something
deliberately beyond the tick (never a real early wake yet), just to validate the plumbing and
WRMSR call succeed and land where expected. Counters: arm attempts, cold/short skips, arm
errors. Confirm via `/proc/interrupts` LOC deltas and `/proc/timer_list` that no unexpected
reprogramming storm occurs.

### Stage 3 — enable for real

New sysctls: `ivh_pv_selfwake` (default 0), `ivh_pv_selfwake_num`/`_den` (default 1/2, a
read-time bias applied on top of the stored EMA — keeps the stored value itself a plain,
inspectable mean), `ivh_pv_gap_alpha_q16`, `ivh_pv_gap_min_cycles`. Program the real deadline.
Classify every halt exit: condition already cleared (true positive — the actual win), still set
but woken before the deadline (ordinary IPI/tick path, unaffected), still set at/after the
deadline (**wasted self-wake** — the idea's main cost, track explicitly). The self-wake must
return through the exact normal bottom of the function so the existing
`HALTED→HASHED`/`HALTED→RUNNING` cmpxchg protocol always runs — this is GLOCK-9's lesson
verbatim, and it's why no new wake-race synchronization is needed at all: the protocol already
handles "woken by something other than an explicit kick" on every ordinary tick-rescued halt
today.

### Stage 4 — knob sweep

New `cvm_setup/selfwake_sweep.sh`, structurally cloned from `threshold_sweep.sh` (same
read-back-and-abort `set_sysctl()`, same restore-on-exit trap). Grid:
`selfwake_num/den ∈ {1/4, 1/2, 3/4, 1/1} × gap_alpha_q16 ∈ {4096, 8192, 16384}`, `selfwake=0`
control repeated first and last to bound host drift. Primary metric: wasted-self-wake rate from
Stage 3. Secondary: `hlt_cycles`/`hlt_events`.

### Stage 5 — validation and honest ceiling

New `S` arm = H1 + `ivh_pv_selfwake=1`, compared **S vs H1 only** (same mechanism), never
directly against D. Expectation, restated so it doesn't get oversold: this session's
vCPU-halted-time accounting bounds the addressable regression at **~1-3 percentage points**, not
most of the 5-8% gap. A correct, fully-working implementation should recover single digits, not
close the gap on its own — anything larger is more likely measurement artifact than mechanism,
and anything under ~1pp needs the full paired-round design above to even see through this rig's
~16-17% single-round CV.

---

## 4. Overall sequencing recommendation

Cheapest-and-most-falsifying first, per the original test-plan agent's own ordering principle,
updated with today's result:

1. **Run G0KX today** (one-line harness addition, ~25 min) — the one remaining untested variant
   of Fix 1. Closes out Fix 1's decision either way before any new kernel code is written.
2. **Ship Fix 1's confirmed ~19% win regardless** (Stage 1, harness default fix + instrumentation)
   — it's free, safe, and already proven.
3. **Idea 2, Stage 0 only** (instrumentation, zero logic change, ~3-line generic-file touch) —
   gives the real opportunity-rate numbers with no risk, before committing to the takeover logic.
4. **Idea 3, Stage 0 only** (the TDX timer-arm cost microbenchmark) — this is the one hard
   go/no-go gate in the whole plan; if it fails, idea 3 is dead before any wait-path code exists.
5. Based on 3 and 4's results, proceed into Idea 2 Stage 1 / Idea 3 Stage 1 in whichever order
   the opportunity numbers favor — both are independent of each other and of Fix 1, and all
   three are designed to be stacked (with the confound-guard counters called out in each section)
   rather than chosen between.

None of steps 1, 3, or 4 requires committing to any of the larger builds — each is a bounded,
cheap, falsifying check first, exactly matching how this whole investigation has proceeded.

**2026-09-06 addendum to this sequencing**: step 1 (`G0KX`) is now moot — see §5, the hypercall
path is closed under the mode collapse regardless of what it would have measured. Steps 2 (Fix 1
ship) is dead for the same reason. Steps 3-4 (Idea 2/3 Stage 0 instrumentation) still stand
exactly as written. Insert **Idea 4 Stage 0** (§6) alongside them — same cheap,
instrumentation-only shape, same reasoning for going first.

---

## 5. Wake-mechanism cost: fully investigated and closed out, 2026-09-06

This closes the question Fix 1 (§1) was trying to answer, but with a different and more complete
answer than "restore the hypercall."

### The 5.7x number was probably never a clean "servicing cost" measurement

`hlt_cycles/hlt_events` measures **wall time asleep**, not cycles spent servicing the wake. Two
confounds inflate it that §1 didn't separate out:

- **Population mismatch.** Mode VANILLA halts on both the IF=1 and IF=0 code paths in
  `ivh_pv_wait()`. Modes PURE_IPI/ADAPTIVE refuse to halt at IF=0 at all (the "IRREDUCIBLE gap,"
  `ivh_wait_irqoff_nohalt`) and busy-spin instead — an earlier measurement put that population at
  ~37% of hackbench's waits (the short-critical-section, `*_irqsave` population). Comparing a
  mean over one population against a mean over a *different* population isn't a clean per-wake
  comparison.
- **Lost/late wakes rescued only by the ~1ms tick**, not per-wake cost — this is closer to what
  §1's own G0K/H1K result (a real but only ~17-19% recovery from restoring latching) was actually
  measuring.

**`ivh_wait_irqoff_nohalt` and `ivh_beat_halt_from_head/node` already exist in
`arch/x86/include/asm/ivh_tsc_beat.h`/`kvm.c` and have never once been reported in any run this
whole investigation.** Reporting them is free (zero new code) and should happen before trusting
any future per-halt-cost number, including a rerun of anything in §§2-3.

### REMRD: investigated in depth, rejected — do not revisit

The obvious "make the wake as cheap as the hypercall" idea: have `ivh_wake()` construct its own
raw x2APIC ICR write using `delivery_mode = APIC_DM_REMRD` (0x300) instead of the standard
`APIC_DM_FIXED` that `smp_send_reschedule()` builds. `APIC_DM_REMRD` is exactly what
`KVM_HC_KICK_CPU`'s handler uses (`arch/x86/kvm/lapic.c:1304`, `__apic_accept_irq()`) to skip all
vector-injection/ISR/EOI/IRET cost — just flips `pv_unhalted` and kicks the vcpu thread directly.
Confirmed empirically reachable on this host (live MSR-latency timing showed ICR access lands in
KVM's software-emulated tier, ~6.65µs, and KVM's own ICR-write path
(`kvm_apic_send_ipi()`/`kvm_x2apic_icr_write()`) does zero delivery-mode validation).

**Rejected anyway, for reasons independent of feasibility:**
- `APIC_DM_REMRD` ("Remote Read") is an **officially retired x86 delivery mode** — real hardware
  dropped it starting with the P6 family/Pentium 4; the current Intel SDM lists `011b` simply as
  Reserved. Confirmed against Intel's own SDM Vol. 3A.
- What KVM does with it is an **undocumented KVM-internal convention**, not an architectural
  guarantee. Nothing requires any other hypervisor (Hyper-V, VMware, Xen) to treat a reserved
  delivery mode the same way — best case it's silently ignored (no benefit), worst case some
  stricter guest-MSR-write validator on a different platform injects a `#GP` fault back at the
  guest for an out-of-spec value — a crash risk, not just a missed optimization, and untestable
  from source alone.
- Given this project's hard requirement to run correctly on arbitrary cloud/VM hardware, this
  fails the bar even though it technically works here. **Closed. Do not build.**

### Final verdict on trimming the wake mechanism itself: no further optimization exists

A dedicated investigation (live disassembly of the running kernel via `/proc/kcore`, plus
read-only MSR timing — no synthetic interrupts fired, see §8) priced every guest-controllable
piece of one wake:

| Component | Cost | Removable? |
|---|---|---|
| tracepoint | already patched to a 2-byte NOP | — |
| `smp_ops` indirect call + `cpu_is_offline()` check | ~5ns | yes, but worthless |
| `mfence;lfence` before the ICR write | ~18ns | **no — genuine correctness requirement**, not overhead (orders the `smp_store_release(&lock->locked,0)` before the WRMSR; this guards the exact same class of lost-wake bug this whole investigation has been careful about elsewhere) |
| the ICR write itself | 0.1µs (hardware-accelerated) or 6.65µs (host-software path) — **decided by the host/hardware, not guest code** | no |
| receiving side's minimal ISR work | ~2ns | yes, but worthless |
| entry stub / IRET | identical cost for any vector | no |

Total guest-controllable budget: **under 50 nanoseconds**, against a gap measured in hundreds of
microseconds. IVH's current wake is also already in the single most hardware-accelerable encoding
(`FIXED`/physical/single-target) — a hand-rolled version would produce a bit-identical ICR value.
**There is nothing left to trim here, by any means that stays portable.** This is a real
"stop looking" result, not "nothing found yet" — treat §§2, 3, 6 as the only remaining levers,
and stop sizing them against a possibly-inflated §1-era gap estimate until §5's free
instrumentation fix is done.

---

## 6. Idea 4: recover IRQ-disabled waiters

**What it targets**: today, a waiter that enters `ivh_pv_wait()` with IRQs *already* disabled at
entry (typically via an outer `spin_lock_irqsave()`) never halts at all — it falls straight
through to a `cpu_relax()` busy-spin loop (`ivh_wait_irqoff_nohalt`), because calling
`safe_halt()` there would side-effect-*enable* interrupts, silently breaking the caller's own
irqsave contract the moment control returns to them. This is the "IRREDUCIBLE gap" comment in
`ivh_pv_wait()` — an earlier measurement (GLOCK-13) put this population at roughly **37% of
hackbench's waits**, entirely excluded from adaptive spinning today, in every mode.

**The fix**: don't avoid the halt — restore the caller's original IRQ state afterward, rather
than leaving it flipped. Concretely:

1. Record whether IRQs were disabled at entry to `ivh_pv_wait()`.
2. Regardless of that state, do exactly what the existing `!irqs_disabled()` branch already does:
   `local_irq_disable()` (a no-op if already disabled), recheck the wait condition, then
   `safe_halt()` — which atomically re-enables IRQs and halts with IF=1 guaranteed, meaning a
   plain maskable IPI wakes it correctly, no hypercall/NMI-class wake needed.
3. Immediately after `safe_halt()` returns — **before returning control to the caller** — if the
   entry state was disabled, call `local_irq_disable()` again to restore it.

The caller never observes a difference: they get IRQs back in exactly the state they left them
in. The only actual change is that for the *duration of the wait itself* (not the caller's actual
critical section, which hasn't started yet), interrupts are genuinely open.

**Correctness risk to audit, not assume away**: the classic reason a lock is taken with
`_irqsave` is to prevent a CPU from deadlocking against itself — an interrupt fires while
*holding* the lock, and its handler also wants that same lock. That specific hazard is about
holding, not waiting: a waiter that takes an interrupt mid-wait, whose handler also wants the same
lock, just becomes another queued waiter — not an obvious deadlock, since whoever currently holds
the lock still releases it regardless of what's happening on other paths. But this needs
verifying against real call sites, not assumed universally safe — audit a representative sample
of `_irqsave` call sites on contended locks before shipping, the same way GLOCK-9's
early-`continue`-past-the-state-store bug and the 2026-07-24 IF=0-hard-freeze class both turned
out to need call-site-level care rather than a blanket rule.

**Staging** (mirror Idea 2/3's shape): Stage 0 — instrumentation only (a new counter distinguishing
"entered with IRQs off, would now halt instead of spin" from today's `ivh_wait_irqoff_nohalt`,
zero behavior change) to get the real opportunity-rate number (should be close to the ~37% figure)
before writing the actual restore-state logic. Stage 1 — implement behind a new sysctl
(`ivh_pv_irqoff_halt`, default 0, matching the on/off gating pattern every other IVH knob uses).
Stage 2 — the correctness audit above, plus a `WARN_ONCE` if IRQ state on return from
`ivh_pv_wait()` ever doesn't match entry state (a cheap, permanent regression guard). Stage 3 —
paired-round wall-clock validation against the relevant mode with this sysctl off as control.

This is a **distinct lever from Idea 2 and Idea 3** — it doesn't touch wake frequency, wake cost,
or wait duration; it expands *which waiters are eligible to halt at all*. Combine its ceiling
estimate with Idea 2/3's carefully — all three ultimately compete for the same limited wall-clock
budget, and (per §5) none of them get help from any further wake-cost trimming.

---

## 7. Cross-hypervisor portability and the "avoidable vs. unavoidable" framing

This section is paper-framing material as much as build-plan material, but belongs here because
it directly bears on what Ideas 2/3/4 are even being built *for*.

### The map, confirmed by source on this exact tree

| Platform/mode | `safe_halt()` resolves to | kick |
|---|---|---|
| Hyper-V | native `sti;hlt` (Hyper-V never overrides `pv_ops.irq.safe_halt`) | real hardware IPI (`hv_qlock_kick` → `__apic_send_IPI`) |
| Plain KVM (no TDX) | native `sti;hlt` | real hardware IPI |
| Xen HVM/PVH | native `sti;hlt` (`enlighten_hvm.c` never touches `pv_ops.irq`) | real hardware IPI |
| **KVM + TDX** | **mandatory `TDCALL`** (`tdx_safe_halt`, wired in unconditionally by `tdx_early_init()`) | real hardware IPI |
| **Xen classic PV** | **mandatory hypercall** (`xen_safe_halt` → `HYPERVISOR_sched_op(SCHEDOP_block,...)`, wired in only by `enlighten_pv.c`'s `xen_init_irq_ops()`) | Xen event-channel-routed IPI (`xen_send_IPI_one`) |

Key point: it was never "which hypervisor" — it's "which specific guest sub-mode." Exactly two
sub-modes (TDX confidential guests, classic Xen PV guests) force a hypercall for halting; every
other combination this project checked resolves the identical `safe_halt()` call to a genuinely
native instruction. `safe_halt()` + a real IPI is therefore the right, uniform choice across all
five rows — it's just that on two of them, the platform (not IVH's code) has already decided what
that call costs.

Also confirmed: neither Xen nor Hyper-V's own native `pv_kick`/`pv_wait` implementations call
`safe_halt()` at all internally — Xen uses its own event-channel poll hypercall
(`xen_poll_irq`), Hyper-V invented its own idle-signaling MSR (`HV_X64_MSR_GUEST_IDLE`). Kick
converges on "send a real interrupt" independently across all three platforms' own native
implementations; wait does not converge at all. That's independent, cross-vendor confirmation
that IPI is the right choice for wake, and no confirmation at all that any particular sleep
primitive is "the" portable choice other than the generic, always-correct `safe_halt()` wrapper.

### Why the mandatory hypercall (TDX/Xen-PV) is fine, but `pv_kick`/`pv_wait` (KVM_HC_KICK_CPU) isn't — these are different justifications, not the same one

Both `KVM_HC_KICK_CPU` and the TDCALL/`SCHEDOP_block` halt are, in isolation, "one-way" calls —
the guest signals the host, no host-supplied data comes back that the guest needs to trust. If
that property alone were the whole rule, there'd be no reason to exclude `KVM_HC_KICK_CPU` either.
The actual second ingredient is **avoidability**:

- The mandatory halt-hypercall is not a choice this project makes. *Every* piece of kernel code on
  these two guest sub-modes that ever wants to sleep pays this same cost — the idle loop, unrelated
  mutexes, all of it. There is no "don't use this" option if a CPU is ever to stop spinning at all;
  it's a property of the platform, forced on all code equally, before IVH's design enters the
  picture at all.
- `KVM_HC_KICK_CPU` (and Xen's/Hyper-V's own lock-specific PV wait primitives) *is* a choice — an
  optional, purpose-built interface that exists specifically for spinlock wake semantics, doesn't
  exist on other platforms, and can simply not be called (a plain IPI is right there as the
  alternative, and is what PURE_IPI already does).

So: "one-way, no dangerous info" is necessary but not sufficient. The dividing line that actually
does the work is **"unavoidable platform cost" vs. "optional, avoidable, lock-specific shortcut
this project chose to lean on."** Recommended paper framing, tightened from "no PV interfaces" to
match this precisely: *no scheduling hints taken from the host, no lock state disclosed to it, and
no dependence on any optional/negotiated host-cooperation interface — while accepting that a
guest-to-host control signal required by the platform itself, for any code, to relinquish a
physical CPU at all, is not a paravirt dependency this design introduces or could avoid.*

### Open architectural question, not yet resolved

Two live options for how Ideas 2/3/4's logic actually gets deployed across platforms, with a real
trade-off, not a clear winner:

1. **Factor the decision logic (tier-1/tier-2, early-bail, Idea 2/3/4) into one shared,
   hypervisor-agnostic function**, called from each of the ~3 platforms' *existing* `pv_lock_ops`
   registrations (`kvm_spinlock_init()`, `xen_init_spinlocks()`, Hyper-V's `hv_spinlock.c`), each
   supplying its own platform-specific sleep/wake primitive underneath. Transparently upgrades
   every existing kernel spinlock on those three platforms; still gets nothing on bare metal or
   any future platform without a fourth port.
2. **A standalone lock type** (in the shape of I-Spinlock/eCS-style designs) with its own API,
   calling `safe_halt()`+IPI directly, with zero hypervisor-detection anywhere in it — runs
   identically everywhere including bare metal, but only benefits call sites explicitly rewritten
   to use it; doesn't transparently improve any existing `spin_lock()` call in the kernel.

Not resolved this session — worth deciding before Idea 2/3/4's Stage 1 work picks a home, since
where the code lives depends on the answer.

---

## 8. Live-testing safety rule (added 2026-09-06, after a real incident)

A background investigation agent, while measuring per-vector interrupt-servicing cost live on
this guest, fired 3000 synthetic self-IPIs each on multiple real interrupt vectors — including
`CALL_FUNCTION_VECTOR` and `RESCHEDULE_VECTOR` — via direct writes to the x2APIC `SELF_IPI` MSR,
completely bypassing the kernel's own SMP-call-function queueing/locking. This corrupted that
subsystem's live state. Result: multiple CPUs later soft-locked forever inside
`smp_call_function_many_cond`/`on_each_cpu_cond_mask` (triggered by an unrelated process's routine
`madvise()`), kernel tainted `CPU_OUT_OF_SPEC`, guest fully unresponsive, required an external hard
reset.

**Hard rule for any future live experiment on this guest**: read-only or non-delivering MSR access
is fine and has been used safely and productively multiple times this investigation (e.g. the
latency-tier probing that ruled REMRD in/out in §5). **Never fire, trigger, or synthesize a real
interrupt delivery on any vector this kernel currently has wired to a real ISR for real work** —
that includes but isn't limited to `RESCHEDULE_VECTOR`, `CALL_FUNCTION_VECTOR`,
`CALL_FUNCTION_SINGLE_VECTOR`, `IRQ_WORK_VECTOR`, TLB-shootdown vectors, the timer vector, and
anything else already listed in `arch/x86/include/asm/irq_vectors.h`. If a live interrupt-delivery
test is genuinely needed, it must use a verified-unused vector with its own isolated, never-wired-
into-anything-else test handler — never piggyback on a vector anything else in the kernel is
actively using, even to "just measure."

---

## 9. End-to-end closure plan and project value (2026-09-06)

This section takes everything above as settled and answers one question: **from today's real
counter data, what is the complete remaining path to closing the VANILLA ↔ PURE_IPI/ADAPTIVE gap,
in dependency order, and how far can it honestly be closed?**

### 9.0 The numbers this plan starts from

Gathered this session, read-only (`/proc/kallsyms` + `/proc/kcore` parse, no kernel modification),
mode ADAPTIVE, `preempt_src=2`, `hackbench -T -g 1 -f 8 -l 400000`:

| Counter | Value | Share | What it bounds |
|---|---|---|---|
| `ivh_pv_wait()` calls, total | 176,769 | 100% | the whole addressable population |
| — entered IRQs-off, **forced to busy-spin** | **42,691** | **24.2%** | **Idea 4's confirmed opportunity** |
| — executed a real halt | 134,078 | 75.8% | Idea 2/3's current working set |
| Site: queue head | 29,343 (16.6%) | — | Idea 2's ceiling, **but see caveat** |
| Site: queued node | 147,426 (83.4%) | — | — |
| Tier-1 fires (predecessor not running) | 99,924 | 56.5% of waits | large, reliable trigger signal |
| Tier-2 checks | 53,846,254 | — | — |
| Tier-2 fires (TSC-heartbeat stale) | 12,875 | **0.024% of checks**, 7.3% of waits | **Idea 2's actual trigger rate today** |

Two caveats that shape everything below and must not be forgotten when these numbers get quoted:

1. **The head/node split spans *both* halted and busy-spun calls.** It is not a halts-only
   classification, so 16.6% is *not* Idea 2's ceiling — it is an upper bound on an upper bound.
   Resolving this needs Idea 2's own dedicated counter (§2 Stage 0), split by halted-vs-spun.
2. **Tier-2 is the trigger Idea 2 is currently designed to hang off, and tier-2 barely fires** —
   12,875 events, previously established to be mostly false positives with a roughly break-even
   net wall-clock effect. Idea 2 wired only to tier-2 inherits both that low volume and that
   false-positive rate. This is a design consequence, addressed in 9.2 Step 4.

### 9.1 Honest expectation: substantial-but-partial closure, not "beat or match stock"

Stated plainly, because the rest of the plan is only worth executing against a truthful target.

**Full closure is not realistic as a general claim.** Three established facts fix a floor:

- Per §5, the wake mechanism has **zero** remaining guest-side headroom (<50ns of a
  hundreds-of-µs gap). "Trim the IPI" is not a work item and never will be again.
- Per §7, on this TDX guest `safe_halt()` is an unconditional `TDCALL`. Both VANILLA and
  PURE_IPI/ADAPTIVE pay it, so it is not itself a *gap* — but it means every halt is expensive in
  absolute terms on this platform, which caps how much any "halt more/halt smarter" idea can win.
- VANILLA's wake retires a HLT with **no ISR at all** (latching `pv_unhalted` + direct vcpu-thread
  kick). PURE_IPI/ADAPTIVE structurally cannot use that, by definition of the mode. The per-wake
  retirement delta is a **permanent, by-design cost of the portability property** — it is the
  price of the paper's central claim, not a bug to be fixed.

Therefore **every remaining point of closure must come from doing fewer, shorter, or better-placed
halts** — Ideas 2, 3, 4 — never from making a halt cheaper.

**The honest target**: drive the residual regression from the 5-8pp era down toward this rig's
~5.4% paired noise floor, i.e. to a point where a pre-registered **non-inferiority** test against
VANILLA passes. Parity in the statistical sense ("indistinguishable from stock under paired
rounds") is a defensible and achievable goal. "Beats stock" is not, and should not be promised in
the paper. There is one plausible regime where the portable modes could genuinely *win* — a
workload dominated by the IRQ-off population, where Idea 4 converts long busy-spins into halts
that stock also cannot avoid paying for — but that is a workload-specific result to report if it
appears, not a headline.

**The claim that actually survives**: *comparable performance with no host-cooperative
dependencies.* That is publishable and, unlike "faster," it is reachable from here.

### 9.2 Execution order

Dependency graph first, then the reasoning:

| # | Step | Gate/blocks | Cost | Risk |
|---|---|---|---|---|
| 1 | Idea 2 Stage 0 + halted/spun split of the site counters | unblocks Idea 2 sizing | ~3-line generic touch + counters | none (no behavior change) |
| 2 | Idea 3 Stage 0 — TDX timer-arm microbenchmark | **hard go/no-go**; kills or greenlights Idea 3 entirely | one module, isolated CPU | none if deadlines set beyond tick |
| 3 | **Resolve §7's standalone-vs-`pv_lock_ops` decision** | **blocks every Stage 1 patch below** | a decision, not code | architectural rework if deferred |
| 4 | Idea 4 Stages 0→3 (incl. correctness audit) | needs step 3 | largest single build | correctness, see 9.3 |
| 5 | Idea 2 Stages 1→4 | needs steps 1, 3; re-size after step 4 | medium | starvation, requeue races |
| 6 | Idea 3 Stages 1→5 | needs steps 2 (GO), 3 | medium | wasted self-wakes |
| 7 | Stacking/interaction matrix | needs 4, 5, 6 | harness only | confounding |
| 8 | Final closure validation vs VANILLA | needs 7 | ≥10 paired rounds × arms | drift |

**Steps 1 and 2 run in parallel and go first.** Both are instrumentation/measurement only, both
are cheap, both are falsifying, and neither requires committing to any build. This is the same
cheapest-first ordering principle §4 has used all along. Step 2 in particular can delete a third
of the remaining plan in an afternoon.

**Step 4 (Idea 4) is the first thing actually built, ahead of Ideas 2 and 3**, for one reason:
**it is the only one of the three whose opportunity is confirmed real and large today.** 24.2% of
all waits are excluded from the mechanism entirely — that is not a modelled ceiling, it is a
measured population. Idea 2's ceiling is still conflated (step 1 fixes that) and its trigger fires
0.024% of the time. Idea 3 has an unrun hard gate. Idea 4 has neither problem. It is also the only
one that *changes the denominator* for the other two, which is a second reason to do it first
rather than re-measuring everything afterward (see 9.4).

**Step 5 carries one design change forced by 9.0's caveat 2.** Idea 2 as written in §2 triggers
solely off tier-2. At 12,875 mostly-false-positive fires, that is a weak foundation. Before
building Stage 1, **add tier-1 (predecessor not running, 99,924 fires, 56.5% of waits) as an
alternative takeover trigger** behind its own sysctl bit, and measure `ivh_head_yield_ok` under
each trigger independently. Tier-1 is a stronger, more reliable, ~8x more frequent signal, and
Idea 2's whole premise ("the head looks stalled and the lock is free") is at least as well served
by it. Keep them separately gated so the paper can report which signal actually carries the
mechanism.

### 9.3 The §7 architectural decision: decide before step 4, not after

**When**: before the first Stage 1 patch of any idea — practically, before Idea 4's Stage 1.

**What it blocks**: Idea 4's Stage 1 edits `ivh_pv_wait()` (`arch/x86/kernel/kvm.c`), which is a
*KVM-specific* `pv_lock_ops` implementation. If the standalone-lock-type route wins, that logic has
a different home and the patch is written against a different file, a different entry signature,
and a different set of available context. Writing it twice is the cost of deferring this.

**Recommendation**: *build the decision logic as a standalone, hypervisor-agnostic core, and
deliver it through `pv_lock_ops` for measurement.* Concretely — one new file containing the
tier-1/tier-2 evaluation, the head-takeover CAS, the self-wake arm, and the IRQ-state
save/halt/restore, with **zero hypervisor detection anywhere in it**, parameterised only by a
two-entry sleep/wake primitive pair that defaults to generic `safe_halt()` + `__apic_send_IPI`.
KVM's `kvm_spinlock_init()` registration then becomes a thin caller of that core, which is what
produces the hackbench numbers on real kernel spinlocks; Xen/Hyper-V get the same three-line
treatment for free; and the identical core compiles and runs on bare metal with no PV path at all,
which is what makes the portability claim demonstrable rather than argued.

This is not a compromise for its own sake — it is what the two options are actually *for*.
Option 1 is a **delivery** mechanism (transparently upgrades existing spinlocks, gives you
measurements today); option 2 is a **structural** property (no hypervisor dependency in the logic
itself, which is the paper's entire thesis). They answer different questions, and a core with a
primitive-pair boundary satisfies both. Additionally, 9.3's audit findings below give an
independent argument for option 2's shape: Idea 4 needs per-call-site knowledge that a generic
`pv_wait()` hook cannot see, and an explicit-opt-in lock type is the natural place to carry it.

### 9.4 Idea 4's correctness audit — what it must actually check

§6 argues "the hazard is about *holding*, not *waiting*, so a briefly-interruptible wait is
probably fine." That argument is **not sufficient as stated**, and the audit's real job is to find
where it breaks. Three concrete hazard classes, in descending severity:

**Hazard A — same-lock re-entry from a hardirq: a genuine deadlock, not a theoretical one.**
Suppose CPU X is a queued waiter on lock L with interrupts now open (Idea 4's window). An
interrupt fires on X; its handler calls `spin_lock(&L)`. The handler queues *behind* X's own
outer node. The lock eventually passes to X's outer node — but X's outer frame cannot run, because
the ISR is on its stack and the ISR cannot return until *its* node gets the lock, which is behind
the outer node that will never proceed. Hard, permanent self-deadlock. Note that qspinlock's
`MAX_NODES=4` nesting support does **not** save this: nesting is supported, the *ordering* is what
deadlocks. This is exactly the hazard `spin_lock_irqsave()` exists to prevent, and it applies to
waiting, not only holding. **Idea 4 is therefore not blanket-safe and must be scoped.**

**Hazard B — the IF=0 region is wider than the lock acquisition.** `irqs_disabled()` at
`ivh_pv_wait()` entry cannot distinguish:

```
  (a) spin_lock_irqsave(&l, flags);          /* the case §6 describes — narrow, likely safe */
  (b) local_irq_save(flags);                 /* a pre-existing IRQ-off region ... */
      prepare_hardware_sequence();           /* ... whose atomicity the caller relies on ... */
      spin_lock(&l);                         /* ... and which our window would break */
  (c) <already inside a hardirq handler>     /* re-enabling here is simply wrong */
```

Case (b) is the real weak point in the "not the critical section" argument: the caller's atomicity
requirement may begin *before* the lock acquisition. From inside `ivh_pv_wait()` the three are
indistinguishable by IF state alone.

**Hazard C — latency, not correctness.** An interrupt taken mid-wait can itself block on unrelated
work, lengthening the wait it was supposed to shorten. Track, don't gate on it.

**How to actually run the audit** (the point is *not* to audit the kernel's `_irqsave` call sites
in general — there are thousands; the point is to audit the ~10 that produce the 42,691):

1. **Attribute first, audit second.** Add a cheap per-CPU hash-bucketed counter keyed on
   `_RET_IP_`/caller return address at every IF=0 arrival in `ivh_pv_wait()`, symbolised offline
   via `/proc/kallsyms`. This turns "audit `_irqsave` call sites" into a concrete list of the
   handful of call sites that account for essentially all of the population on this workload.
   Read-only, no behavior change, belongs in Idea 4 Stage 0.
2. **Harvest Hazard A mechanically with lockdep.** Build with `CONFIG_PROVE_LOCKING`, run the
   workload, and harvest the lock classes lockdep has marked *hardirq-safe* (i.e. observed or
   inferred to be taken in hardirq context). That set is precisely the set of locks for which
   Idea 4's window is unsafe. Cross-reference against step 1's attributed call-site list; the
   intersection is the go/no-go population.
3. **Refuse the free cases outright, with no audit needed.** From inside `ivh_pv_wait()`,
   `in_nmi()`, `in_hardirq()`, and `in_serving_softirq()` are all cheaply testable. Decline the
   IF=0 halt in all three unconditionally — that removes Hazard C-(c) entirely at zero analysis
   cost, and should be in the first Stage 1 patch, not bolted on later.
4. **Hand-audit only what survives 1-3.** For each remaining call site, the specific question is
   the (a)-vs-(b) distinction above: *does the caller's IRQ-off region begin at the lock
   acquisition, or before it?* `spin_lock_irqsave()` on the acquisition line answers (a);
   a separately-scoped `local_irq_save()`/`local_irq_disable()` earlier in the function answers
   (b) and disqualifies the site.
5. **Carry the verdict in the lock, not in the wait path.** Once 1-4 produce a per-site verdict,
   there must be somewhere to *record* it. A generic `pv_wait()` receives only a byte pointer and
   cannot know which lock it is waiting on, let alone whether that lock was cleared. This is the
   direct link to §9.3: **the explicit-opt-in standalone lock type is where a per-lock "safe to
   halt with IRQs open" property naturally lives.** If the shared-`pv_lock_ops` route is chosen
   instead, Idea 4 has to fall back to a global sysctl plus the mechanical exclusions in 2-3,
   which is strictly weaker.

**Permanent regression guards** (ship with Stage 1, keep forever):

- `WARN_ONCE` if IRQ state on return from `ivh_pv_wait()` does not match entry state.
- Verify the IRQ *tracing* state is consistent too, not just the hardware flag —
  `safe_halt()`/`local_irq_disable()` vs their `raw_`/`trace_hardirqs_on|off()` counterparts is a
  classic source of lockdep/`irqflags` corruption that produces confusing failures far from here.
- Keep `ivh_wait_irqoff_nohalt` reporting alongside the new "would-halt" counter permanently, so
  the split between "excluded by policy" and "excluded by hazard" stays visible in every run.

### 9.4.1 Stage 0 built and run, 2026-09-07 — real attribution data, and a hard result

The step-1 attribution counter from 9.4 was actually built and run this session
(`GLOCK-14` → `17`, three iterations — see below). Real, load-bearing result: **the population is
concentrated in exactly one lock class, and that lock class fails the Hazard A check by design,
not by chance.**

**Build iteration, logged because the first two attempts taught something real about instrumenting
this call path, not just "it didn't work":**

1. **First attempt** — captured `_RET_IP_` at the top of `queued_spin_lock_slowpath()`
   (`kernel/locking/qspinlock.c`). Result: 100% of samples attributed to one symbol,
   `_raw_spin_lock_irqsave+0x176`. Root cause, confirmed in source: `_raw_spin_lock_irqsave()`
   (`kernel/locking/spinlock.c:283`) is `noinline` and is the single, shared, generic wrapper every
   `spin_lock_irqsave()` call site in the entire kernel funnels through — capturing the return
   address at the slowpath entry can only ever see that wrapper, never the real caller above it.
2. **Second attempt** — tried `stack_trace_save(entries, 2, 0)` from the same slowpath-entry point
   to walk one more frame past the wrapper. Result: unchanged, still ~100% the same wrapper symbol
   — the unwinder did not reliably resolve a second frame from this call site (deep, heavily
   inlined, hot-path code; exact cause not further chased, not worth the time given a cleaner fix
   existed).
3. **Working fix** — moved the capture out of the generic slowpath entirely and into the three
   real, `noinline` lock-entry functions themselves (`_raw_spin_lock`, `_raw_spin_lock_irq`,
   `_raw_spin_lock_irqsave`, all in `kernel/locking/spinlock.c`) — a plain `_RET_IP_` read at a
   genuine, non-inlined function-call boundary needs no unwinding and is exact by construction.
   This is the version that shipped in `GLOCK-17` and produced the real data below.

**Real data** (`hackbench -T -g 1 -f 8 -l 400000`, mode ADAPTIVE, `preempt_src=2`,
`beat_threshold=220000` — several runs across `GLOCK-14`-`17`, irqoff share ranged 24.2%-34.8%
across runs, consistent with this rig's known contention-drift noise, not a measurement error):

| Count | Share | Call site |
|---|---|---|
| 22,061 | 86.85% | `__wake_up_sync_key+0x23` |
| 3,309 | 13.03% | `prepare_to_wait+0x21` |
| 26 | 0.10% | `finish_wait+0x3a` |
| 5 | 0.02% | `__put_partials+0x55` (SLUB allocator) |
| 1 | 0.00% | `get_partial_node.part.0+0x27` (SLUB allocator) |

**99.88% of the entire irqoff-excluded population is one lock class: the generic wait-queue lock**
(`wq_head->lock`, reached via `prepare_to_wait()`/`finish_wait()`/`__wake_up_sync_key()` and the
rest of the `wake_up()` family). Unsurprising given the workload — hackbench moves messages
through pipes, and blocking pipe I/O is built on wait queues — but the *lock class*, not the
workload, is what matters for the safety question below. The SLUB allocator tail is real but
negligible.

**Hazard A is not hypothetical for this lock class — confirmed by source, no lockdep run needed.**
Every `wake_up()`-family function funnels through `__wake_up_common_lock()`
(`kernel/sched/wait.c`), which takes the lock via `spin_lock_irqsave(&wq_head->lock, flags)` — not
the plain or `_irq` form. That specific choice exists precisely because `wake_up()`/
`wake_up_interruptible()` must be safely callable from *any* context, including a hardirq handler
— this is one of the most foundational, universally-relied-upon guarantees in the kernel (every
block/network/character device driver's completion interrupt calls a `wake_up()` variant). So the
exact hazard §9.4 described — an interrupt firing during Idea 4's open window, whose handler wants
the *same* lock, permanently deadlocking the CPU against its own earlier queue position — is a real
risk for the dominant call site found here, not a remote edge case reached only by unusual code.

**Verdict: Idea 4 cannot be shipped as originally scoped (a blanket hook inside a generic
`ivh_pv_wait()`) against this measured population.** A generic wait-path hook has no way to know,
at the point it's called, whether *this particular* wait-queue instance is ever woken from hardirq
context elsewhere in the kernel — some are, some (e.g. this exact workload's own pipe wakeups,
which happen to fire from process context, not hardirq) plausibly are not, but the mechanism can't
tell the difference at runtime, and a single wrong guess is a permanent hang, not a slowdown. This
sharpens, rather than resolves, the §7/§9.3 architecture question: **a per-lock "safe to halt with
interrupts open" property is not optional plumbing, it is the load-bearing fact that makes Idea 4
buildable at all**, and a generic `pv_lock_ops` hook has nowhere to attach one. The standalone,
explicit-opt-in lock type is the only one of the two §7 options with anywhere to put it.

**What this does *not* rule out**: the ~13% `prepare_to_wait`/`finish_wait` tail and the SLUB tail
are the same lock-class problem, not separate ones — no rescoping helps there. Idea 4 as "convert
the wait-queue population" is closed. Idea 4 as "convert a specific, provably-hardirq-safe lock
that some other subsystem opts into via the standalone lock type" remains open and is now the only
form worth building.

### 9.4.2 Hazard A: deliberately reproduced live, 2026-09-07 — confirmed real, closing Idea 4 in its original form

Rather than leave 9.4.1's reasoning as untested theory, we built the mechanism for real (a new
sysctl, `ivh_pv_irqoff_halt`, default 0 = today's safe busy-spin; 1 = enable IRQs for the halt,
restore before returning — `arch/x86/kernel/kvm.c`, shipped in `GLOCK-18`) and a small standalone
kernel module (`hazard_a_test.ko`) built specifically to reproduce Hazard A on a *private* lock,
so a positive result couldn't be confused with corrupting any real kernel subsystem: N userspace
threads contend on the module's own `raw_spinlock_t` via `write()` → `raw_spin_lock_irqsave()`
(matching the real-world shape found in 9.4.1's attribution exactly), while a per-CPU,
hard-irq-context, pinned `hrtimer` (`HRTIMER_MODE_REL_PINNED_HARD`, 2ms period) repeatedly attempts
a real, blocking `raw_spin_lock()` on that same lock — the direct reproduction of "an independent
interrupt lands during Idea 4's open window and wants the same lock."

**Result: reproduced on the first real attempt.** Sequence: module armed 07:35:56; a baseline run
with `ivh_pv_irqoff_halt=0` completed cleanly (9,883 lock acquisitions, timer fired 297,838 times,
**every single one** successfully acquired the lock — mechanism confirmed correct and safe when the
window never opens); `ivh_pv_irqoff_halt` set to 1 at 07:37:17; the guest froze within roughly a
minute and required an external hard reset. `journalctl -b -1` shows nothing abnormal — no panic,
no soft-lockup warning, routine logging simply stops at 07:38:02. That silence is itself
consistent with the predicted failure mode, not a different one: the deadlock is *inside the timer
interrupt's own handler*, so the mechanism that would normally flag a stuck CPU (the periodic timer
tick) is exactly what's stuck.

**This is not a rare-collision result — the numbers say it shouldn't be.** 16 CPUs × a 2ms timer
= ~500 fires/sec/CPU. The clean baseline measured ~3,294 lock acquisitions/sec system-wide from 32
threads over 16 CPUs (2x oversubscribed), meaning a large fraction of attempts genuinely queue
rather than acquire immediately — and every queued attempt holds IRQs off for the whole wait
(`_irqsave`), so under Idea 4 every one opens a window roughly as long as the halt itself (order of
100s of µs per earlier `hlt_cycles` measurements). A window's chance of overlapping that same CPU's
own next timer firing is roughly `window_length / 2000µs`; with 100+ such windows/CPU/sec, that
puts the expected collision rate at tens per CPU per second, hundreds system-wide — and one
collision is sufficient for a permanent deadlock. Hitting it within under a minute of enabling the
mode is what this predicts, not bad luck. **Under real, heavy contention — the oversubscribed-CVM
scenario this whole project targets — this is expected to happen within seconds to minutes, not an
edge case that might never occur in practice.**

**Verdict, and why this was not escalated for further design review**: this is closed, not
salvageable in its original (generic `pv_lock_ops` hook) form, and no further code-level cleverness
changes that. The failure isn't an inefficient or buggy implementation — it's a missing *fact*:
whether a given lock is ever touched by an interrupt handler anywhere in the kernel. A generic hook
inside `ivh_pv_wait()` cannot know that about any lock it's asked to wait on, for any lock, ever,
without being told — no algorithm supplies information nobody gave it. The only real fix is
structural, restated more firmly than 9.4.1's hedge: **a lock must explicitly declare itself safe
for this mechanism, which requires the standalone opt-in lock type from §7/§9.3.** That remains
open. "Convert IRQs-off waiters unconditionally" (or any generic, workload-wide policy) does not.

**Cleanup**: `ivh_pv_irqoff_halt` reset to 0 (its safe default) immediately after; confirmed 0 on
the next boot without any action needed, since sysctls don't persist across reboots. The
`hazard_a_test` module is not loaded on the current boot. No production code path is affected by
any of this — `ivh_pv_irqoff_halt` is a dedicated, isolated experimental knob, not a modification
to `ivh_adaptive_mode`'s existing, already-validated modes.

**Follow-up, 2026-09-08 — organic (non-synthetic) exposure, survived.** A fair objection was
raised: 9.4.2's rate estimate ("tens to hundreds of collisions/sec, expected within seconds to
minutes") was derived entirely from `hazard_a_test.ko`'s own engineered parameters (a dedicated
2ms hard-irq timer built specifically to hammer the *same* private lock) — it was never a claim
about how often a real, unmodified kernel's actual interrupt handlers collide with genuinely-queued
waiters under ordinary use. That distinction hadn't been stated clearly enough. To check it
directly: `ivh_pv_irqoff_halt=1` was set again, with `hazard_a_test.ko` **not loaded**, and 8 real
`hackbench -T -g 1 -f 8 -l 400000` passes were run back-to-back (~5.7 minutes of sustained real
contention, an independent 2-second heartbeat running throughout to catch a freeze even if
hackbench itself were the thing stuck). **Result: survived cleanly, heartbeat uninterrupted, all 8
passes completed, no oops/panic.** Not a trivial exposure either — live counters confirm the exact
hazardous path (real `safe_halt()` with the IF-reopen, excluding nmi/hardirq/softirq context) was
taken **540,452 times** (`ivh_irqoff_halt_used`) out of 732,118 total IF=0 entries
(`ivh_wait_irqoff_nohalt`) during the run.

**What this does and doesn't change**: it does not overturn 9.4.2's closure — one workload's
absence of a hit over 5.7 minutes doesn't rule out the hazard for lock/interrupt-handler pairings
this workload never exercises, and the synthetic reproduction already proved the failure mode is
mechanistically real, not hypothetical. But it is real, meaningful counter-evidence, not nothing:
540K+ genuine exercises of the reopened-IF window, against hackbench's actual lock population
(dominated by the scheduler wake path per 9.4.1's attribution), produced zero collisions — the
organic collision rate for *that* lock population is evidently far below the synthetic test's
engineered worst case, consistent with the user's own field observation ("this workload has run
adaptive spinning without incident before"). This sharpens, rather than weakens, the case for the
per-lock opt-in path: real workloads built on locks that are provably never touched from hardirq
context look genuinely safe in practice, not merely safe in theory — the remaining risk is
specifically the unknown tail of locks somewhere in the kernel that *are* touched from hardirq
context, which is exactly what an opt-in mechanism sidesteps by construction. `ivh_pv_irqoff_halt`
reset to 0 immediately after; no lasting state change.

### 9.5 Stacking: plausibly additive, with three specific ways it could go wrong

The three ideas attack genuinely different quantities — **wait eligibility** (4), **lock-idle
time** (2), **wait duration** (3) — unlike the earlier wake-cost ideas, which turned out to be
substitutes for one another. Additivity is the reasonable prior. The failure modes to watch:

**Interaction 1 (the big one) — Idea 4 grows the population that pays the unreducible per-halt
cost.** Converting 42,691 busy-spins into real halts increases the halting population by ~31.8%
(134,078 → 176,769). Per §5 that per-halt cost cannot be reduced. **If those waiters' typical wait
is shorter than a halt+wake round trip, Idea 4 is a net loss even though its opportunity is real.**
Mitigation, and this should be in the design from the start rather than discovered in Stage 3:
**do not halt at IF=0 unconditionally — gate it on the existing adaptive signal.** Only take the
IF=0 halt when tier-1 or tier-2 says the predecessor is actually stalled (i.e. the wait is
predicted long). That reuses machinery already built and tested, and it converts Idea 4 from
"halt 24.2% more often" into "halt more often only where halting pays." Instrument both variants
(`ivh_pv_irqoff_halt` ∈ {off, unconditional, adaptive-gated}) and let the data pick.

**Interaction 2 — Idea 4 invalidates Idea 2's ceiling measurement.** More halted waiters means
more sleeping queued nodes, which is *favorable* for Idea 2 (more takeover opportunity) but shifts
the head/node split and increases `head_ctl` CAS contention. **Consequence for sequencing**:
measure Idea 2's true ceiling (step 1) *before* Idea 4 lands, then **re-measure it after**, and
report both. Do not size Idea 2's Stage 4 endpoints off the pre-Idea-4 number.

**Interaction 3 — Idea 3 is partly *unlocked* by Idea 4, favorably.** A self-programmed timer
wake is delivered as an interrupt and therefore requires IF=1. Today the IF=0 population cannot
use Idea 3 at all. After Idea 4, it can — genuine positive synergy. But the timer then fires into
a context the caller believed was IRQ-off, so it is covered by exactly the Hazards A/B audit above
and introduces no *new* hazard class, only more traffic through the existing one. Also confirm
Idea 3's Stage-3 rule (self-wake returns through the normal bottom of the function so the
`HALTED→HASHED`/`HALTED→RUNNING` protocol always runs) still holds when the exit is also an
Idea-2 takeover candidate — a self-woken W2 that immediately attempts a head takeover exercises
both state machines in one path and deserves its own `WARN_ONCE` and a targeted stress arm.

**Confound guards, per-run, non-negotiable**: `ivh_head_woke_moot/ivh_head_arm` (Idea 2's
separability guard, already specified in §2), the wasted-self-wake rate (Idea 3's, §3 Stage 3),
and the halted/spun-split site counters (Idea 4's denominator). With three stacked mechanisms and
a ~5.4% noise floor, mechanism counters — not wall-clock — are what attribute an effect to a
cause.

### 9.6 Final validation: how to know whether the gap is actually closed

The rig has real contention drift, and a single sequential comparison has already misled this
investigation once this session. The final stage must therefore be paired and interleaved, exactly
as §§1-3 already require.

**Design**:

- **Arms**: `V` (VANILLA), `A` (ADAPTIVE, all new sysctls off — the true control for every new
  mechanism), `A4`, `A2`, `A3` (each new idea alone), `A234` (all stacked). `P` (PURE_IPI) as a
  diagnostic arm to separate "adaptive logic" from "portable wake" whenever `A` and `V` diverge.
- **Structure**: one round = all arms, order randomized within the round; ≥10 rounds; no co-tenant
  VM on the host; `V` repeated first and last within each round to bound drift *inside* the round,
  not only across the run.
- **Statistics**: report **paired per-round deltas** with t-statistic and sign count (n-of-n
  rounds negative), never pooled means across arms. This is the analysis form that made the
  G0K−G0 result trustworthy at −19.4% (t=−29.6, 5/5) and is the house standard.
- **The primary test is non-inferiority, not superiority.** Pre-register it before the run:
  *`A234 − V` paired median ≤ 2%, with the 95% CI upper bound below the rig's 5.4% noise floor.*
  Framing it as superiority guarantees a "negative result" write-up for an outcome (parity without
  host cooperation) that is actually the goal.
- **Secondary/mechanism endpoints, reported in every single run** — these are what make a null or
  partial wall-clock result interpretable rather than a dead end:

| Metric | Source | Answers |
|---|---|---|
| `ivh_wait_irqoff_nohalt` + new would-halt counter | §5/§6 | did Idea 4 actually convert the population? |
| `hlt_cycles`/`hlt_events`, per arm | `ivh_tsc_beat.h:670-672` | per-halt cost, on a now-comparable population |
| halt-duration log2 histogram | §1 Stage 1 | is the ~1ms tick-rescue mode still there? |
| head/node split, **halted-only** | step 1 | Idea 2's real denominator |
| `ivh_head_yield_ok`/`_try`/`_moot`, per trigger (tier-1 vs tier-2) | §2 | which signal carries Idea 2 |
| wasted-self-wake rate | §3 Stage 3 | Idea 3's own cost |
| tier-1/tier-2 fire counts and rates | 9.0 | did stacking change the trigger population? |

- **Second workload, mandatory.** Every number in this document comes from one hackbench
  configuration. At least one structurally different contended workload (and the §2 Stage 2
  synthetic single-lock driver, which is the only thing that stresses the requeue path hard) must
  be run before any closure claim, or the result is an overfit to `-T -g 1 -f 8 -l 400000`.
- **Stopping/kill criteria, pre-registered**: if `A234 − A` is indistinguishable from zero while
  all three mechanism counters confirm the mechanisms fired at their designed rates, the honest
  conclusion is *"the residual gap is the by-design cost of portable wake retirement (§5, §9.1),
  and it is X%"* — report X and stop, rather than tuning knobs against noise.

**Safety** (§8 applies to everything above): none of these stages synthesizes interrupt delivery.
Idea 4's and Idea 2's wakes go through the kernel's own existing wake path; Idea 3's timer is
programmed through `clockevents_program_event()` — the kernel's own public API on the CPU's own
clockevent device — never a raw ICR or SELF_IPI write, and its go/no-go microbenchmark (§3 Stage 0)
deliberately programs deadlines beyond the tick so nothing ever fires. Any *new* live experiment
proposed later that needs a real interrupt delivered must use a verified-unused vector with its own
isolated handler. Read-only MSR access remains fine.

### 9.7 Value of this work

This work builds the **reactive, local** half of a two-part answer to lock-holder preemption in
confidential VMs. IVH's other half already exists and has been deliberately off throughout this
investigation: a proactive vCPU-migration engine that moves work off a vCPU *before* it is
preempted, rather than reacting after a lock is already held by a doomed one. Pairing that
proactive half with an adaptive-spinning mechanism that is *itself* trustworthy under an untrusted
host is, as far as this project's own survey has found, a genuinely new combination. Prior
adaptive-spinning designs for virtualized environments — I-Spinlock, eCS, and the stock
pv-qspinlock this work is measured against — take their inputs from the host: steal-time,
`pv_kick`/`pv_wait`, preemption hints. A confidential VM's threat model says not to
trust exactly those inputs, and a hardened, non-KVM, or resource-constrained host can simply
decline to offer them; they are negotiated features, not guarantees. This design's inputs are
chosen because they cannot be withheld: interrupt delivery is basic functionality no virtualized
guest can run without, and the TSC is a locally readable, locally verifiable register requiring
zero host cooperation to believe. What is proven today is the trust property and the mechanism:
PURE_IPI/ADAPTIVE work and depend on nothing the host negotiates. What is **not** proven is
parity — a measurable regression against stock remains, the wake mechanism has no headroom left
(§5), and closing the rest rests entirely on Ideas 2/3/4 landing as planned. The contribution here
is the trustworthy substrate; the performance case is still open.

## 10. Tier-2 low-fire-rate audit (2026-09-08) — three independent verdicts, no bug found

Triggered by a live counter reading (H1 mode, real hackbench contention, fresh GLOCK-20 boot,
`ivh_pv_beat_threshold=220000`): `ivh_beat_tier2_checked=218,745,428`, `ivh_beat_tier2_fired=32,572`
— a 0.0149% fire rate. Suspicious enough on its face to warrant three parallel, independent,
read-only Opus audits before trusting it: (1) threshold/units calibration and age-comparison
arithmetic, (2) heartbeat publish-side wiring and predecessor-vs-self addressing, (3) whether
`SPIN_THRESHOLD`'s own real-time duration structurally starves tier-2 of a chance to fire before
tier-1 or outright loop exhaustion wins. All three came back clean; none found a defect.

**(1) Calibration.** `ivh_beat_age()` uses raw `rdtsc()` on both the publish and read side, no
unit conversion anywhere on the tier-2 path (`ivh_tsc_beat.h:278-296`). `age` is signed, compared
correctly, never clamped or wrapped (`qspinlock_paravirt.h:341`). The kernel's own boot log
(`tsc: Detected 2200.000 MHz processor`) and IVH's own `late_initcall` calibration confirm
`tsc_khz == 2200000` exactly, so 220000 cycles really is 100.0µs on this host, not an assumed
constant. Live `ivh_beat_min_age` is -504 to -712 cycles across all 16 CPUs (~0.3µs) — cross-vCPU
TSC drift is two orders of magnitude too small to explain anything.

**(2) Publish wiring.** `ivh_tsc_beat_publish()` writes `this_cpu`; `ivh_beat_age(cpu)` reads
`per_cpu(..., cpu)` for the caller-supplied *predecessor's* cpu (`prev->cpu`, stamped once at
`pv_init_node()`), never the caller's own id — confirmed by tracing the exact call chain. Publish
is wired into both hot loops (`pv_wait_node()` line 667, `pv_wait_head_or_lock()` line 845) with a
live, decrementing `loop` counter, not a frozen value. Live `ivh_beat_publishes=292,592,669`
reconciles with the loop-iteration counters to within ~10% (the residual explained by `gotlock`
exits that bypass one iteration counter but not the publish itself). Publishing is real, frequent,
and reaching the right memory.

One coupling issue surfaced as a side effect, worth carrying into any future threshold retuning:
the in-spin publish cadence (every 4096 iterations, tied to `ivh_pv_beat_publish_mask`) is only
loosely coupled to the threshold *in cycles* — nothing in the code ties the mask to
`ivh_pv_beat_threshold`, so at high enough per-iteration cost the publish cadence itself could
exceed the threshold and self-age a perfectly healthy waiter. Not observed to matter at the current
220000/4095 pairing (see the histogram below), but it is a latent footgun if either knob is retuned
independently of the other.

**(3) Structural timing.** The hypothesized starvation mechanism — that a full `SPIN_THRESHOLD`
(32768-iteration) pass completes in real time shorter than the 100µs threshold, so tier-1 or plain
loop exhaustion always wins before tier-2's window opens — is refuted by direct measurement, not
just argument. Bounding real per-iteration cost from the observed age histogram against the known
publish cadence gives roughly 4-32 cycles/iteration, i.e. `SPIN_THRESHOLD` spans on the order of
60-480µs of real time — the *same order as or several times longer than* the 100µs threshold, not
much shorter. More decisively: tier-2's own age distribution has no ceiling at the threshold —
377,401 of 218.7M samples exceed 131K cycles and 5,104 reach as high as 7.6ms — so the mechanism
demonstrably *can* and *does* see the far tail; nothing is cutting it off structurally. And the
clock tier-2 reads is the *predecessor's* elapsed staleness, not bounded by the successor's own
loop budget at all: a predecessor preempted before the successor even joined the queue reads stale
on the successor's very first check.

**The real explanation, from the live `ivh_beat_age_hist_raw` histogram (218.7M samples, the exact
population tier-2 evaluates):** 63.8% of samples are under 3.7µs, 93.9% under 14.9µs, 99.827% under
59.6µs, 99.998% under 119µs. The 220000-cycle/100µs threshold sits at the **99.985th percentile**
of the real, live staleness distribution in this workload. A 0.0149% fire rate is not a suppressed
or broken signal — it is the exact, arithmetically necessary consequence of that distribution.
Tier-1 fires 26x more often (844,937 vs 32,572) not because tier-2 is broken, but because tier-1 is
a discrete one-hop event (predecessor's own state flips to halted) while tier-2 requires 100µs of
*continuous* real elapsed time with no publish — an event this specific workload essentially does
not produce. This independently reproduces the project's earlier, separately-derived finding
(the raw-age-histogram work, §9.0-era) that 99.7%+ of real staleness samples fall under 60µs.

**Conclusion:** nothing is wrong with sensitivity, TSC-stamp reading, or publish coverage. Tier-2's
low fire rate is real and correct given the current threshold and this workload's actual
preemption profile. The only lever that would change the fire rate is the threshold value itself —
the distribution's knee sits around 2^16 cycles (~30µs) — which is a tuning/design decision (trading
earlier, less-certain bail-outs for a higher false-positive rate against genuinely-still-running
predecessors), not a bug fix.

### 10.1 Synthesis: kick cost and early bail are two separate axes, not one

Putting §5 and §10 together resolves the "why doesn't H1 beat D" question cleanly, and the answer
is structural, not a defect in either mechanism:

- **Wake cost (kick) and spin cost (early bail) are independent axes.** VANILLA/D wakes via
  `KVM_HC_KICK_CPU` — the *same* hypercall stock upstream `pv_kick()` uses, hitting KVM's REMRD
  fast path in `__apic_accept_irq()` (skip vector injection, just flip `pv_unhalted` and kick the
  vcpu thread). PURE_IPI/G0 and ADAPTIVE/H1 both instead send a real FIXED-vector IPI, which is
  structurally more expensive to service (full injection/ISR/EOI/IRET) — established in §5 by
  direct KVM source reading, independent of anything tier-2-related. This tax is paid on *every*
  wake in G0 and H1 alike, whether or not tier-2 ever fires, because it is a property of *how the
  wake is delivered*, not of *when the decision to wait was made*.
- **Early bail (tier-2) only ever acts on the other axis** — how long a waiter spins before
  deciding to halt — and, per §10, fires on 0.015% of checks in this workload because genuine
  ≥100µs host preemption is simply rare here. So in this specific test, tier-2 has essentially no
  aggregate spin-time to give back, and even where it does fire, it cannot touch the kick-cost tax
  at all — that cost is identical whether the halt was reached via tier-1, tier-2, or plain
  `SPIN_THRESHOLD` exhaustion.
- **Net result:** G0 and H1 pay the same structural kick-cost regression against stock PV, and H1
  additionally gets a spin-time discount that is real but currently too rare to move measured
  wall-clock time — matching the noisy, no-clear-winner D/G0/H1 hackbench results from this session.
  This is not evidence against the design: §9.7 already frames the goal as a trustworthy substrate
  with an open, honestly-stated performance gap, not parity, and §5 already rejected the one lever
  (REMRD) that could have closed the kick-cost gap, on portability grounds. Tier-2 would very
  plausibly earn its keep on a host with heavier real oversubscription (its entire premise); it is
  not, and was never going to be, a lever against the kick-cost tax itself.
