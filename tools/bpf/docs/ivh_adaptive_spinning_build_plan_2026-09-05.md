# IVH adaptive spinning: decisive test results + incremental build plans, 2026-09-05

Status: **one hypothesis tested and falsified as originally scoped (with one untested variant
still open), two designs staged into buildable, debug-instrumented, independently-testable
patches. Nothing in this doc has been implemented yet.**

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
