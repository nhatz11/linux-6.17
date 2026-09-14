# IVH adaptive-TPAUSE + targeted-IPI wake: implementation plan and go/no-go

**Date:** 2026-07-22
**Kernel:** `6.17.0-rseqport55-trimsys+`, branch `kernel-43-clean`
**Status:** PLAN for your approval — no code written. Decide GO / NO-GO / GO-WITH-SCOPE at the end.
**Builds on:** `ivh_six_goals_report_2026-07-22.md` §4 (Fable). Every code reference below was re-read against the live tree; where I disagree with §4 I say so and show why.

---

## 0. TL;DR — the recommendation, up front

**Conditional GO, with the experiment reframed.** The mechanism is buildable, low-risk *to build correctly* (the existing bounded-poll design makes it structurally impossible to deadlock even if every IPI is lost), and the phase-1 scope is *broader and cheaper* than §4 concluded: build **role C (MCS node waiters) AND role B (queue head), both**, because role B's targeted kick turns out to be a ~2-line change §4 mistakenly ruled out. Role A doesn't exist on this path at all.

**But the honesty that has to come first:** the 8% regression you measured is against the *wrong baseline for IVH's thesis*, and I can prove it. On this exact host, **the host advertises `KVM_FEATURE_PV_UNHALT`** (verified live — see §2). That means your `ivh_pv_wait_mechanism=0` baseline is not "do nothing" — it is the **real host-cooperative halt + hypercall-kick path**, byte-for-byte the pre-IVH `kvm_wait()`/`kvm_kick_cpu()`. So the 8% number is: *IVH's deliberately-non-cooperative busy-wait losing to the host-cooperative mechanism it was designed to replace on hosts that don't offer cooperation.* Beating host-cooperative halt/kick with a guest-only spin loop is **not realistic and is not the goal.**

The baseline that actually tests IVH's thesis is `mechanism=0` **with PV_UNHALT disabled on the host** (degrades to plain `cpu_relax()` busy-spin), under **real vCPU oversubscription**. Both conditions are host-side, outside this guest's control. Against *that* baseline the redesign has a genuine shot at a win. Against the *current* PV_UNHALT-on baseline, my honest expectation is **neutral-to-slightly-negative — recover most of the 8%, probably not beat halt/kick.** If you can't reconfigure the host, you cannot demonstrate a win on this box, and the paper's central experiment is blocked on host access, not on this code.

I'd still build it — the code is cheap, correct, and needed regardless — but go in knowing the win lives in a host regime you have to arrange, and say so in the paper rather than reporting an 8% loss as if it were the verdict.

---

## 1. What's actually there today (re-verified, not trusted)

**The two `pv_wait()` call sites — there are exactly two, confirmed by grep:**

| Role | Call site | Waits on (the `ptr`/`val` passed to `ivh_pv_wait`) | Has predecessor CPU identity? | Wake vehicle today |
|---|---|---|---|---|
| **C** — 2nd+ MCS node | `pv_wait_node()` `qspinlock_paravirt.h:342` | `&pn->state == VCPU_HALTED` | **Yes** — `prev`/`pp->cpu`, a real `struct pv_node`, passed into the function | `pv_kick_node()` during MCS handoff (`qspinlock.c:405`) — **currently sends no IPI**, only advances state HALTED→HASHED + hashes |
| **B** — queue head | `pv_wait_head_or_lock()` `qspinlock_paravirt.h:486` | `&lock->locked == _Q_SLOW_VAL` | No — it waits on the lock word, has no predecessor node | `pv_kick(node->cpu)` in `__pv_queued_spin_unlock_slowpath()` `:559` — **currently `ivh_pv_kick()` no-ops when `mechanism=1`** |

**Role A (the pending-bit waiter) is not on this path at all.** In `queued_spin_lock_slowpath()` the pending-bit waiter uses `smp_cond_load_acquire(&lock->locked, !VAL)` (`qspinlock.c:~211`) — a plain `cpu_relax` spin, it never calls `pv_wait`. So the "first two waiters" framing in §4 over-counts for *this* mechanism: only role C and role B ever enter `ivh_pv_wait()`. Role A can't be given an IPI wake because it never sleeps in a kickable primitive; it also doesn't need one (pending bit is the front of the line, its wait is the shortest). **A is out of scope structurally, not "deferred."**

**A correction to §4 on role B.** §4 filed role B (queue head) under "no usable owner-CPU identity, needs new bookkeeping, defer." That's true for the *wait-early / predecessor-health* direction (the queue head has no predecessor node to check `vcpu_is_preempted()` on). But it is **false for the wake direction**: the unlock slowpath already recovers the queue head's CPU via `pv_unhash(lock)` → `node->cpu`, and upstream's own comment (`qspinlock_paravirt.h:551-556`) guarantees that read is safe even after the lock memory is freed ("we can still use the pv_node to kick the CPU... kicking an active vCPU is harmless"). So **role B's targeted kick is buildable right now, in ~2 lines, and it targets the single most latency-critical waiter in the whole queue** (the one next to acquire). That's why I recommend including it in phase 1, not deferring it.

**`ivh_pv_wait()` / `ivh_pv_backoff()` today** (`arch/x86/kernel/kvm.c:1117-1218`): when `mechanism=1`, a bounded self-poll — `deadline = rdtsc() + IVH_PV_WAIT_TSC` (65536 cyc ≈ 22 µs at 3 GHz), napping in `IVH_PV_TPAUSE_CYCLES` (512 cyc ≈ 170 ns) TPAUSE-C0.2 windows, re-checking `*ptr` each iteration, returning on change or deadline. `ivh_pv_kick()` is a documented no-op under `mechanism=1`. WAITPKG is present on this CPU (verified), so `__tpause` is live, not the `cpu_relax` fallback.

**The `pv_wait_early()` steal-check already exists** (`qspinlock_paravirt.h:286-289`): under `mechanism=1` it returns `vcpu_is_preempted(prev->cpu)` for role C, bailing the hot MCS spin into `ivh_pv_wait()`'s backoff when the predecessor's vCPU is host-preempted. This is the "is prev healthy" logic the user asked for — **it is already written and already reads the right signal (the KVM steal bit via `vcpu_is_preempted`), for role C only.** Nothing to add there; the redesign builds *on* it.

---

## 2. Live environment facts that decide the framing

Verified on the booted kernel this session (not from memory):

- **`KVM_FEATURE_PV_UNHALT` = 1** (cpuid `0x40000001` EAX = `0x01007efb`, bit 7 set). **This is the load-bearing fact.** `mechanism=0` on this host = real halt + hypercall kick.
- `KVM_FEATURE_PV_SEND_IPI` = 1 (bit 11) → guest IPIs are PV-accelerated; `kvm_send_ipi_mask` is installed. A targeted IPI here is cheap-ish.
- `KVM_FEATURE_STEAL_TIME` = 1 (bit 5) → `vcpu_is_preempted()` reads a real, live signal.
- WAITPKG present → `__tpause` C0.2 is the real backoff, not `cpu_relax`.
- Boot log confirms `kvm-guest: IVH: PV spinlock substitute registered` — so `kvm_spinlock_init()` took the PV path (not `KVM_HINTS_REALTIME`, not single-CPU, not `nopvspin`); `pv_ops.lock.wait/kick = ivh_pv_wait/ivh_pv_kick` are installed.
- 16 vCPUs. `ivh_pv_wait_calls` was at ~3.5M from your prior hackbench run (confirms hackbench heavily exercises this path; NHextend3 barely does, as you found).
- Sysctls currently safe: `ivh_pv_wait_mechanism=0`, `ivh_universal_eligible=0`.

**Implication:** TPAUSE C0.2 does **not** vmexit — it never yields the pCPU back to the host, it only yields the SMT sibling thread and some power. Halt (the `mechanism=0` path here) *does* vmexit and hands the pCPU to the host to run another vCPU — including, potentially, the actual lock holder. On an oversubscribed host that is a real structural advantage for halt that no amount of guest-side TPAUSE tuning can replicate. IVH's bet is only that the halt→host-reschedule→hypercall-kick→vmentry *roundtrip* (plus the steal window at the wake boundary) costs more than it saves — and that bet only pays when the host was **not** going to helpfully reschedule you anyway, i.e., when it offers no PV_UNHALT cooperation. Which is exactly the baseline you can't see on this host without host-side changes.

---

## 3. The coverage question, answered definitively

**User's sub-task:** does `ivh_pv_wait()`/`ivh_pv_kick()` fire uniformly across `raw_spin_lock`, `_irq`, `_irqsave`, `_bh` and their unlocks, or only some — and does that change where the kick hooks in?

**Answer: uniform. All variants funnel through exactly one lock chokepoint and one unlock chokepoint. This is cleaner than the `ivh_pre_lock`/`cs_enter`/`cs_exit` coverage (which §4.4 correctly found is per-variant in `spinlock.c`).** Traced through the real headers:

- `_raw_spin_lock{,_irq,_irqsave,_bh}` → (via `LOCK_CONTENDED` / `__raw_spin_lock_*` in `spinlock_api_smp.h`) → `do_raw_spin_lock()` → `arch_spin_lock(&lock->raw_lock)` (`spinlock.h:187`) → `queued_spin_lock()` (`qspinlock.h:147`) → on contention `queued_spin_lock_slowpath()` → `pv_queued_spin_lock_slowpath` → `__pv_queued_spin_lock_slowpath`. **One path.** The `_irq`/`_irqsave`/`_bh` variants differ *only* in the local-irq / preempt / softirq bookkeeping wrapped *around* the identical arch primitive — they do not touch the lock word or the pv path differently.
- `_raw_spin_unlock{,_irq,_irqrestore,_bh}` → `do_raw_spin_unlock()` → `arch_spin_unlock()` → `queued_spin_unlock()` (`qspinlock.h:54`) → `pv_queued_spin_unlock` → `__pv_queued_spin_unlock` → (slowpath) `__pv_queued_spin_unlock_slowpath`. **One path.**

**Consequence for the kick hooks:** you do **not** need per-variant handling. Both injection points below sit *below* the irq/bh distinction, so `raw_spin_unlock_irqrestore()` (your common case) and plain `raw_spin_unlock()` reach the identical `pv_kick`/`pv_kick_node` sites. The specific worry in the task ("if `_irqrestore` doesn't go through the same release path as plain unlock, the kick point needs to account for it") **does not arise** — they do go through the same path. One caveat that *is* real: the kick fires inside `queued_spin_lock_slowpath`/`__pv_queued_spin_unlock_slowpath`, which for `_irqsave`/`_bh` callers runs with **IRQs or softirqs already disabled by the caller**. `smp_send_reschedule()` is safe from IRQ-disabled context (it's used from the scheduler under `rq->lock`), so this is fine — but the implementer must not add anything that could sleep or re-enable IRQs there.

**rwlock is not covered — structurally, same as §4.4's finding, and for the same reason.** `read_lock`/`write_lock` use `queued_read_lock`/`queued_write_lock` (qrwlock), a **separate primitive**. Read/write waiters spin on the rwlock word via `atomic_cond_read` and never call `pv_wait`. The qrwlock slowpath touches the pv path only via its internal `wait_lock` (`arch_spin_lock(&lock->wait_lock)`, `qrwlock.c:43`) — a brief spinlock, not the rwlock wait itself. So **rwlock read/write waiters get no adaptive-TPAUSE, no IPI wake, from this mechanism.** Whether that matters is a workload question (how much real rwlock contention exists); it is not fixable inside this design without separately instrumenting qrwlock.

---

## 4. Concrete implementation sketch (real names, real sites)

Two independent, small changes plus one constant. Both gated on `READ_ONCE(ivh_pv_wait_mechanism)` so `mechanism=0` stays byte-for-byte stock. All new code is `#ifdef CONFIG_PARAVIRT_SPINLOCKS` (already the case for these files in this `.config`).

### 4.1 Role C — MCS node waiter (the user's primary ask)

**(a) Longer nap window** — `arch/x86/kernel/kvm.c`. Add a wider deadline for the adaptive path. `IVH_PV_WAIT_TSC = 65536` (~22 µs) is fine when there's no wake and you're relying on the deadline to re-poll; with a real IPI cutting the nap short, you want the deadline *long* so you're not needlessly re-entering the poll loop:

```c
#define IVH_PV_ADAPTIVE_TSC   (3000000ULL)   /* ~1 ms at 3 GHz; the IPI, not
                                                this deadline, is the common
                                                wake. Deadline is the backstop. */
```

Use `IVH_PV_ADAPTIVE_TSC` as the deadline in `ivh_pv_wait()`'s `mechanism=1` branch. Keep `IVH_PV_TPAUSE_CYCLES` per-nap granularity modest (512–4096); TPAUSE C0.2 re-arms each loop iteration and the IPI resumes whichever nap is in flight. **`ivh_pv_wait()`'s do-while body does not otherwise change** — the existing `if (READ_ONCE(*ptr) != val) return;` + `ivh_pv_backoff()` loop already does the right thing when an IPI resumes the `__tpause`: it re-checks the byte and returns if it changed, re-naps if not. The whole change on the wait side is the constant.

**(b) The real kick** — `kernel/locking/qspinlock_paravirt.h`, in `pv_kick_node()` (line 377), which the MCS owner calls after `arch_mcs_spin_unlock_contended(&next->locked)` (`qspinlock.c:404-405`). Today it advances the successor's state and returns. Add, gated:

```c
static void pv_kick_node(struct qspinlock *lock, struct mcs_spinlock *node)
{
    struct pv_node *pn = (struct pv_node *)node;
    u8 old = VCPU_HALTED;

    smp_mb__before_atomic();
    if (!try_cmpxchg_relaxed(&pn->state, &old, VCPU_HASHED))
        return;                      /* successor still spinning; no kick */

    WRITE_ONCE(lock->locked, _Q_SLOW_VAL);
    (void)pv_hash(lock, pn);

    /* IVH: the successor is napping in ivh_pv_wait() on &pn->state; we just
     * moved it HALTED->HASHED (the cmpxchg is a full barrier and orders that
     * write before the IPI). Cut its TPAUSE short with a content-free IPI;
     * TPAUSE resumes on any unmasked interrupt, the re-check sees state !=
     * VCPU_HALTED and returns. If mechanism==0 this is unreachable in a way
     * that matters (the halt/hypercall path handles its own wake). */
    if (READ_ONCE(ivh_pv_wait_mechanism))
        smp_send_reschedule(pn->cpu);
}
```

- **Why `pn->cpu` is safe here:** `next` is a live per-CPU `qnode` of a CPU currently in the queue; `pn->cpu` was set at `pv_init_node()` (`smp_processor_id()`). No lifetime hazard.
- **Why the ordering is already correct:** the `try_cmpxchg_relaxed` (full barrier via `smp_mb__before_atomic()` + RMW) publishes `pn->state = VCPU_HASHED` *before* the IPI. The woken waiter's post-`__tpause` `READ_ONCE(*ptr)` therefore observes the change. **If an implementer moves the IPI before the state write, the result is a benign wasted wake (waiter resumes, sees HALTED, re-naps to deadline) — not a hang, but it defeats the purpose. Keep the IPI last.**
- **Why the gate on the cmpxchg is the right kick throttle:** we only IPI when the successor actually reached `VCPU_HALTED` (i.e., is in `ivh_pv_wait`). A successor still hot-spinning in the `SPIN_THRESHOLD` loop (`VCPU_RUNNING`) fails the cmpxchg and gets no IPI — it'll see `node->locked=1` on its own. This naturally suppresses IPIs on lightly-contended fast handoffs.

### 4.2 Role B — queue head (recommend including; ~2 lines)

`arch/x86/kernel/kvm.c`, `ivh_pv_kick()` (line 1205). It's called from `__pv_queued_spin_unlock_slowpath()` `:559` `pv_kick(node->cpu)`, which runs **after** `smp_store_release(&lock->locked, 0)` (`:549`). So `lock->locked` is already `0` (≠ `_Q_SLOW_VAL`) before the kick — the queue head napping in `ivh_pv_wait(&lock->locked, _Q_SLOW_VAL)` will, on being resumed, see the change and return. Make the kick real under `mechanism=1`:

```c
static void ivh_pv_kick(int cpu)
{
    if (!READ_ONCE(ivh_pv_wait_mechanism)) {
        if (kvm_para_has_feature(KVM_FEATURE_PV_UNHALT))
            ivh_pv_hypercall_kick(cpu);      /* unchanged stock path */
        return;
    }
    /* IVH mechanism: real targeted wake for the hashed queue head.
     * lock->locked was already store-released to 0 before pv_kick(node->cpu)
     * in __pv_queued_spin_unlock_slowpath(), so the ordering is correct. */
    smp_send_reschedule(cpu);
}
```

- **Why `cpu` is safe:** it's `node->cpu` from `pv_unhash(lock)`; upstream's comment at `qspinlock_paravirt.h:551-556` already guarantees this read is valid post-unhash and kicking an active vCPU is harmless. We are doing exactly what stock `pv_kick` was designed to do — just with a reschedule IPI instead of the KVM halt-wake hypercall.

### 4.3 What is new vs. what already exists — summary

| Piece | State today | Change |
|---|---|---|
| `pv_wait_early()` predecessor steal-check (role C) | **Exists**, `mechanism`-gated, reads `vcpu_is_preempted(prev->cpu)` | none |
| `ivh_pv_wait()` bounded TPAUSE poll | Exists, 22 µs window | swap deadline constant → ~1 ms (`IVH_PV_ADAPTIVE_TSC`) |
| `ivh_pv_backoff()` TPAUSE C0.2 | Exists | none (optionally lengthen per-nap) |
| Role-C targeted wake | **Missing** (`pv_kick_node` sends no IPI) | +4 lines in `pv_kick_node()` |
| `ivh_pv_kick()` role-B wake | **No-op** under `mechanism=1` | make it `smp_send_reschedule(cpu)` |
| Role A (pending-bit) | Not on this path | out of scope, no change |
| rwlock waiters | Not on this path | out of scope, no change |

That is the entire phase-1 surface: **one constant + two small gated additions**, both on already-installed hooks. No new subsystem, no `struct qspinlock` growth, no side hash table (§4's roles-A/B "option 1/option 2" heavy engineering is not needed because A is absent and B's wake identity already exists via `pv_unhash`).

---

## 5. Risk and failure-mode analysis (concrete, not abstract)

This touches `pv_kick_node` and `ivh_pv_kick`, which run on **every contended handoff of every raw spinlock in the kernel**. Blast radius is system-wide. The reasons this is nonetheless *low-risk to get right*:

1. **The bounded-poll invariant makes a lost/missed IPI non-fatal — this is the single most important property.** `ivh_pv_wait()` always returns by `IVH_PV_ADAPTIVE_TSC` even with zero IPIs, and both callers (`pv_wait_node`, `pv_wait_head_or_lock`) re-check `node->locked`/`lock->locked` in their own `for(;;)`. So the worst case of a *completely broken* IPI path is **added latency up to ~1 ms per nap, never a hang**. The IPI is a pure latency optimization layered on a mechanism that is already correct without it. Lead the paper's safety argument with this.

   *Corollary risk of the longer window:* raising the deadline from 22 µs → 1 ms **widens exactly this worst case**. If the IPI silently fails to arrive (e.g., a masking bug, wrong CPU), latency degrades to ~1 ms per handoff — catastrophic for throughput though still not a deadlock. So the longer window and the working IPI are a package: shipping the 1 ms window *without* verifying the IPI actually lands is the real danger. Gate the window widening behind confirmed IPI delivery in testing.

2. **A spurious/extra IPI is harmless.** `smp_send_reschedule` to a running CPU just flags a resched check; to a napping CPU it resumes `__tpause`, which re-checks the condition byte and re-naps if unchanged. **It can never cause a double-acquire**, because acquisition is gated by the atomic lock-word cmpxchg in `queued_spin_lock_slowpath`/`pv_wait_head_or_lock`, not by the wake. Waking ≠ owning.

3. **The one genuine correctness obligation: order the releasing byte-write before the IPI.** Role C: `pn->state = VCPU_HASHED` (the cmpxchg, full barrier) before the IPI. Role B: `smp_store_release(&lock->locked, 0)` before `pv_kick`. Both are *already* correctly ordered in the current code; the implementer's job is not to break that order. Get it wrong → benign wasted wakes (see §4.1), not corruption.

4. **`pn->cpu` staleness.** Not a hazard: role C's `next` is a live queued node; role B's `node` comes from `pv_unhash` and upstream already sanctions the post-unhash `node->cpu` read. No change to those lifetimes.

5. **IPI storms on hot locks — the real *performance* risk (not correctness).** Every slowpath handoff on a very hot lock now potentially sends an IPI (~1–3 µs each, PV-accelerated here but not free). On a lock with thousands of handoffs/sec across many CPUs this can add measurable IPI overhead and even self-inflicted interrupt pressure. Mitigations, in order of preference: (a) the `VCPU_HALTED` cmpxchg gate in `pv_kick_node` already suppresses IPIs for fast, uncontended handoffs; (b) consider only IPI-ing when the successor was observed preempted (`vcpu_is_preempted(pn->cpu)`) — spend the IPI only where the waiter is likely actually napping long; (c) a per-CPU rate limit as a last resort. **Recommend building (a) first (it's free), measure IPI rate via `/proc/interrupts` RES counter delta, add (b) only if the rate is high.**

6. **IRQ-disabled context.** The kick fires with caller IRQs/softirqs possibly disabled (`_irqsave`/`_bh`). `smp_send_reschedule` is safe there (scheduler uses it under `rq->lock`). Do not add anything sleepable.

---

## 6. Will it beat baseline? Honest expectation

**Against the current `mechanism=0` baseline on this host (PV_UNHALT on = halt + hypercall-kick): no, expect neutral-to-slightly-negative.** The redesign should *recover most of the 8%* — the longer window cuts polling/cache traffic, the IPI cuts wake latency to near halt/kick levels, and the existing steal-check stops spinning for preempted predecessors. But halt yields the pCPU to the host (TPAUSE does not), so on any oversubscription halt/kick retains a structural edge this code cannot erase. Predict: closing most of the gap, landing at roughly parity-to-a-few-percent-behind. Reporting that as a "win" would be dishonest.

**Against the thesis-relevant baseline (`mechanism=0` with PV_UNHALT *off* = plain `cpu_relax` busy-spin), under real oversubscription: plausible win, and this is the experiment worth running.** `cpu_relax` busy-spins hot and is preemption-blind; the adaptive path stops burning a pCPU spinning for a host-preempted predecessor (the steal-check), naps in a low-power C0.2 state that yields the SMT sibling, and reacts promptly on the real unlock (IPI). That is precisely the LHP regime IVH targets. **Both enabling conditions are host-side** (see §7) — this guest cannot create them alone.

**Sober caveat even there:** TPAUSE does not free the pCPU for another *VM*, only the SMT sibling thread. So the co-runner-visible-benefit hypothesis (§7.3) is weaker than it sounds for a non-halting mechanism — the whole point of not halting is to *keep* the pCPU. Expect co-runner benefit mainly on SMT-sibling co-location, little cross-core. If you want a neighbor VM to reclaim a full core, you have to give it up (halt) — which contradicts the non-halting thesis. Worth stating outright.

---

## 7. Test plan

Paired methodology, same as established this session. **After every comparison, restore `ivh_pv_wait_mechanism=0` and `ivh_universal_eligible=0`.** Daemons: check `ps -ef | grep -E "MY_ivh_atc|vcap "`; `vcap` cwd must be `/home/nick/vsched_main/vcapacity`; load `vsched_module.ko` first if needed.

### 7.1 Guest-side, testable now (but only re-measures the wrong baseline)

- **Workload:** `hackbench -g4 -l20000` (drives ~3M `pv_wait` calls/run; NHextend3 does **not** exercise this path — its lock is pure userspace `cmpxchg` — so do not use it as the primary test here. If NHextend3 is used for anything, `NHEXTEND_DURATION=20` minimum.)
- **Metric:** wall-clock (≥3 rounds each, report all rounds + mean, non-overlapping), plus **`ivh_pv_wait_calls` delta** from `/proc/ivh_debug` (confirms the path is exercised), plus **RES-IPI delta** from `/proc/interrupts` (confirms the kick fires *and* watches for IPI storms per §5.5).
- **Arms:** `mechanism=0` vs `mechanism=1`, prototype vs current. Expect (per §6): prototype `mechanism=1` recovers most of the current 8% gap but likely stays at/behind `mechanism=0` *on this host*, because `mechanism=0` here is halt/kick. **This confirms the code works and is faster than the current TPAUSE substitute; it does not and cannot show a thesis win on this host.** Frame it that way.
- **Correctness soak:** `hackbench -g16 -l200000` several times + `stress-ng --class scheduler` with `mechanism=1` — a deadlock/hang here surfaces immediately (whole-kernel spinlocks). No hang after sustained load is the correctness gate before trusting the 1 ms window.

### 7.2 The experiment that actually tests the thesis (needs host-side setup)

You must arrange these on the **host**, not from inside the guest:

1. **Disable PV_UNHALT for the guest** so `mechanism=0` degrades to `cpu_relax` — the honest IVH baseline. QEMU: drop the feature, e.g. `-cpu host,-kvm-pv-unhalt` (or the libvirt `<feature policy='disable' name='kvm-pv-unhalt'/>` equivalent). Re-verify inside the guest with the cpuid check from §2 (bit 7 should read 0). *Until this is done, no run on this host distinguishes IVH's mechanism from host-cooperative halt/kick.*
2. **Create real vCPU oversubscription / unpinning.** LHP only exists when vCPUs contend for pCPUs. Either pin N guest vCPUs to fewer than N pCPUs, or co-schedule a second busy VM on the same pCPUs, or simply leave vCPUs unpinned on a loaded host. **Pinned 1:1 with no oversubscription has no LHP to mitigate — the redesign is expected to be neutral there by construction** (kvm.c's own `KVM_HINTS_REALTIME` path even drops PV entirely in that case).
   - *Testable from inside the guest:* the workload and all guest-side metrics. *Not controllable from inside the guest:* the pCPU:vCPU ratio and pinning — that is the host operator's job. State this split explicitly in the paper's methodology.
3. **Arms for the thesis experiment:** (PV_UNHALT off) `mechanism=0` [cpu_relax] vs `mechanism=1` [adaptive TPAUSE+IPI], under oversubscription, ≥3 rounds. **This is the comparison that can produce a real win**; §6 argues it plausibly does.

### 7.3 Co-runner-visible-benefit experiment (design; realistic scope)

Hypothesis: when THIS VM's workload spins less aggressively, a neighbor VM sharing host resources sees higher throughput.

- **Setup (host):** two guests on overlapping pCPUs; this guest runs `hackbench` (heavy `pv_wait`), neighbor runs `sysbench` (cpu or oltp), measure neighbor throughput. Arms: this VM at `mechanism=0`(cpu_relax) vs `mechanism=1`(adaptive), PV_UNHALT off.
- **Honest expectation (from §6 caveat):** benefit is real only where TPAUSE's SMT-sibling yield or the steal-bail's reduced spin actually frees a resource the neighbor can use — i.e., mainly **SMT-sibling co-location**. Cross-core, a non-halting mechanism keeps its pCPU and the neighbor sees little. So structure the experiment to *distinguish* SMT-sibling co-location from separate-core co-location; a null result cross-core is expected, not a failure.
- **Realism:** design is straightforward; execution needs a two-VM host testbed and host-side pinning you don't currently control from this guest. **Realistic next session only if that testbed exists; otherwise it's a "future work / needs infra" item.** Don't block phase-1 code on it.

### 7.4 What's realistic when

- **This/next session, in-guest:** §7.1 (prototype vs current, correctness soak). Confirms the code is correct and beats the *current* TPAUSE substitute.
- **Needs host access (you, on the host):** §7.2 PV_UNHALT-off + oversubscription — **the only setup that can show a thesis win.**
- **Needs a two-VM testbed:** §7.3 co-runner — design ready, execution gated on infra.

---

## 8. Decision

**Recommended: GO on phase 1 = role C (longer window + `pv_kick_node` IPI) + role B (`ivh_pv_kick` reschedule IPI).** Small, correct, reuses installed hooks, and the bounded-poll invariant makes it deadlock-safe by construction. Roles A and rwlock are out of scope (A structurally, rwlock needs separate qrwlock work).

**But approve it with eyes open on the win:** on this host, as configured, the redesign is expected to *recover most of the measured 8%*, **not beat `mechanism=0`** — because `mechanism=0` here is host-cooperative halt/kick, which IVH's thesis is designed to avoid, not out-spin. The demonstrable win lives in a host regime (**PV_UNHALT off + oversubscription**) that only the host operator can create. If you cannot reconfigure the host, build the code anyway (it's cheap and needed), report it honestly as "at parity with, and structurally cheaper than, the current TPAUSE substitute; thesis-win experiment blocked on host access," and do **not** dress the on-host neutral result up as a win.

**If you want to reduce this to the single cheapest thing that de-risks the whole idea:** do §7.2 step 1 (ask the host operator to disable `kvm-pv-unhalt`) and re-run §7.1's *current* mechanism=0 vs 1 — **before writing any code.** If plain `cpu_relax` (PV_UNHALT off) already beats or ties the current TPAUSE substitute, the redesign's ceiling is low and you've learned it for the cost of one host reboot. If the current substitute already beats cpu_relax under oversubscription, the redesign (which strictly improves on the substitute) is worth building. That one host-side experiment is worth more than any amount of in-guest tuning against the wrong baseline.
