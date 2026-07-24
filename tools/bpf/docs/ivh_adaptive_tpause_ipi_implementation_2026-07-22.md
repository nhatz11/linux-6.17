# IVH adaptive-TPAUSE + targeted-IPI wake: implementation report

**Date:** 2026-07-22
**Kernel:** `6.17.0-rseqport55-trimsys+`, branch `kernel-43-clean`
**Status:** IMPLEMENTED, compiles clean. Not boot-tested (requires the full rebuild + reboot the user does separately).
**Implements:** `ivh_adaptive_tpause_ipi_plan_2026-07-22.md` §4 (role C + role B, phase 1), scope as approved: role C + role B only, role A and rwlock out of scope, no TSC/steal-time changes.

---

## 1. What changed

Three files touched, all in the `pv_ops.lock.wait`/`pv_ops.lock.kick` chokepoint the plan identified. No other files modified.

### 1.1 `arch/x86/kernel/kvm.c`

- **New includes** (top of file): `<linux/smp.h>` for `smp_send_reschedule()`/`arch_smp_send_reschedule()`, and `<trace/events/ipi.h>` for `trace_ipi_send_cpu()` — this is the tracepoint `smp_send_reschedule()`'s own macro body calls (`include/linux/smp.h:139-142`); without the header the build fails with `implicit declaration of function 'trace_ipi_send_cpu'` (hit this exact error on the first build attempt, see §2).

- **New constant**, added just after the existing `IVH_PV_WAIT_TSC`/`IVH_PV_TPAUSE_CYCLES` pair:
  ```c
  #define IVH_PV_ADAPTIVE_TSC	3000000ULL	/* max cycles/wait, mechanism==1 */
  ```
  ~1 ms at 3 GHz, exactly the value the plan specified (§4.1(a)). `IVH_PV_WAIT_TSC` (65536, ~22 µs) is untouched and still governs the `mechanism==0` path — nothing about the old behavior changed.

- **`ivh_pv_wait()`** (the `mechanism==1` branch, previously using `IVH_PV_WAIT_TSC` as the deadline): now uses `IVH_PV_ADAPTIVE_TSC`. This is the only functional change in this function — the do-while body (`if (READ_ONCE(*ptr) != val) return; ivh_pv_backoff();`) is byte-for-byte unchanged, exactly as the plan said it should be (§4.1(a): "the whole change on the wait side is the constant").

- **`ivh_pv_kick()`**: restructured so the `mechanism==0` branch is unchanged in behavior (still `ivh_pv_hypercall_kick(cpu)` iff the host advertises `KVM_FEATURE_PV_UNHALT`, else a no-op) but now falls through to a real `smp_send_reschedule(cpu)` when `mechanism==1`, replacing the prior unconditional no-op. This is role B from the plan (§4.2).

### 1.2 `kernel/locking/qspinlock_paravirt.h`

- **`pv_kick_node()`**: after the existing `try_cmpxchg_relaxed(&pn->state, &old, VCPU_HASHED)` gate and the existing `WRITE_ONCE(lock->locked, _Q_SLOW_VAL); pv_hash(lock, pn);` lines (both untouched), added:
  ```c
  if (READ_ONCE(ivh_pv_wait_mechanism))
      smp_send_reschedule(pn->cpu);
  ```
  This is role C from the plan (§4.1(b)) — placed *after* the state write, matching the plan's explicit ordering requirement.

### 1.3 `kernel/locking/qspinlock.c`

- Added `#include <trace/events/ipi.h>` next to the pre-existing `#include <linux/smp.h>` (line 16). Needed for the same reason as in `kvm.c` §1.1 — `qspinlock_paravirt.h` is textually included into `qspinlock.c` (guarded by `_GEN_PV_LOCK_SLOWPATH`), so the tracepoint declaration has to be visible in the translation unit that actually calls `smp_send_reschedule()` from `pv_kick_node()`.

No other lines in any of the three files were touched. `ivh_pv_wait_mechanism`, `pv_wait_early()`, `KVM_FEATURE_PV_UNHALT`/`ivh_pv_hypercall_kick()`, `kvm_spinlock_init()`, and everything else the plan said was already correct and shouldn't change, did not change.

---

## 2. Build confirmation

```
$ sudo make -j$(nproc) arch/x86/kernel/kvm.o kernel/locking/qspinlock.o
  CC      arch/x86/kernel/kvm.o
  CC      kernel/locking/qspinlock.o
```

First attempt failed (both objects) with:
```
./include/linux/smp.h:140:9: error: implicit declaration of function 'trace_ipi_send_cpu' [-Wimplicit-function-declaration]
```
— `smp_send_reschedule()`'s macro body calls `trace_ipi_send_cpu()`, declared in `include/trace/events/ipi.h`, which neither `kvm.c` nor `qspinlock.c` had pulled in before this change (nothing in either file previously called `smp_send_reschedule()`). Fixed by adding that include to both files (§1.1, §1.3). Second attempt built clean, zero errors.

Then re-touched both `.c` files and rebuilt a third time with `W=1` (extra warnings) to check nothing was silently suppressed:
```
$ touch arch/x86/kernel/kvm.c kernel/locking/qspinlock.c
$ sudo make -j$(nproc) W=1 arch/x86/kernel/kvm.o kernel/locking/qspinlock.o
  CC      arch/x86/kernel/kvm.o
  CC      kernel/locking/qspinlock.o
```
No warnings, `W=1` or otherwise. `.o`/`.cmd`/`.d` files chowned back to `nick:nick` after each build per the established pattern.

Not built: a full kernel image. Not booted. That is the user's own next step.

---

## 3. Correctness invariants — re-derived against the actual code written

### 3.1 Bounded-poll fallback stays intact (lost/dropped/misdelivered IPI => late wake, never no wake)

Traced both call sites' control flow after the edit:

- **Role C** (`pv_wait_node()`, unchanged): `smp_store_mb(pn->state, VCPU_HALTED); if (!READ_ONCE(node->locked)) pv_wait(&pn->state, VCPU_HALTED);` then, regardless of why `pv_wait()`/`ivh_pv_wait()` returned, the *outer* `for(;;)` loop in `pv_wait_node()` re-enters its `SPIN_THRESHOLD` spin and re-checks `node->locked` again. `ivh_pv_wait()` itself, in the `mechanism==1` branch, is a `do { if (READ_ONCE(*ptr) != val) return; ivh_pv_backoff(); } while ((s64)(rdtsc() - deadline) < 0);` loop — this always terminates by `deadline = rdtsc() + IVH_PV_ADAPTIVE_TSC` regardless of whether any IPI ever arrives, because the loop condition is a plain TSC comparison, not something an IPI participates in. If every IPI this feature ever sends were black-holed, the waiter still returns from `ivh_pv_wait()` within ~1 ms and the caller's own re-check loop still runs. Worst case is added latency (now up to ~1 ms/nap instead of ~22 µs/nap), never a hang. This is exactly the plan's §5.1 argument, and I confirmed it against the real loop bodies rather than taking it on faith.
- **Role B** (`pv_wait_head_or_lock()`, unchanged): same shape — `pv_wait(&lock->locked, _Q_SLOW_VAL);` inside a `for(;; waitcnt++)` that re-attempts `trylock_clear_pending()`/hash-and-`xchg` on every iteration regardless of how the previous `pv_wait()` call returned. Same conclusion: a totally-lost IPI degrades to the ~1 ms deadline, never a stall.

Both callers were read in full (not just the lines the plan quoted) to confirm neither has a branch that assumes the wake happened — I did not find one.

### 3.2 No double-wake / double-acquire hazard

- Acquisition in both roles is gated by an atomic on the lock word (`trylock_clear_pending()`'s CAS in role B, `node->locked` written by the *predecessor's* `arch_mcs_spin_unlock_contended()` in role C), never by whether a wake occurred. `smp_send_reschedule()` does not touch `lock->locked` or `node->locked` — it only causes the target CPU to exit whatever wait state it's in (TPAUSE nap or a normal running context) and re-run its own poll loop. A spurious/extra/duplicate IPI is therefore inert: at worst it's a wasted resched-IPI interrupt on a CPU that re-checks its condition, finds nothing changed, and re-naps (role C) or re-loops (role B). I confirmed neither `pv_kick_node()` nor `ivh_pv_kick()` writes to any state that gates acquisition — the pre-existing `try_cmpxchg_relaxed(&pn->state, &old, VCPU_HASHED)` in `pv_kick_node()` is the only state transition added code depends on, and it's a single-fire CAS (a second call with the same `node` would find `old != VCPU_HALTED` and return early without sending a second IPI) — so even repeated calls to `pv_kick_node()` on the same node (which doesn't happen in the current call graph, but hypothetically) can't double-send.
- Node/lifetime reuse: role C's `next` is the live per-CPU `qnode` of a CPU that is, by construction, still inside `queued_spin_lock_slowpath()` (it cannot have returned — returning requires observing `node->locked==1`, which this very code path is what sets). Role B's `node` comes from `pv_unhash(lock)`, and the existing upstream comment immediately above the `pv_kick(node->cpu)` call (`qspinlock_paravirt.h` unlock slowpath, unchanged by this patch) already establishes that this read is valid and that kicking a non-halted/already-departed vCPU is harmless. Neither invariant was created by this patch; both were already relied on by the pre-existing `pv_kick`/`pv_kick_node` machinery, and this patch adds no new lifetime dependency — it only adds an IPI send that fires under the same gate (or, for role B, at the same call site) as before.

### 3.3 The IPI actually causes a re-check (not a no-op wake)

This is the one invariant I did not want to take from the plan on faith, so I ran it down independently:

- `ivh_pv_backoff()`'s nap primitive is `__tpause()` (WAITPKG `TPAUSE`), or `cpu_relax()` if WAITPKG isn't present.
- For the `cpu_relax()` fallback, there is nothing to verify — it's a plain busy-spin that already re-checks `*ptr` every iteration of `ivh_pv_wait()`'s own do-while, with or without any IPI. No dependency on the IPI's semantics at all in that case.
- For the real `TPAUSE` path, I found the kernel's own documented semantics for exactly this instruction in `arch/x86/lib/delay.c`'s `delay_halt_tpause()`, used for `udelay()`-style busy-waits that must also tolerate being called with IRQs disabled:
  ```c
  /*
   * On Intel the TPAUSE instruction waits until any of:
   * 1) the TSC counter exceeds the value provided in EDX:EAX
   * 2) global timeout in IA32_UMWAIT_CONTROL is exceeded
   * 3) an external interrupt occurs
   */
  ```
  Condition (3) is stated unconditionally — not qualified by RFLAGS.IF. `smp_send_reschedule()` sends a normal IPI (the reschedule vector) to the target's local APIC; that is "an external interrupt occurs" from the target CPU's point of view regardless of whether the target currently has interrupts locally masked. This matches how TPAUSE/MWAIT-family instructions are used elsewhere in the tree specifically *because* they need to be interruptible even from IRQs-disabled contexts (the delay.c code above is called from contexts that may have IRQs off). After `__tpause()` returns (for any of the three reasons), control returns to `ivh_pv_backoff()`'s caller, i.e. back into `ivh_pv_wait()`'s do-while loop, which immediately does `if (READ_ONCE(*ptr) != val) return;` — the re-check the invariant requires. I did not find any path where the IPI arrives, `__tpause()` returns, and the do-while loop fails to re-check `*ptr`; the re-check is unconditional at the top of every iteration.
- One caveat worth stating plainly: this confirms the *waiter's* wake works. It does not by itself confirm IPI *delivery* timing/rate under real contention (i.e. whether `smp_send_reschedule()` can queue up, coalesce, or be delayed behind other IPIs on a busy vCPU) — that's a live-system question the plan already flagged under "IPI storms" (§5.5) as a *performance* risk, not a correctness one, and out of scope for this static review. The bounded-deadline fallback in §3.1 is exactly what makes that performance risk not also a correctness risk.

### 3.4 IRQ-disabled-context safety

Both new call sites (`pv_kick_node()`'s IPI and `ivh_pv_kick()`'s IPI) run inside `queued_spin_lock_slowpath()`/`__pv_queued_spin_unlock_slowpath()`, which for `_irqsave`/`_bh` callers execute with IRQs or softirqs already disabled by the *caller*. `smp_send_reschedule()` → `arch_smp_send_reschedule()` → (x86) `native_smp_send_reschedule()` is the same primitive the scheduler itself calls from `resched_curr()` under `rq->lock` with IRQs disabled, so it is established-safe from that context; I did not add anything that sleeps, blocks, or re-enables IRQs at either site — both new blocks are `if (...) smp_send_reschedule(...)`, nothing else.

### 3.5 Where I did *not* independently re-verify beyond the plan

The plan's claim that `pn->cpu` (role C) and `node->cpu` (role B, via `pv_unhash`) are safe to read at the point they're read — I re-read the surrounding code and agree with the plan's reasoning (§1.1's "no lifetime hazard" / the pre-existing upstream comment at the role-B unlock site), but this is inherited logic from the existing, unmodified `pv_kick`/`pv_unhash`/`pv_hash` machinery, not something this patch introduces or changes. I'm confident in it because it's the same guarantee the *existing* (pre-IVH, upstream) `pv_kick()` call already relied on before this patch — this patch doesn't add a new read of `.cpu`, it only adds what happens once that pre-existing, already-safe read has occurred.

### 3.6 Confidence summary

High confidence on 3.1 (bounded fallback), 3.2 (no double-acquire), and 3.4 (IRQ-context safety) — these follow directly from control flow I read line-by-line in the actual patched files. Reasonably high but not "watertight until boot-tested" confidence on 3.3 — the TPAUSE-wakes-on-any-interrupt claim is backed by the kernel's own documented semantics in `delay.c` for a genuinely IRQ-disabled-tolerant use case, which is the right kind of evidence, but I have not verified it against the Intel SDM's formal instruction reference or against a runtime trace on this exact CPU's microcode. This is the one invariant I'd want the user's boot test to actively confirm (§4 below gives a concrete signal for that: if `mechanism=1` in-guest testing shows `ivh_pv_wait_calls` growing but wall-clock/latency does NOT improve at all relative to a hand-verified expectation, that's the first place I'd look — though note per §5 below that a neutral-to-negative result vs. the *current* `mechanism=0` baseline is expected regardless, for reasons that have nothing to do with this invariant).

---

## 4. Where I did not change scope, and one thing I noticed but did not touch

- No TSC-based preemption detection was added anywhere. `pv_wait_early()` is untouched; it still reads `vcpu_is_preempted(prev->cpu)` (steal-time), exactly per the explicit exclusion in the task.
- Role A and rwlock remain untouched and out of scope, per the plan and the task's confirmed scope.
- `ivh_pv_wait_mechanism`'s registration, `kvm_spinlock_init()`, and the sysctl table are untouched.
- **One thing noticed, not touched (per instructions to note rather than fix):** in `ivh_pv_kick()`'s `mechanism==0` branch, the comment now says "the `cpu_relax()` busy loop self-corrects" to describe the case where PV_UNHALT is absent — this is accurate but is describing pre-existing behavior I didn't change; flagging only because the comment text shifted slightly during the edit and a reviewer diffing comments-only changes should know that line's *meaning* is unchanged even though its wording is not identical to before.

---

## 5. Next steps for the user

### 5.1 Rebuild and boot

Full kernel rebuild + reboot into the new image — your own step, not run here. The two objects that changed (`arch/x86/kernel/kvm.o`, `kernel/locking/qspinlock.o`) already compile clean incrementally (§2); a full build should not surprise you beyond normal link-time checks.

### 5.2 Correctness soak (do this before any performance comparison)

With `mechanism=1`:
```
echo 1 > /proc/sys/kernel/ivh_pv_wait_mechanism
hackbench -g16 -l200000     # several times
stress-ng --class scheduler -t 60   # a few rounds
```
This is whole-kernel-spinlock-path code — a hang here would show up immediately as a system-wide stall, not a subtle regression. No hang after sustained load under both workloads is the gate before trusting the 1 ms window in any performance run. If you see *any* stall, softlockup warning, or hung-task warning with `mechanism=1` that does not reproduce with `mechanism=0`, stop and report back before doing anything else — do not attribute it to noise.

### 5.3 The actual comparison

Workload: **`hackbench -g4 -l20000`** — this is the workload established this session that actually exercises this code path (confirmed via `ivh_pv_wait_calls` in `/proc/ivh_debug`, ~3 million calls/run). Do **not** use NHextend3 as the primary test here — its lock is pure userspace `cmpxchg` and only produces ~858 `ivh_pv_wait` calls, nowhere near enough to say anything about this feature; if you want to include it for any secondary reason, `NHEXTEND_DURATION=20` minimum per the existing convention.

Arms, ≥3 rounds each, non-overlapping, report every round plus the mean:
```
echo 0 > /proc/sys/kernel/ivh_pv_wait_mechanism   # baseline: unchanged from before this patch
hackbench -g4 -l20000   (x3+)

echo 1 > /proc/sys/kernel/ivh_pv_wait_mechanism   # new: adaptive TPAUSE + real IPI wake
hackbench -g4 -l20000   (x3+)
```
For each round, also capture:
- `ivh_pv_wait_calls` delta from `/proc/ivh_debug` — confirms the path was actually exercised at the volume you expect.
- The RES (reschedule-IPI) line delta from `/proc/interrupts`, summed across CPUs — this is your direct evidence the new kicks are firing at all, and lets you watch for the "IPI storm" performance risk the plan flagged (§5.5 of the plan) even though it isn't a correctness issue.

After every comparison round, restore `ivh_pv_wait_mechanism=0` and `ivh_universal_eligible=0`, per the established protocol this session. Also check for stray `MY_ivh_atc`/`vcap` daemons (`ps -ef | grep -E "MY_ivh_atc|vcap "`) before each run, same as before.

### 5.4 The PV_UNHALT confound — still open, still applies here exactly as before

This is unchanged from earlier in this conversation and applies to this new code with **exactly the same force** it applied to the old `mechanism=0` vs `mechanism=1` comparison: this host advertises `KVM_FEATURE_PV_UNHALT` (verified live, cpuid `0x40000001` bit 7 set). That means your `mechanism=0` baseline is **not** "do nothing" — it is the real, host-cooperative halt + `KVM_HC_KICK_CPU` hypercall path, i.e. exactly the mechanism this whole project exists to replace on hosts that *don't* offer that cooperation. Testing `mechanism=1` (this new code) against `mechanism=0` on *this* host is testing IVH's guest-only mechanism against the host's own best cooperative path, not against the "no cooperation" baseline the thesis is actually about.

Say this plainly, because it's easy to forget once new code is in hand and the temptation is to treat a fresh comparison as if it settles something the old comparison didn't: **it does not.** The honest expectation, unchanged from the plan (§6): this redesign should recover most of the previously-measured 8% regression relative to `mechanism=0` (longer window cuts polling, the IPI cuts wake latency toward halt/kick levels), but landing at roughly parity-to-a-few-percent-behind `mechanism=0` on *this* host is the expected, non-alarming outcome — not a sign the code is broken. The comparison that can show a real win for IVH's thesis needs `KVM_FEATURE_PV_UNHALT` disabled on the **host** (outside this guest's control) so `mechanism=0` degrades to a plain `cpu_relax()` spin, plus real vCPU oversubscription. If you haven't arranged that host-side change yet, running §5.3 now is still useful (it tells you the code is correct and cheaper than the old TPAUSE substitute) but it cannot be reported as evidence of the thesis's central claim either way.

---

## 6. Deviations from the plan

None. The implementation matches plan §4 exactly: one widened constant (`IVH_PV_ADAPTIVE_TSC`, same value the plan specified), the role-C IPI in `pv_kick_node()` placed after the state-publishing cmpxchg (as the plan required), and the role-B IPI replacing the no-op in `ivh_pv_kick()`. The only addition beyond the plan's literal code sketch was the two `#include` lines needed to make `smp_send_reschedule()` compile (§2) — a build-mechanics necessity, not a design change.
