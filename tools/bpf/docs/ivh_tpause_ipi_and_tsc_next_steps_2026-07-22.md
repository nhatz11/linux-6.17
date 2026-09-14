# IVH: can we dodge HLT/hypercalls for adaptive spinning, and can TSC replace steal-time? — dispatch for a fresh session

Date: 2026-07-22. Kernel at time of writing: `6.17.0-rseqport56-tpauseIPI+`,
branch `kernel-43-clean`, repo `/home/nick/kernels/linux-6.17-rseqport`.

**This doc is written to be read with zero other context.** It is a static
research dispatch, not a build/test log — see the safety note below.

## Safety note (why this doc has no new benchmark numbers)

Earlier in the session that produced this doc, this exact VM froze and had to
be hard-restarted during live testing (`stress-ng` + heavy `hackbench` +
flipping `ivh_pv_wait_mechanism` under real host contention from a co-runner
VM). Root cause was never identified (no panic/lockup signature in
`journalctl`, but absence of evidence isn't confirmation either way — ordinary
test commands don't log to journald regardless of outcome). Given that, this
dispatch is **static analysis only**: reading kernel/KVM source, existing
docs, `git log`. No `stress-ng`, no heavy `hackbench`, no live
`ivh_pv_wait_mechanism` flips under load were run to produce anything below.
Where a claim would benefit from a live measurement, that's flagged explicitly
as unverified rather than assumed.

## Read-first pointer list, in this order

1. This doc (you're here) — the map.
2. `tools/bpf/docs/ivh_tpause_ipi_verification_2026-07-22.md` — the real,
   measured reason `ivh_pv_wait_mechanism=1` (TPAUSE+IPI) regresses ~36%
   under real host contention. Read this in full; §4 of this doc leans on it
   heavily.
3. `tools/bpf/docs/ivh_halt_ipi_mechanism2_implementation_2026-07-22.md` — the
   design and correctness reasoning for `ivh_pv_wait_mechanism=2` (real
   HLT + plain IPI, no `KVM_HC_KICK_CPU`). Implemented, **not yet booted**.
4. `tools/bpf/docs/ivh_six_goals_report_2026-07-22.md`, §6 ("steal-time →
   TSC") — an earlier, independent pass at the same TSC question this doc's
   Tasks 2/3 answer in more depth. Not contradicted below, built on.
5. `arch/x86/kernel/kvm.c` — `ivh_pv_wait()` / `ivh_pv_backoff()` /
   `ivh_pv_kick()` / `kvm_spinlock_init()`, the actual mechanism-0/1/2 code.
6. `kernel/locking/qspinlock_paravirt.h` — `pv_wait_node()`,
   `pv_kick_node()`, `pv_wait_early()`, `pv_wait_head_or_lock()`.
7. `kernel/sched/cputime.c:256-286` (`steal_account_process_time()`) and
   `kernel/sched/fair.c:13207-13301` (`ivh_steal_imminent()` /
   `ivh_rq_capacity_and_timeleft_ok()`) — Task 2's target.
8. `/home/nick/vsched_main/vcapacity/main.cpp` — Task 3's target.

---

## 0. What IVH is (self-contained background)

**IVH** = a guest-side, hypervisor-agnostic mechanism to mitigate
**Lock-Holder Preemption (LHP)** in oversubscribed KVM guests: a host that
time-slices physical CPUs across multiple VMs can preempt a guest vCPU at any
point, including while that vCPU's thread holds a lock. Every other thread
waiting on that lock — inside the same guest or (worse) spinning on other
vCPUs — burns real CPU cycles waiting for a holder the *host*, not the guest,
has descheduled, and the guest OS has no visibility into or control over that
host scheduling decision by default.

This project attacks LHP two, mostly-independent ways, both in this exact
kernel tree:

- **Pre-lock migration** (`kernel/sched/fair.c`'s `ivh_pre_lock()` /
  `bpf_sched_pre_lock_migrate()`, `kernel/sched/bpf_sched.c`,
  `sys_ivh_cs_enter()`/`kernel/rseq.c`): before a userspace thread takes a
  *userspace* lock, check whether its current vCPU looks "in danger" (recently
  preempted by the host, or capacity below a threshold — the
  `ivh_steal_imminent()` gates, Task 2 below) and, if so, migrate the thread to
  a healthier vCPU first. This is the part of the project referenced by this
  session's memory notes (`lock_depth`, Hot Threads, NHextend3, `vcap`).
- **PV-spinlock wait/kick substitute** (`arch/x86/kernel/kvm.c`,
  `kernel/locking/qspinlock_paravirt.h`): replaces the *kernel's own* internal
  contended-spinlock wait/wake primitives (used by every `spin_lock()` in the
  kernel that contends, not just IVH's own code) with something that avoids
  the stock `halt()`/`KVM_HC_KICK_CPU` host-cooperative pair. This is what
  Task 1 below is about, and what mechanisms 0/1/2 (next section) are.

Both share the same underlying steal-time infrastructure (`rq->last_preemption`,
`vcpu_is_preempted()`, `paravirt_steal_clock()`) — which is why Task 2's
question ("replace steal-time's derivation with TSC") matters to both halves
of the project, not just the PV-spinlock half.

## 1. What was tried this session: `ivh_pv_wait_mechanism` 0/1/2

Sysctl `kernel.ivh_pv_wait_mechanism`, read with `READ_ONCE()` in
`ivh_pv_wait()`/`ivh_pv_kick()` (`arch/x86/kernel/kvm.c`) and
`pv_wait_early()`/`pv_kick_node()` (`kernel/locking/qspinlock_paravirt.h`):

| value | wait | wake | yields the pCPU? | measured/expected result |
|---|---|---|---|---|
| **0** (default) | if host has `KVM_FEATURE_PV_UNHALT`: real `halt()`/`safe_halt()`. Else: plain `cpu_relax()` busy-spin. | `KVM_HC_KICK_CPU` hypercall (only if PV_UNHALT). | **Yes**, when PV_UNHALT present (this host has it). No, in the fallback. | Baseline. Byte-for-byte pre-IVH `kvm_wait()`. |
| **1** | TPAUSE (WAITPKG), ~1.36 ms deadline (`IVH_PV_ADAPTIVE_TSC`). | real `smp_send_reschedule()` IPI. | **No.** TPAUSE measured natively executing in 37 ns — confirmed no vmexit. | **Measured ~36% regression** under real host contention (§1 doc). |
| **2** | real `halt()`/`safe_halt()`, **unconditional**, not gated on PV_UNHALT. | same `smp_send_reschedule()` IPI as mechanism 1. | **Yes**, unconditionally. | Not yet booted/measured. Expected ≈ mechanism 0 on *this* host (which already has PV_UNHALT — no yield advantage to gain, only a slightly more expensive wake vehicle). Expected to actually beat mechanism 0 only on a host **without** PV_UNHALT. |

**The root-cause finding that matters most for Task 1**: mechanism 1's
regression is not a bug and not fixable by retuning the deadline window. It is
a direct, measured consequence of TPAUSE structurally never causing a vmexit
— confirmed at the microbenchmark level (`scratchpad/tpause_cost.c`: expired
TPAUSE = 37 ns, consistent with native execution, not the thousands of cycles
a trapped instruction would cost) and at the histogram level (82.7% of long
waits return with the polled condition *still false* — the waiter didn't miss
a wake, the holder genuinely never got to run, because the non-yielding
waiter was still occupying a pCPU the host could otherwise have handed back to
the holder). Mechanism 0 wins specifically **because** its HLT yields the
pCPU. This is the tension Task 1 investigates.

---

## 2. Task 1 — can genuine host-level pCPU yielding be achieved without HLT or an explicit hypercall?

### 2.1 The non-negotiable framing

"Yield the pCPU to the host" is, by definition, a **host scheduling
decision**. The host's scheduler (ordinary Linux CFS running the vCPU as a
normal thread, for KVM) can only act on information that actually reaches it.
So the real question is not "can we avoid all guest→host communication" (we
categorically cannot — that would mean the host acts on nothing, i.e. no
yielding happens) but **"is there a channel cheaper, more indirect, or less
overtly cooperative than a full HLT vmexit or an explicit hypercall?"**
Every avenue below is evaluated on exactly that question, against the real
KVM/x86 source in this tree (`arch/x86/kvm/`, `arch/x86/kernel/kvm.c`), not
against generic virtualization folklore.

### 2.2 Shared-memory / polled signaling (steal-time page, PV_EOI, APF)

**Checked directly in `arch/x86/kvm/x86.c`, ruled out.** The KVM steal-time
page (`struct kvm_steal_time`, `record_steal_time()` at `x86.c:3674`, and its
counterpart `kvm_steal_time_set_preempted()` at `x86.c:5079`) is written by
the **host**, and only at two specific points: `record_steal_time()` runs
when the vCPU is about to be **run** (schedule-in, inside
`vcpu_enter_guest()`), and `kvm_steal_time_set_preempted()` runs when it is
about to be **descheduled** (schedule-out, `vcpu_put()`). The host does not
proactively poll this page, or any other guest memory, "at low cost" at any
other time — there is no background thread or timer sweeping guest memory for
hints. The only place the host reads *anything back* from that page at all is
the `KVM_FEATURE_PV_TLB_FLUSH` branch (`x86.c:3712-3736`), which reads back
the `preempted` byte the host itself wrote earlier, purely to decide whether
it owes the guest a TLB flush-on-resume — not a general "read a guest hint"
facility, and it only fires at the same schedule-in boundary as everything
else.

This means the timing is exactly backwards from what a useful hint page would
need: the moment a waiter would want to write "please deprioritize me," it is
*already running* (that's the problem — it's occupying the pCPU the holder
needs). The host has no scheduled reason to look at guest memory *while the
guest is running and not otherwise trapping*, so a plain memory write, with no
accompanying trap, is invisible to the host scheduler for the entire duration
it would need to matter. **A guest-written hint page is only useful if the
host is made to read it on some trigger — and any such trigger is either an
existing vmexit (piggybacking, see below) or a new one, which reduces to "a
new, cheaper vmexit," not "no vmexit."** PV_EOI and the async-page-fault
(`apf_reason`) mechanisms have the identical shape (host writes at a specific
point, or the exchange is gated behind an interrupt/vmexit) — neither offers
an idle-hint channel either.

**The one real, honest partial idea this raises**: a *new* PV feature where
the host's own scheduler tick (which already fires on every physical CPU
roughly every 1-4 ms via the host's own timer, entirely independent of any
guest vmexit) opportunistically reads a guest-written "I have nothing useful
to do" bit as part of its ordinary per-thread accounting, and factors it into
whether to let this vCPU's thread keep its timeslice. This would be
genuinely novel and would not require a guest vmexit *at the moment of
signaling* (only an ordinary store to memory). But it requires **host
kernel/KVM code that does not exist today** — this is real host-side
implementation work, not a guest-only kernel change, and it does not exist
anywhere in this tree or (to the author's knowledge) in upstream KVM. It also
only shifts *when* the cooperation happens (host polls on its own tick
instead of the guest triggering a vmexit) — it is still cooperation, just
cooperation the host provides "for free" as part of infrastructure it already
runs, which is a legitimately different (and arguably more honest to call
"non-explicit") shape than a hypercall, but it is **not buildable from the
guest side alone**, which is the constraint this project has set for itself.
Flagging it as the closest thing to "a way out," while being clear it's
future host work, not a guest kernel patch.

### 2.3 Existing, underused KVM/PV features

Full feature-bit list in this tree, `arch/x86/include/uapi/asm/kvm_para.h`:
`CLOCKSOURCE(2)`, `NOP_IO_DELAY`, `MMU_OP`, `ASYNC_PF`, `STEAL_TIME`,
`PV_EOI`, `PV_UNHALT`, `PV_TLB_FLUSH`, `ASYNC_PF_VMEXIT`, `PV_SEND_IPI`,
`POLL_CONTROL`, `PV_SCHED_YIELD`, `ASYNC_PF_INT`, `MSI_EXT_DEST_ID`,
`HC_MAP_GPA_RANGE`, `MIGRATION_CONTROL`. Of the ones not already discussed:

- **`KVM_FEATURE_POLL_CONTROL`** (`arch_haltpoll_enable/disable`, already
  wired up in this exact `kvm.c`, lines 1411-1445): this controls the
  **host's own halt-polling behavior** (`MSR_KVM_POLL_CONTROL`) — whether the
  host, after a guest HLT vmexit, busy-polls briefly before actually
  descheduling the vCPU thread, trading host CPU for lower guest wake
  latency. It is real, it is cheap to toggle, and it is the closest existing
  feature to "communicate something about halting more cheaply" — but it
  **tunes the cost of HLT's round trip, it does not replace the need for
  HLT.** You still pay the vmexit; this only affects what the host does
  after receiving it. Not a way around HLT.
- **`KVM_FEATURE_PV_SCHED_YIELD`** / **`KVM_HC_SCHED_YIELD`**
  (`arch/x86/kvm/x86.c:9898` `kvm_sched_yield()`, hypercall handler at
  `x86.c:9998`): a **directed yield** — "let this specific other vCPU run
  ahead of me." This kernel already has working code that calls exactly this
  hypercall, just for a different purpose:
  `kvm_smp_send_call_func_ipi()` (`arch/x86/kernel/kvm.c:655-668`) issues
  `KVM_HC_SCHED_YIELD` when a call-function IPI target is
  `vcpu_is_preempted()`. This is the nearest already-available building block
  in this exact tree if the "no explicit hypercall" constraint is ever
  relaxed to "at most one, minimal hypercall" — but it is, definitionally,
  an explicit hypercall, so it's ruled out by the user's own stated
  constraint. Named here only because it's the cheapest, most
  purpose-built such call that already exists and is already used elsewhere
  in this file.
- **Nothing else in the list offers an idle/yield channel.** PV_TLB_FLUSH,
  MSI_EXT_DEST_ID, MIGRATION_CONTROL, HC_MAP_GPA_RANGE are unrelated to
  scheduling.

### 2.4 MWAIT/MONITOR family beyond TPAUSE — checked against real VMX exit-control code, ruled out cleanly

This is the most concrete, source-verifiable finding in this section, and it
gives a clean architectural answer rather than a guess.

- **Ring-0 `MONITOR`/`MWAIT`** (the pre-WAITPKG, privileged instructions,
  distinct from `UMONITOR`/`UMWAIT`/`TPAUSE`) **do have a VMX interception
  control**: `CPU_BASED_MWAIT_EXITING` / `CPU_BASED_MONITOR_EXITING`
  (`arch/x86/include/asm/vmx.h:32,47`). Checked this exact KVM's default
  behavior in `arch/x86/kvm/vmx/vmx.c:4407-4409`
  (`vmx_exec_control()`): these exiting bits are **only cleared** (i.e. MWAIT
  allowed to run natively) if `kvm_mwait_in_guest()` is true, which requires
  the VM to have been created with `KVM_CAP_X86_DISABLE_EXITS` for MWAIT —
  **not the default.** So by default, ring-0 MWAIT/MONITOR *do* trap.
- **But the trap handler is a dead end.** `kvm_emulate_mwait()`/
  `kvm_emulate_monitor()` (`arch/x86/kvm/x86.c:2097-2126`,
  `kvm_emulate_monitor_mwait()`) do **not** treat the trap as an idle signal
  at all — by default they **emulate the instruction as a NOP**
  (`kvm_emulate_as_nop()`, with a `pr_warn_once` the first time). No directed
  yield, no scheduling hint, nothing. Worse: because it's a NOP, a real
  MWAIT-based wait loop using this path would return **immediately** every
  time (zero-duration "wait"), so the guest would spin through its poll loop
  even harder than a plain PAUSE loop, for the *cost* of a full vmexit every
  single call. This is strictly worse than TPAUSE (no vmexit, correct wait
  semantics) and worse than HLT (real yield). **Concretely ruled out, not
  just unattractive.** This also explains, independently of this project, why
  `pv_tlb_flush_supported()`/`pv_sched_yield_supported()`
  (`arch/x86/kernel/kvm.c:497-519`) both gate on
  `!boot_cpu_has(X86_FEATURE_MWAIT)` — this kernel already assumes MWAIT is
  typically unavailable to cloud KVM guests, consistent with what was just
  found.
- **`UMONITOR`/`UMWAIT`/`TPAUSE` (WAITPKG) have no VMX interception control
  at all.** There is no `CPU_BASED_*` or secondary-exec-control bit for these
  instructions anywhere in `arch/x86/include/asm/vmx.h` or handled in
  `arch/x86/kvm/vmx/vmx.c`'s exit-reason table. This is not an oversight —
  it's the ISA's design intent: WAITPKG was specifically designed as a
  **ring-3-usable, non-interceptable** lightweight wait, precisely so
  ordinary user-mode spin-loops get a cheap nap without needing kernel or
  hypervisor involvement. **No host-side VMX configuration, opt-in or
  otherwise, can make TPAUSE trap.** This is confirmed by, and gives the
  architectural reason for, this session's own live measurement (TPAUSE at
  37 ns, no vmexit) in the verification doc — it isn't a fluke of this
  particular kernel or host, it is baked into the instruction's definition.
- **Honesty caveat on HLT itself**: `kvm_hlt_in_guest()`
  (`vmx.c:4410-4411`) shows HLT-exiting is *also* host-togglable
  (`KVM_CAP_X86_DISABLE_EXITS` with the HLT flag) — "HLT always vmexits" is
  the default and the case `kvm_spinlock_init()` already checks for
  (`KVM_HINTS_REALTIME` routes to native qspinlock specifically because
  dedicated pCPUs mean no LHP to mitigate), but it is not an absolute
  architectural universal the way "TPAUSE never traps" is — a host *could*,
  in principle, configure passthrough HLT too, in which case mechanism 2
  degrades exactly like mechanism 1 (no vmexit, no yield). Worth stating for
  intellectual honesty even though it doesn't change the practical
  recommendation.

**Verdict on this sub-question: dead end, and a clean architectural one.**
Ring-0 MWAIT traps but is emulated uselessly; WAITPKG cannot be made to trap
by any host configuration. There is no "cheaper MWAIT-family vmexit" hiding
here.

### 2.5 PLE (Pause-Loop Exiting) — the closest real candidate, and why it still doesn't give the guest a new lever

This is the one genuinely interesting, previously-undocumented-in-this-project
finding from this pass, so it's given full treatment.

**What it is, confirmed in `arch/x86/kvm/vmx/vmx.c`**: KVM already has a
built-in mechanism for exactly this problem, for exactly the case of a vCPU
spinning on `PAUSE` in a tight loop — Pause-Loop Exiting. `ple_gap`
(default 128) and `ple_window` (default 4096, `arch/x86/kvm/x86.h:96-100`)
are VMX execution-control fields (`vmcs_write32(PLE_GAP,...)`,
`vmx.c:4658-4662`), active **by default** unless the host explicitly disabled
it (`!kvm_pause_in_guest(kvm)`). When the guest executes enough consecutive
`PAUSE` instructions without other work to exceed `ple_window`, the CPU traps
(`EXIT_REASON_PAUSE_INSTRUCTION` → `handle_pause()`, `vmx.c:5827-5840`), and
the handler calls **`kvm_vcpu_on_spin()`** (`virt/kvm/kvm_main.c:3956-4030`)
— which does a **directed yield to another vCPU in the same VM that is
`preempted`** (host-descheduled), i.e. the host's own best-effort attempt at
exactly "find the vCPU that might be holding the lock this spinner wants, and
give it a scheduling boost." This is a **real, host-native, no-guest-code,
no-explicit-hypercall mechanism that already does directed yielding on plain
`PAUSE` spins.**

**Why this doesn't give IVH a usable new lever, though, for concrete reasons:**

1. **It's invisible and uncontrollable from the guest.** There is no CPUID
   bit, MSR, or any other guest-visible indicator of whether PLE is active,
   what `ple_gap`/`ple_window` are set to, or whether the host has disabled
   it. This session's guest has no `kvm_intel` module loaded (it isn't
   itself a nested hypervisor), so there is no way to check even indirectly
   whether the *actual* host in this environment has PLE on. This must be
   flagged as **unverified** — the analysis below assumes typical (default)
   KVM host configuration, not something confirmed for the real host behind
   this VM.
2. **It doesn't apply to the mechanism this project modifies.** PLE counts
   `PAUSE` instructions specifically; `ivh_pv_backoff()`'s TPAUSE nap (used
   by mechanism 1) is a different instruction entirely and does not
   increment PLE's counter — this is the same architectural fact as §2.4,
   from a different angle. So PLE cannot rescue mechanism 1 as designed.
3. **It likely already fires, for free, in code paths that predate and are
   untouched by this project** — worth naming precisely: `SPIN_THRESHOLD`
   (`arch/x86/include/asm/spinlock.h:25`) is `1 << 15` = 32768. Both
   `pv_wait_node()` and `pv_wait_head_or_lock()`
   (`kernel/locking/qspinlock_paravirt.h:323,486`) run a bare
   `cpu_relax()` (a real `PAUSE`, ~50 cycles per the session's own
   `tpause_cost.c` measurement) in a loop up to that many iterations *before*
   ever calling `ivh_pv_wait()` — on the order of 1.6M cycles of real PAUSE
   spinning in the worst case, **far more** than the default `ple_window` of
   4096. If PLE is active on the real host, this pre-existing hot-spin phase
   (present in **all three** mechanisms identically, since it's shared,
   unmodified upstream code) plausibly already gets PLE-trapped and
   directed-yielded, independent of `ivh_pv_wait_mechanism`'s value entirely.
   **This is a shared substrate across mechanisms 0/1/2, not a differentiator
   between them** — it doesn't explain the measured 36% regression (which is
   specifically about what happens *after* this shared hot-spin phase, inside
   `ivh_pv_wait()` itself), but it's worth knowing it's there, because it
   means some of the "genuine yielding" the project associates with HLT may
   already be happening, unannounced, via PLE, upstream of any of this
   project's own code.
4. **Even if confirmed active, it's not a lever IVH can pull.** PLE is a
   host module parameter and VMX feature; nothing in the guest kernel can
   force a shorter window, verify it fired, or rely on it as a designed
   mechanism rather than an opportunistic side effect. A design that
   *depends* on PLE would be depending on an invisible, host-configured,
   possibly-absent feature — not something a paper can claim as "the guest
   achieved yielding," since the guest did nothing to cause it (it's exactly
   as "cooperative" as HLT, just silently so).

**Net honest read**: PLE is real, and it is the single closest thing in this
whole investigation to "a vmexit cheaper/more indirect than HLT that KVM
already handles specially" — but it's a *host* mechanism operating on plain
`PAUSE`, not something the guest can invoke, verify, or design around, and it
doesn't reach the specific TPAUSE-based code this project's mechanism 1 uses.
It's worth a line in the paper as "there is a pre-existing host mechanism
(PLE) that may already provide some of this for free, upstream of any code
this project controls" — but it is not a candidate design to build.

### 2.6 "Give up less than a full core" — the guest-side reframing

This changes the goal from "signal the host" to "don't waste the vCPU's own
capacity even without telling the host anything." Worth engaging with on its
own terms, and it turns out to have a sharp, structural answer specific to
*where* the waiting happens:

**Not compatible with the actual code this project modifies.** `ivh_pv_wait()`
is called from deep inside the kernel's own qspinlock slowpath — a context
used, among many other places, from inside other locks and interrupt-disabled
regions, where the calling thread fundamentally cannot be descheduled to let
"other guest work" run in its place without changing what a spinlock *is*.
The primitive that already does exactly "give up the CPU and let other guest
work run while waiting" is a **mutex**, not a spinlock: contended mutexes
call `schedule()`. This project's own memory (see `feedback_workload_selection.md`
in the user's memory index: "mutex does NOT produce `lock_depth > 0` at tick
time") already independently established that mutex-based contention doesn't
reproduce the condition IVH exists to detect and fix. So applying this
reframing *to the PV-spinlock wait/kick hook* would mean turning the very
primitive under study into the primitive this project already tried and
found doesn't exhibit the target problem — not a workaround, a
category change.

**It does have a real, different home in this exact project, though.** The
*other* half of IVH — pre-lock migration (§0 above) — is precisely this
reframing, already built: a userspace thread about to take a lock is migrated
to a different vCPU with spare capacity *before* it ever waits, using
information already available to the guest scheduler, no host communication
involved. That's "make sure the vCPU's capacity isn't wasted" applied at the
level the guest *can* act on without needing to interrupt an in-progress
kernel spinlock wait. So: **yes, legitimate reframing, already substantially
built — just in the other half of the project, not applicable to the
specific PV-wait/kick hook this dispatch's Task 1 is about.**

### 2.7 Rethinking "kick" (already done) and whether a cheaper-than-HLT trap exists

Mechanism 2 already answers "can the *kick* avoid the hypercall" — yes, a
plain `smp_send_reschedule()` IPI, verified against real KVM/APIC semantics
in the mechanism-2 implementation doc. The remaining question specific to
this dispatch is whether some *other* instruction, cheaper than HLT, causes a
vmexit KVM already handles specially as an idle/yield signal. §2.4 and §2.5
are the exhaustive answer: ring-0 MWAIT traps but is uselessly emulated;
WAITPKG cannot trap at all; PLE exists but operates on plain `PAUSE`, is
invisible to the guest, and isn't something the guest can invoke on demand.
There is no fourth option found in this pass.

### 2.8 Verdict and recommendation for Task 1

**Brutal-honesty verdict: there is no clean way to achieve genuine,
guest-controllable, host-visible pCPU yielding without HLT (or an equally
explicit hypercall/directed-yield-style vmexit).** This is not "we didn't
look hard enough" — every concrete avenue the prompt asked to investigate
converges on the same architectural reason: **any instruction the guest can
execute without needing kernel/hypervisor cooperation (WAITPKG's UMWAIT/TPAUSE)
is, by the same design property, structurally invisible to the host** — that
invisibility is the *entire feature*, not an omission. The only instructions
that reach the host at all either (a) trap but are handled uselessly for this
purpose by default (ring-0 MWAIT), (b) already are host-cooperative and are
what mechanisms 0/2 already use (HLT), or (c) are explicit hypercalls (ruled
out by the stated constraint). PLE is the one real "vmexit KVM already
handles specially" that isn't HLT or a hypercall, but it operates on a
different instruction (`PAUSE`), is invisible to and uncontrollable by the
guest, and isn't something this project's code can invoke or depend on.

**Recommendation**: Treat this as a closed, negative result for the paper,
stated exactly this way rather than softened. The paper's honest framing
should be: *"we searched for a guest-only substitute for HLT's genuine yield
and did not find one; the closest pre-existing host mechanism (PLE) is
invisible to guest code and doesn't intercept the specific low-level wait
instructions this project's own non-cooperative design already uses, which is
itself informative — it suggests the property that makes an instruction cheap
enough for a guest to use freely (no trap) is the same property that makes it
useless for host-visible yielding."* Mechanism 2 (real HLT + plain IPI) is the
right place to stop pushing on "avoid HLT," and instead demonstrates the
narrower, still real claim: hypercall-less wake is achievable and costs
~nothing extra even on a PV_UNHALT-capable host (§6, mechanism-2 doc), and
would be the mechanism's real value on a host that doesn't advertise
PV_UNHALT (busy-spin fallback today) — that's the paper claim to make, not
"we eliminated host cooperation," which the evidence doesn't support.

---

## 3. Task 2 — TSC replacing `steal_account_process_time()`'s derivation of `last_preemption`/`last_idle`

### 3.1 What it currently does (read directly, `kernel/sched/cputime.c:256-286`)

```c
static __always_inline u64 steal_account_process_time(u64 maxtime)
{
#ifdef CONFIG_PARAVIRT
    if (static_key_false(&paravirt_steal_enabled)) {
        u64 steal;
        struct rq *rq = this_rq();

        steal = paravirt_steal_clock(smp_processor_id());   /* exact ns, from KVM's kvm_steal_time page */
        steal -= rq->prev_steal_time;
        steal = min(steal, maxtime);
        account_steal_time(steal);                          /* feeds /proc/stat's "steal" field */
        rq->prev_steal_time += steal;
        if (steal > 0) {
            u64 now = sched_clock();
            if (steal > 1000000) {                          /* only "big" (>1ms) steals update these */
                if (rq->last_preemption > rq->last_idle_tp)
                    rq->last_active_time = now - rq->last_preemption - steal;
                else
                    rq->last_active_time = now - rq->last_idle_tp - steal;
                rq->last_preemption = now;
            }
            rq->preemptions += 1;
            if (rq->max_latency < steal)
                rq->max_latency = steal;
        }
        return steal;
    }
#endif
    return 0;
}
```

Called from `account_process_tick()`/`account_other_time()`/related
irqtime-accounting call sites (`cputime.c:305,413,514,543,695`) — i.e. on
**tick granularity**, roughly once per jiffy per CPU (plus extra calls on
IRQ/softirq accounting boundaries). `paravirt_steal_clock()` reads
`kvm_steal_clock()` (`arch/x86/kernel/kvm.c:411-426`), a seqlock-protected
read of the host-written `kvm_steal_time` page — an **exact, hypervisor-
reported nanosecond figure**, not an inference.

Downstream consumers (grepped across `kernel/sched/`):
- `rq->last_preemption`, `rq->last_idle_tp`, `rq->last_active_time` feed
  `ivh_steal_imminent()`/`ivh_rq_capacity_and_timeleft_ok()`
  (`kernel/sched/fair.c:13207-13301`) — IVH's Gate 1+2 "is this vCPU in
  danger" check, compared against `ivh_time_left_threshold_ns` (default
  **4,000,000 ns = 4 ms**, `kernel/sched/bpf_sched.c:37`).
- `rq->clock_preempt` (set separately, `cputime.c:503`, inside
  `account_process_tick()` unconditionally on every tick — **not** gated on
  `steal>0`) feeds `is_cpu_preempted()` (`cputime.c:288-294`, threshold
  1.5 ms), consumed by the rescue/migration-target-health logic in
  `fair.c:214-215,13506,13541-13542`.

### 3.2 Concrete TSC-heuristic code sketch

The comment already on `steal_account_process_time()`
("missed clock ticks are not redelivered later... may on occasion account
more time than the calling functions think elapsed") is itself an admission
that this function is already, in effect, inferring "delay" from tick timing
irregularity in some cases — the TSC approach makes that inference the
*primary* signal instead of a side effect.

```c
/* New per-rq state, kernel/sched/sched.h, alongside last_preemption etc. */
u64 prev_tick_tsc;   /* rdtsc() at the last call, 0 == "not yet initialized" */
u64 prev_tick_ns;    /* sched_clock() at the last call, paired with the above */

/*
 * tsc_steal_heuristic() - TSC-only substitute for steal_account_process_time().
 * No CONFIG_PARAVIRT, no paravirt_steal_clock(), no hypervisor cooperation
 * required. Infers "this tick took longer than it should have" instead of
 * reading an exact hypervisor-reported figure.
 */
static __always_inline u64 tsc_steal_heuristic(u64 maxtime)
{
    struct rq *rq = this_rq();
    u64 now_ns = sched_clock();
    s64 excess_ns;
    u64 steal = 0;

    if (!rq->prev_tick_tsc) {
        rq->prev_tick_tsc = rdtsc();
        rq->prev_tick_ns  = now_ns;
        return 0;
    }

    /*
     * Expected elapsed ns for one call of this function, with slack for
     * ordinary scheduling/IRQ jitter that is NOT host steal (this is the
     * new, uncalibrated tunable -- see 3.3). ivh_tsc_tick_slack_pct is a
     * new sysctl, same shape as every other ivh_* tunable in bpf_sched.c.
     */
    u64 expected_ns = (NSEC_PER_SEC / HZ) * READ_ONCE(ivh_tsc_tick_slack_pct) / 100;
    excess_ns = (s64)(now_ns - rq->prev_tick_ns) - (s64)expected_ns;

    if (excess_ns > 0) {
        steal = min_t(u64, (u64)excess_ns, maxtime);
        account_steal_time(steal);          /* same downstream /proc/stat field */
        if (steal > 1000000) {
            if (rq->last_preemption > rq->last_idle_tp)
                rq->last_active_time = now_ns - rq->last_preemption - steal;
            else
                rq->last_active_time = now_ns - rq->last_idle_tp - steal;
            rq->last_preemption = now_ns;   /* identical shape to the original */
        }
        rq->preemptions += 1;
        if (rq->max_latency < steal)
            rq->max_latency = steal;
    }

    rq->prev_tick_tsc = rdtsc();
    rq->prev_tick_ns  = now_ns;
    return steal;
}
```

This is a **drop-in replacement at the same call sites**, preserving every
downstream field's shape (`rq->last_preemption`, `rq->last_active_time`,
`rq->preemptions`, `rq->max_latency` all still get written the same way) —
only the *source* of the "was there a steal-like delay" signal changes, from
an exact hypervisor counter to an inferred gap between expected and observed
tick spacing. Note `rdtsc()` here is used only as *a* free-running clock to
timestamp calls cheaply — `sched_clock()` (already read unconditionally
either way) could serve the same comparison; the reason to prefer `rdtsc()` is
avoiding `sched_clock()`'s own `kvm-clock`/pvclock read where that path is
non-trivial, not because TSC carries different information (see the
identical point made concretely for Task 3, §4.2 below — TSC deltas and
wall-clock deltas measure the *same* real elapsed time on this kind of guest,
they are not independent signals).

### 3.3 Honest cost of the ambiguity, quantified against this specific use

**What's lost, stated plainly**: steal-time gives an *exact* figure, sourced
from the host's own scheduler, with a clean semantic ("the host preempted
this vCPU for exactly N ns"). A TSC/tick-gap heuristic cannot distinguish
that from: a long-running interrupt or softirq, an SMI (invisible to the OS
entirely — genuinely unobservable by any in-guest mechanism, TSC or
otherwise), a delayed timer tick under `NOHZ`, or simply this being the first
call after a real idle period (`prev_tick_ns` stale). All of these would
misfire as "steal."

**Does this matter for *this* use, though — not in the abstract?** Two facts
narrow the bar considerably:
1. The existing code *itself* only treats steals **> 1 ms** as significant
   enough to update `last_preemption`/`last_active_time` (`cputime.c:270`)
   — sub-millisecond noise already doesn't move the signal IVH's gates
   actually consume, in either the current or a TSC-based version.
2. `ivh_time_left_threshold_ns` — the tolerance built into the actual
   migration-timing decision this signal feeds — defaults to **4 ms**
   (`bpf_sched.c:37`), an order of magnitude more slop than the 1 ms floor
   above. `ivh_pv_wait()` itself (mechanism 1/2's own deadline,
   `IVH_PV_ADAPTIVE_TSC`) already trusts raw `rdtsc()` deltas for
   sub-millisecond decisions in this exact tree, with no CPUID-asserted TSC
   invariance — real, working precedent that short-window TSC trust is
   already acceptable here.

So the real question isn't "can TSC replace an exact ns counter" (no, not
exactly) but "can a heuristic accurate to within a few hundred µs to ~1 ms
reliably flag events that matter at a 4 ms decision granularity" — which is a
much more answerable, much less demanding bar, and plausibly yes, **but
untested**: false-positive/negative rates have not been measured here (this
would need the live A/B `vcap`-vs-heuristic comparison the six-goals report's
§6.3 also recommends as the concrete next step, not a static-analysis
answer).

### 3.4 Recommendation

Buildable, worth doing **as a userspace or debug-instrumented prototype
first** (a debug sysctl that computes both the real steal figure and the TSC
heuristic side by side, logging disagreement, without changing IVH's actual
decision path yet) before committing the kernel to depending on it. Do not
skip straight to replacing `steal_account_process_time()`'s production
behavior — the failure mode (a false "steal" during ordinary IRQ/softirq
load, causing a spurious migration or backoff) is exactly the kind of thing
this project's own EWMA-tuning history (`ivh_hot_preempt_ewma_k_rise/_fall`,
per project memory) shows takes real measurement to get right, not a one-shot
guess.

---

## 4. Task 3 — TSC replacing how `vcap` derives capacity

### 4.1 What `vcap` currently does, confirmed directly in `main.cpp`

`vcap` (`/home/nick/vsched_main/vcapacity/main.cpp`, 882 lines) is a
userspace daemon with one worker pthread pinned per vCPU
(`stick_this_thread_to_core()`, `setup_threads()`). Every `profile_time` ms
(`do_profile()`, line 656):

1. `get_cpu_information()` (line 211) reads **`/proc/vcap_info`** (an
   out-of-tree kernel module's export, per-CPU `preempts`/`steal_time`/
   `max_latency` — sourced from exactly the `rq->preemptions`/
   `paravirt_steal_clock()`-derived/`rq->max_latency` fields
   `steal_account_process_time()` populates, confirmed in §3.1) and
   **`/proc/stat`**'s per-CPU user+nice+system jiffies.
2. `get_finalized_data()` (line 349) computes, line 368-370:
   ```cpp
   double capacity_perc_1 = (double)(used_time) / (used_time + stolen_pass);
   ```
   where `stolen_pass = data_end[i].steal_time - data_begin[i].steal_time`
   (line 363) — **steal-time is the entire numerator/denominator input**,
   confirmed directly, not secondhand.
3. Separately, on a slower "heavy profiling" cadence
   (`heavy_profile_interval`, adaptively grown when stable), each worker
   thread measures `CLOCK_THREAD_CPUTIME_ID` vs. `CLOCK_MONOTONIC_RAW`
   wall time around its busy loop (`run_computation()`, lines 837-882) to
   compute `perf_use = threadcputime / walltime`, then
   `capacity_adj = (1/perf_use) * raw_compute` (lines 394-400) — a
   **second, independent capacity signal that already does not use
   steal-time at all.**

### 4.2 The key correction: `rdtsc()` ≈ wall-clock elapsed time on this guest, not an independent signal

This matters enough to state precisely, because it changes what a "TSC-based"
Task 3 design should actually look like. KVM virtualizes the guest TSC so
that it (via TSC scaling/offsetting) continues to track **real elapsed
physical time**, including through periods the vCPU is descheduled — the
guest's `rdtsc()` does *not* freeze or skip during host steal; it advances
just like any other real-time clock would. This means a plain
`rdtsc_end - rdtsc_start` measurement across a busy-loop window gives you
**the same information as any other elapsed-wall-time measurement**
(`CLOCK_MONOTONIC`, `chrono::high_resolution_clock`, already used
extensively in `vcap` today) — not something categorically different. TSC's
practical advantage over those is being **cheaper to read** (no syscall, no
vDSO seqlock-protected pvclock retry loop the way `kvm-clock`-backed
`CLOCK_MONOTONIC` requires on this guest) and **not requiring a hypervisor
clock source at all** — not "measuring something wall-clock can't."

**The useful comparison for detecting stolen/wasted capacity is therefore
not "TSC vs wall-clock" (near-equivalent), but "elapsed real time vs. actual
useful work completed"** — and `vcap` **already has exactly this
comparison implemented**, today, in the `capacity_adj`/`perf_use` heavy-
profiling branch (§4.1, point 3): `CLOCK_THREAD_CPUTIME_ID` measures actual
scheduled-on-CPU time for the pinned worker thread, which is precisely zero
during any window the thread didn't get to run — regardless of whether the
cause was host steal or in-guest contention. This is functionally a
"TSC-free TSC heuristic" already running in this exact file.

### 4.3 Concrete next steps, two versions

**(a) Cheapest, smallest change — promote the existing mechanism, don't
invent a new one.** `capacity_adj` is currently computed only on the
infrequent "heavy" cadence (`heavy_profile_interval`, grown up to 1.6x when
stable, `get_finalized_data()` lines 430-447) specifically because the
thread-priority-boost/wait-for-workers synchronization it requires
(`move_thread_to_high_prio()`, `wait_for_workers()`) has real overhead. The
concrete, buildable step: measure how much that overhead actually costs, and
whether `capacity_adj` (or a cheaper variant of the same idea) could run
every interval and **replace** `capacity_perc`'s steal-time dependency
outright, rather than serve as a periodic cross-check. This requires no new
TSC code at all — it's already TSC-adjacent (elapsed-vs-consumed-time ratio)
and already exists.

**(b) A literal `rdtsc()` version, if wanted for the paper's specific
framing** (e.g. to make a portability argument independent of
`CLOCK_THREAD_CPUTIME_ID`'s own scheduler-accounting machinery):

```cpp
/* Once at startup, same pattern as NHextend3.c's calibrate_tsc()
 * (NHextend3.c:125-141) -- this project already has this exact code. */
static double tsc_per_ns = 3.0; /* recalibrated below */
static void calibrate_tsc_per_ns() {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t c0 = __rdtsc();
    struct timespec sleep_ts = {0, 20000000}; /* 20ms */
    nanosleep(&sleep_ts, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t c1 = __rdtsc();
    uint64_t wall_ns = (t1.tv_sec - t0.tv_sec) * 1000000000ULL + (t1.tv_nsec - t0.tv_nsec);
    if (wall_ns) tsc_per_ns = (double)(c1 - c0) / (double)wall_ns;
}

/* Per profiling round, inside run_computation()'s existing busy-loop: */
uint64_t tsc_start = __rdtsc();
/* ... existing addition_calculator busy-loop, unchanged ... */
uint64_t tsc_end = __rdtsc();
double elapsed_ns = (tsc_end - tsc_start) / tsc_per_ns;

/* "Expected" iterations/ns established once, during a known-quiet baseline
 * period (analogous to raw_compute's role today) -- this is the SAME
 * calibration-burden problem as Task 2's slack multiplier, not a smaller one. */
double expected_iters = expected_iters_per_ns * elapsed_ns;
double capacity_perc_tsc = (double)addition_calculator / expected_iters;
```

This is real code, but it is **not obviously better than (a)** — it
introduces the same "establish and continuously revalidate an
expected-work baseline" calibration problem as Task 2's slack multiplier,
for a signal that (per §4.2) doesn't measure anything wall-clock-based
approaches don't already measure on this guest. Its actual distinguishing
value is **portability** (works without `kvm-clock`/paravirt steal-time
support at all) — consistent with, not a correction to, the six-goals
report's own §6.4 conclusion that this "moves the complexity, doesn't
remove it," and that portability (not simplification) is the honest
motivation if pursued.

### 4.4 `constant_tsc`/`nonstop_tsc` — re-verified directly on this VM, still a real obstacle

Re-checked this session, not assumed from the prior investigation:

```
$ grep -m1 flags /proc/cpuinfo | tr ' ' '\n' | grep -i tsc
tsc
rdtscp
tsc_known_freq
tsc_deadline_timer
tsc_adjust
```

**`constant_tsc` and `nonstop_tsc` are absent.** Confirmed, not inherited
secondhand. `current_clocksource` is `kvm-clock`, not `tsc` — this guest
kernel does not trust its own TSC as invariant and uses the paravirt clock
instead. The host CPU (`INTEL(R) XEON(R) GOLD 6554S`, a modern
Sapphire-Rapids-generation server part) almost certainly has real invariant
TSC hardware — but **that cannot be confirmed from inside this guest**,
since the hypervisor controls what CPUID bits are exposed, and this one
doesn't expose the invariance guarantee. This is a genuine, unresolved gap:
a userspace `calibrate_tsc()`-style one-time calibration (as NHextend3
already does) is a reasonable practical workaround for a short profiling
window, but does not carry the kernel's own architectural guarantee that the
TSC rate won't drift with P-state/C-state transitions over a long-running
daemon's lifetime — and there is no way, from this guest, to tell whether
that risk is real (genuinely non-invariant hardware) or just conservative
CPUID exposure (invariant hardware, bit not passed through). Flagging this
as open, exactly as the prompt asked, rather than routing around it.

---

## 5. Summary table

| Task | Verdict | Confidence | Concrete next step if pursued |
|---|---|---|---|
| 1: dodge HLT/hypercall for real yielding | **No clean solution exists.** WAITPKG is architecturally non-interceptable (by design); ring-0 MWAIT traps but is emulated as a useless NOP; PLE is real but invisible/uncontrollable from the guest and doesn't intercept TPAUSE; a host-side "idle hint page" is the only genuinely novel idea and requires host kernel work this project can't do from the guest alone. | High — grounded in reading the actual VMX exec-control code and KVM exit handlers, not inference. | Stop searching for a guest-only substitute; ship mechanism 2 as "hypercall-less wake, same real yield as mechanism 0," and frame the PLE finding as a footnote about pre-existing host behavior, not a design to build on. |
| 2: TSC replacing `steal_account_process_time()` | Plausible for this specific use (4 ms decision tolerance, already-precedented short-window TSC trust in this tree), not a free substitute (loses exact-ns ground truth, gains a new uncalibrated slack tunable). | Medium — the code sketch is real and drop-in-shaped; the false-positive rate is genuinely unmeasured. | Build a side-by-side debug comparator (heuristic vs. real steal-time) before touching production decision logic. |
| 3: TSC replacing `vcap`'s capacity derivation | `vcap` already has a TSC-*equivalent* mechanism (`capacity_adj`/`perf_use`, `CLOCK_THREAD_CPUTIME_ID`-based) running today, just on a slower cadence — that, not a new literal-`rdtsc()` rewrite, is the fastest real path. A literal-TSC version is buildable but not obviously better, and this VM's confirmed lack of `constant_tsc`/`nonstop_tsc` is a real, unresolved risk for any long-running literal-TSC daemon. | Medium-high — `main.cpp` was read directly for this, not assumed. | Measure the actual overhead of promoting `capacity_adj` to every interval; only build a literal-`rdtsc()` version if the motivation is specifically portability off paravirt steal-time. |

## 6. Appendix — key file/line references used in this dispatch

- `arch/x86/kernel/kvm.c:1056-1409` — `ivh_pv_wait()`, `ivh_pv_backoff()`,
  `ivh_pv_kick()`, `kvm_spinlock_init()`, `ivh_pv_wait_mechanism`.
- `arch/x86/kernel/kvm.c:497-519,655-668` — `pv_tlb_flush_supported()`,
  `pv_sched_yield_supported()`, `kvm_smp_send_call_func_ipi()`
  (existing `KVM_HC_SCHED_YIELD` usage).
- `kernel/locking/qspinlock_paravirt.h:263-373,382-445` —
  `pv_wait_early()`, `pv_wait_node()`, `pv_kick_node()`.
- `arch/x86/include/asm/spinlock.h:25` — `SPIN_THRESHOLD (1 << 15)`.
- `arch/x86/include/asm/vmx.h:32,47,48` — `CPU_BASED_MWAIT_EXITING`,
  `CPU_BASED_MONITOR_EXITING`, `CPU_BASED_PAUSE_EXITING`.
- `arch/x86/kvm/vmx/vmx.c:4370-4413` — `vmx_exec_control()`
  (`kvm_mwait_in_guest()`/`kvm_hlt_in_guest()` gating).
- `arch/x86/kvm/vmx/vmx.c:181-197,4658-4662,5827-5840` — PLE module
  params, `handle_pause()`.
- `virt/kvm/kvm_main.c:3956-4030` — `kvm_vcpu_on_spin()` (directed yield).
- `arch/x86/kvm/x86.c:2097-2126` — `kvm_emulate_monitor_mwait()`
  (MWAIT/MONITOR emulated as NOP).
- `arch/x86/kvm/x86.c:3674-3757,5079` — `record_steal_time()`,
  `kvm_steal_time_set_preempted()`.
- `arch/x86/kvm/x86.c:9898-9910,9979-10002` — `kvm_sched_yield()`,
  `KVM_HC_KICK_CPU`/`KVM_HC_SCHED_YIELD` handlers.
- `kernel/sched/cputime.c:256-294,499-503` —
  `steal_account_process_time()`, `is_cpu_preempted()`,
  `account_process_tick()`'s `clock_preempt` update.
- `kernel/sched/fair.c:13207-13301` — `ivh_steal_imminent()`,
  `ivh_rq_capacity_and_timeleft_ok()`.
- `kernel/sched/bpf_sched.c:37` — `ivh_time_left_threshold_ns` default
  (4,000,000 ns).
- `/home/nick/vsched_main/vcapacity/main.cpp:211-258,349-454,656-738,
  837-882` — `get_cpu_information()`, `get_finalized_data()`,
  `do_profile()`, `run_computation()`.
- `NHextend3.c:123-152` — `calibrate_tsc()`/`tpause_wait_ns()`, the
  pattern both TSC code sketches above are modeled on.
