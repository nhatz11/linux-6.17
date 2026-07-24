# IVH `ivh_pv_wait_mechanism=2`: real HLT + reschedule-IPI wake, hypercall-less

Date: 2026-07-22
Branch: `kernel-43-clean`
Booted kernel at time of writing: `6.17.0-rseqport56-tpauseIPI+` (mechanism 2 requires a rebuild+reboot to run)

## 1. What this mechanism is

A third value of the existing sysctl `kernel.ivh_pv_wait_mechanism`:

- `0` (default, unchanged): host-cooperative. If the host advertises
  `KVM_FEATURE_PV_UNHALT`, real `halt()`/`safe_halt()` woken by the
  `KVM_HC_KICK_CPU` hypercall (byte-for-byte pre-IVH `kvm_wait()`). Otherwise
  plain `cpu_relax()` busy-spin (no yield).
- `1` (unchanged): IVH's own non-yielding TPAUSE poll (`ivh_pv_backoff`,
  `IVH_PV_ADAPTIVE_TSC` ~1 ms deadline), woken by a real `smp_send_reschedule()`
  IPI.
- `2` (NEW): a **real `halt()`/`safe_halt()`** — a genuine vCPU yield with a
  real HLT vmexit — woken by the **same real `smp_send_reschedule()` IPI** as
  mechanism 1, **with no dependency on `KVM_FEATURE_PV_UNHALT`**. This is the
  "yield like mechanism 0, but hypercall-less" mechanism.

The design intent: recover mechanism 0's genuine yielding (which is exactly why
mechanism 0 beats mechanism 1 under real contention — see
`ivh_tpause_ipi_verification_2026-07-22.md`: TPAUSE never vmexits, so the host
never learns the vCPU is idle and the non-yielding waiter competes with the
descheduled holder for the pCPU) while dropping the host-cooperative
`KVM_HC_KICK_CPU` wake in favor of a plain targeted IPI.

## 2. What changed (files / functions / real references)

All changes are strictly additive; mechanisms 0 and 1 are behaviorally
untouched (only comments that said "mechanism==1" were widened to say
"mechanism 1 or 2" where the existing nonzero-guarded code already covers 2).

### `arch/x86/kernel/kvm.c`

- **`ivh_pv_wait()`** — added a new `if (READ_ONCE(ivh_pv_wait_mechanism) == 2)`
  branch inserted **after** the `mechanism==0` block and **before** the
  `mechanism==1` deadline-poll (so 0 and 1 keep their exact control flow). The
  branch is the mechanism-0 PV_UNHALT halt sequence copied verbatim but **not**
  gated on `kvm_para_has_feature(KVM_FEATURE_PV_UNHALT)`:

  ```c
  if (READ_ONCE(ivh_pv_wait_mechanism) == 2) {
      if (irqs_disabled()) {
          if (READ_ONCE(*ptr) == val)
              halt();
      } else {
          local_irq_disable();
          if (READ_ONCE(*ptr) == val)
              safe_halt();
          else
              local_irq_enable();
      }
      return;
  }
  ```

- **`ivh_pv_kick()`** — **no logic change.** The existing structure already
  handles mechanism 2: `if (!READ_ONCE(ivh_pv_wait_mechanism)) { ...PV_UNHALT
  hypercall or no-op...; return; }` followed by an unconditional
  `smp_send_reschedule(cpu)`. Any nonzero mechanism (1 **or** 2) falls through
  to `smp_send_reschedule(cpu)`. Only the comment was updated to say
  "mechanism==1 or ==2" and to explain that mechanism 2 relies on that same IPI
  to un-halt its real HLT.

### `kernel/locking/qspinlock_paravirt.h`

- **`pv_kick_node()`** — **no logic change.** The existing
  `if (READ_ONCE(ivh_pv_wait_mechanism)) smp_send_reschedule(pn->cpu);`
  (added last session for mechanism 1) already fires for any nonzero mechanism,
  so mechanism 2's queue-node successor is woken by it. Comment widened to
  cover mechanism 2 (un-halt the HLT vs cut the TPAUSE nap) and to note
  mechanism 2's timer-tick backstop.
- **`pv_wait_early()`** — **no logic change.** The existing
  `if (!READ_ONCE(ivh_pv_wait_mechanism)) return false;` keeps mechanism 0 at
  exact upstream behavior; for any nonzero mechanism it returns
  `vcpu_is_preempted(prev->cpu)`. For mechanism 2 this means: bail out of the
  hot MCS spin into a real yielding HLT precisely when the predecessor we wait
  on is host-descheduled — which is *more* desirable for mechanism 2 than for
  mechanism 1. Comment updated to say so.

### Sysctl bounds — checked, no change needed

`ivh_pv_wait_mechanism` is registered (`ivh_pv_sysctls[]`) with
`proc_doulongvec_minmax` and **no** `.extra1`/`.extra2`. Confirmed in
`kernel/sysctl.c:__do_proc_doulongvec_minmax()`: `min = table->extra1;
max = table->extra2;` and the bound check is `if ((min && ...) || (max && ...))`
— both NULL means **no clamp**. Writing `2` is accepted verbatim; there is no
hidden ceiling that would silently clamp 2 to 1. (Verified by reading the
handler, not assumed.)

## 3. Verification of the architectural premise

### Premise A — "HLT traps unconditionally, independent of PV_UNHALT"

**Holds.** Reasoning, and how confident:

- Architecturally, HLT is a guest-privileged instruction that KVM
  virtualizes; by default it causes a VM exit (`kvm_emulate_halt` →
  `kvm_vcpu_halt`). `KVM_FEATURE_PV_UNHALT` is a *wake-side* feature (it lets
  the host wake a halted vCPU via a directed `KVM_HC_KICK_CPU` hypercall
  instead of a generic interrupt); it does not change whether HLT exits. This
  is consistent with the existing mechanism-0 code, which halts **only** when
  PV_UNHALT is present — and it does so not because HLT needs PV_UNHALT to
  trap, but because without a directed kick the upstream design chose to avoid
  HLT-based waiting entirely (falling back to `cpu_relax`). Mechanism 2's whole
  premise is that the plain reschedule IPI is a perfectly good substitute wake,
  so the PV_UNHALT gate can be dropped. **Confidence: high**, from KVM/x86
  semantics + the code's own structure.
- The one case where a guest HLT does *not* vmexit is host-side
  HLT-passthrough, which KVM enables for dedicated pCPUs / `KVM_HINTS_REALTIME`.
  `kvm_spinlock_init()` already routes `KVM_HINTS_REALTIME` to native
  qspinlock (never registers `ivh_pv_wait`), so mechanism 2's HLT path is
  unreachable in exactly the configuration where HLT wouldn't trap. **Verified
  by reading `kvm_spinlock_init()`.** This is the load-bearing safety
  interlock and it already exists.
- **Not independently live-verified** on this specific host: I did not
  instrument an actual HLT-exit counter for a mechanism-2 wait (that requires
  the rebuilt kernel). The reasoning is sound but the *empirical* confirmation
  that this host's KVM exits on these HLTs is a next-step measurement (see §5).

### Premise B — "a halted vCPU wakes on a `smp_send_reschedule()` IPI"

**Holds, and I traced it in code.** `smp_send_reschedule()` →
`smp_ops.smp_send_reschedule` = `native_smp_send_reschedule`
(`arch/x86/kernel/smp.c:292`) → `native_smp_send_reschedule()`
(`arch/x86/kernel/apic/ipi.c:68`) → `__apic_send_IPI(cpu, RESCHEDULE_VECTOR)`.
That is a genuine APIC IPI, not a paravirt construct, and KVM does **not**
override `smp_ops.smp_send_reschedule` in this tree (grep of `kvm.c` /`smp.c`
found no override). An IPI targeting a host-blocked vCPU's LAPIC is emulated by
KVM as an interrupt injection that unblocks the target — baseline
interrupt-driven wake, no PV_UNHALT required. The handler is
`DEFINE_IDTENTRY_SYSVEC_SIMPLE(sysvec_reschedule_ipi)`
(`arch/x86/kernel/smp.c:248`), which is effectively a no-op ack — waking the
CPU is the whole effect, which is exactly what we need. **Confidence: high**
for the code path; the "KVM injects and unblocks" step is standard KVM
behavior, reasoned rather than instrumented on this host.

### Net: does the premise the task prompt stated hold up?

**Yes, it holds up** — with one honest sharpening: the value of "hypercall-less"
here is real but its *performance* upside on **this** host is doubtful (§6),
because on this host mechanism 0 already has PV_UNHALT and is therefore already
getting a *cheaper* wake than mechanism 2's full IPI, while both yield
identically. The premise is architecturally correct; the expected *win* is not.

## 4. Build confirmation

```
sudo make -j$(nproc) arch/x86/kernel/kvm.o kernel/locking/qspinlock.o
  CC      kernel/locking/qspinlock.o
  CC      arch/x86/kernel/kvm.o
```

Clean compile, no warnings on either object. Build artifacts chowned back to
`nick:nick` (`find arch/x86/kernel kernel/locking -user root` → 0). This is an
incremental `.o` verification only; **a full kernel rebuild + reboot is the
user's step** and has NOT been done.

## 5. Per-invariant correctness reasoning (specific to the mechanism-2 code)

### No permanent hang (lost/misdelivered IPI is survivable)

The `safe_halt()` branch runs with IRQs enabled after the `sti;hlt`, so even if
every IPI were lost, the **timer tick** un-halts the vCPU; the PV slowpath's own
`for(;;)` (both `pv_wait_node()` and `pv_wait_head_or_lock()`) then re-checks
`node->locked`/`lock->locked` and retries. The `halt()` (already-irqs-disabled)
branch runs with IF=0, but a pending interrupt — timer tick included — still
un-halts the CPU (HLT resumes on any pending interrupt regardless of IF; the
interrupt is merely held pending until IRQs re-enable). So the worst-case wake
bound is **one timer tick**, which is tighter and more robust than mechanism 1's
explicit `IVH_PV_ADAPTIVE_TSC` deadline because HLT is wired into the normal
tick/interrupt infrastructure. **Confidence: high** — this is the same halt
sequence mechanism 0 has shipped with (and upstream `kvm_wait()` before it).

### No missed wake / lost-wakeup race

This is the classic check-then-halt race. The guard is identical to
mechanism 0's (deliberately copied verbatim):

- **`safe_halt()` branch**: `local_irq_disable()` → re-check
  `READ_ONCE(*ptr) == val` → `safe_halt()`. `safe_halt()` is `sti; hlt` and
  x86 gives `sti` a one-instruction interrupt shadow, so the pair is atomic
  w.r.t. interrupt delivery: an IPI that arrives after the re-check but before
  the halt is not delivered until *after* `hlt` begins executing, so it
  un-halts us — it cannot be consumed-and-lost in the gap. If instead the
  condition already changed, we take the `else` and `local_irq_enable()`
  without halting.
- **`halt()` branch** (caller already had IRQs off): re-check under the
  already-disabled IRQs, then `halt()`. Since IRQs are off, no interrupt is
  serviced between the re-check and the halt, and any IPI that arrives is left
  pending and un-halts the CPU. No lost wake.

Additionally, the *waker* side never races the state write: `pv_kick_node()`
publishes `pn->state = VCPU_HASHED` via `try_cmpxchg_relaxed` with
`smp_mb__before_atomic()` **before** `smp_send_reschedule()`, and the unlock
path stores `lock->locked` before `ivh_pv_kick()`'s IPI — so in both wait sites
the condition word the halted waiter re-checks is already updated before the IPI
is sent; the IPI only needs to un-halt, not to publish. **Confidence: high.**

### `irqs_disabled()` handling matches mechanism 0 exactly

Both branches exist for the reason the upstream `kvm_wait()` comment gives: PV
spinlock waits happen both with IRQs already disabled (e.g. an IRQ-context
spinlock slowpath) and with IRQs enabled. In the already-disabled case we must
**not** re-enable them (`halt()` = bare `hlt`, leaving IF as-is); in the
enabled case we must disable-then-atomically-halt-and-reenable (`safe_halt()`)
so the re-check and halt are race-free while still returning with IRQs enabled.
Mechanism 2 replicates this branch-for-branch. **Confidence: high** — it is a
literal copy of the audited mechanism-0 sequence.

### Live-toggle safety across a sysctl change

`ivh_pv_wait_mechanism` is read with `READ_ONCE` at each site and can change
between a waiter's `ivh_pv_wait()` and a waker's `ivh_pv_kick()`. Every
combination is safe: a mechanism-2 halted waiter that is later kicked under
mechanism 1 still gets `smp_send_reschedule()` (nonzero fall-through) → wakes;
kicked under mechanism 0 gets the PV_UNHALT hypercall or a no-op — in the no-op
case the timer-tick backstop still frees it. No combination strands a halted
vCPU. (The genuinely dangerous live toggle, `virt_spin_lock_key`/`pv_ops`
registration, is boot-only and untouched, per `kvm_spinlock_init()`'s comment.)
**Confidence: high.**

## 6. Do I think mechanism 2 can beat mechanism 0 on THIS host? (brutal version)

**On this host: almost certainly not — expect parity at best, more likely a
hair slower than mechanism 0, matching the user's own stated expectation.**
Here is the honest mechanics, not reassurance:

- Both mechanism 0 (with PV_UNHALT, which this host has) and mechanism 2 do the
  **same real HLT** → same vmexit → same genuine yield → same host opportunity
  to reschedule the descheduled holder. So on the *yield* axis — the axis that
  makes mechanism 0 beat mechanism 1 — mechanism 2 is **even** with mechanism 0,
  not better. There is no yield advantage to be had; mechanism 0 already yields.
- The only difference is the **wake vehicle**: mechanism 0 uses the directed
  `KVM_HC_KICK_CPU` hypercall (a lightweight, purpose-built vCPU kick);
  mechanism 2 uses a full `RESCHEDULE_VECTOR` APIC IPI that KVM must emulate
  and inject. On a PV_UNHALT-capable host the hypercall is the *cheaper* of the
  two. So mechanism 2 pays a (small) wake-side tax that mechanism 0 avoids.
- Therefore my expectation is: **mechanism 2 ≈ mechanism 0, trending slightly
  worse** under the hackbench co-runner workload — i.e. I agree with the user's
  "slightly worse but hypercall-less" framing, and I'd add that "slightly
  worse" is the *good* outcome to hope for here, because it would demonstrate
  that dropping host cooperation costs almost nothing when the host happens to
  cooperate anyway.

Where mechanism 2 should actually **win big** is the case this host can't
demonstrate: a host **without** PV_UNHALT. There, mechanism 0 degenerates to
`cpu_relax()` busy-spin — **no yield at all** — and would lose to a
host-descheduled holder exactly the way mechanism 1 does. Mechanism 2 keeps its
real HLT yield regardless. So the paper's claim to make is not "mechanism 2 >
mechanism 0 on a cooperative host" (it won't be), but "mechanism 2 delivers
mechanism-0-class yielding **without requiring** host PV cooperation, at near-
zero cost even when that cooperation is present." To *prove* that second half
you would ideally also measure on a PV_UNHALT-disabled host (e.g. boot the
guest with the host not advertising the feature, or a config that masks it),
where mechanism 0 falls back to busy-spin and mechanism 2 should clearly win.

One caveat carried over from the mechanism-1 investigation: hackbench's
context-switch floor may mask small deltas between 0 and 2 entirely. If 0 and 2
come out statistically indistinguishable, that is itself the supporting result
(hypercall-less at parity), not a null.

## 7. Next steps for the user

1. **Rebuild + reboot** the kernel (your step; not done here). Only an
   incremental `kvm.o`/`qspinlock.o` compile was verified.
2. After boot, confirm PV substitute registered: `dmesg | grep 'IVH: PV
   spinlock substitute registered'` and `cat /proc/sys/kernel/ivh_pv_wait_mechanism`
   (should read `0`).
3. Confirm the sysctl accepts 2 without clamping:
   `sudo sysctl kernel.ivh_pv_wait_mechanism=2 && cat
   /proc/sys/kernel/ivh_pv_wait_mechanism` → must print `2` (if it prints `1`
   or errors, stop — that contradicts the §2 bounds analysis and must be
   investigated before trusting any numbers).
4. **Benchmark** on the established methodology, under real co-runner
   contention (an oversubscribing co-runner on the host so lock-holder
   preemption actually occurs — without it this path is never exercised):
   - Workload: `hackbench -g4 -l20000`, **≥3 rounds** each.
   - Compare `ivh_pv_wait_mechanism` = **0 vs 1 vs 2** (set
     `ivh_universal_eligible=1` if that is what the prior rounds used to force
     the IVH path; restore to 0 after — see step 6).
   - Per round, capture `/proc/ivh_debug`'s `ivh_pv_wait_calls` (confirms the
     path is actually taken and roughly equal call counts across mechanisms so
     you're comparing like with like) and the **`RES` column of
     `/proc/interrupts`** deltas (mechanism 2 should show RES growth similar to
     mechanism 1 and *more* than mechanism 0, since 0's PV_UNHALT wake is a
     hypercall not a RES IPI — that RES delta vs mechanism 0 is the direct,
     visible signature of "hypercall-less").
5. **Optional but valuable for the paper**: repeat on a host/config where
   `KVM_FEATURE_PV_UNHALT` is **not** advertised to the guest. That is the only
   configuration where mechanism 2 is expected to clearly beat mechanism 0
   (mechanism 0 → busy-spin, mechanism 2 → real yield). Confirm with
   `dmesg`/`cpuid` that PV_UNHALT is absent in that run.
6. **Restore** after testing: `sudo sysctl kernel.ivh_pv_wait_mechanism=0` and
   `ivh_universal_eligible=0`.

## Appendix — exact edit sites

- `arch/x86/kernel/kvm.c`, `ivh_pv_wait()`: new `mechanism==2` branch between
  the `mechanism==0` block and the `mechanism==1` deadline poll.
- `arch/x86/kernel/kvm.c`, `ivh_pv_kick()`: comment only; existing
  `smp_send_reschedule(cpu)` fall-through already serves mechanism 2.
- `kernel/locking/qspinlock_paravirt.h`, `pv_kick_node()`: comment only;
  existing `if (READ_ONCE(ivh_pv_wait_mechanism)) smp_send_reschedule(pn->cpu)`
  already serves mechanism 2.
- `kernel/locking/qspinlock_paravirt.h`, `pv_wait_early()`: comment only;
  existing nonzero-guarded `vcpu_is_preempted()` early-bail already serves
  mechanism 2.
