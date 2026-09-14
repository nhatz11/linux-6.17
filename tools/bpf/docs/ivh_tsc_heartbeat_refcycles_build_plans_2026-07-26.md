# Build plans: TSC heartbeat (consumer #2) and REF_TSC+idle vcap steal (consumer #3)

Date: 2026-07-26. Branch `kernel-43-clean`, repo
`/home/nick/kernels/linux-6.17-rseqport`. **Planning only — no source
edited, nothing built, nothing booted in this pass.**

Predecessor: `tools/bpf/docs/ivh_tsc_replacement_consumers_2_3_design_2026-07-25.md`
(feasibility). That doc's scope split still holds: **consumer #1
(`ivh_this_cpu_steal_ns()` in `cs_enter()`/`cs_exit()`,
`kernel/locking/spinlock.c`) is out of scope and stays exact host ground
truth** — it is the yardstick IVH is evaluated against and must not become
an inferred number.

Everything asserted below was re-verified against this tree and this
running guest during this pass, not carried over from the summary. §0
lists what was checked and the six places the prior record turns out to
be **wrong or stale**.

> **Revision, 2026-07-26 (correction pass).** This document was re-checked
> claim by claim against the tree before anyone built from it. Every
> file:line citation was re-opened; the ones that had drifted are fixed in
> place. Four substantive corrections were made to the document's *own*
> content, and each is marked inline where it lands rather than only listed
> here:
>
> 1. **§0.3.0 / §0.3.3 / §2.7 / §3.7 — two divergent copies of
>    `vsched_module.c`.** The `/proc/vcap_preempted` harness exists in the
>    in-tree source but **not in the module currently loaded on this
>    guest**. §2.7's "no kernel rebuild" claim was wrong.
> 2. **§1 / §4.1 / §2.4 / §2.5 — the "known-negative result" framing was
>    overstated.** `ivh_adaptive_spinning_final_report_2026-07-16.md`
>    measured a different consumer with a different false-positive cost,
>    and its stated mechanism does not hold at `CONFIG_HZ=1000`.
> 3. **§0.1 / §3.6 / §4 — `0x013c` is not a "GP-counter bus-clock
>    fallback" on this CPU family.** It is fixed counter 2 as
>    `CPU_CLK_UNHALTED.REF_TSC_P`.
> 4. **§0.3.2 — `nmi_watchdog=0` has a confirmed cause**
>    (`hardlockup_detector_disable()` in `kvm_guest_init()`), not the
>    guessed one.
>
> Two new findings were added: §0.3.5 (in-tree comments still reasoning
> from `CONFIG_HZ=250`) and §0.3.6 (the kernel `is_cpu_preempted()`
> nohz-idle blind spot, which Plan 1 Step 0 inherits).
>
> Everything else — the perf API behaviours, the config states, the
> `struct rq` layout, the qspinlock coverage argument in §2.2, the
> idle/iowait mutual-exclusivity claim in §3.4, the REF_TSC counter
> exhaustion problem in §3.6, and the live measurements in §0.2 — was
> re-verified and **holds as written**.

---

## 0. Re-verification log, and corrections to the record

### 0.1 Confirmed against source

| Claim | Verified at |
| --- | --- |
| `pv_wait_early()` ends in `return vcpu_is_preempted(prev->cpu);` | `kernel/locking/qspinlock_paravirt.h:317` |
| `PV_PREV_CHECK_MASK 0xff`, `SPIN_THRESHOLD (1 << 15)` | `qspinlock_paravirt.h:38`, `arch/x86/include/asm/spinlock.h:25` |
| `pv_wait_early()` called from `pv_wait_node()`'s inner loop | `qspinlock_paravirt.h:346-352` |
| `is_cpu_preempted()`: `sched_clock() - cpu_rq(n)->clock_preempt > 1500000`, `EXPORT_SYMBOL` | `kernel/sched/cputime.c:288-294` |
| `clock_preempt` written unconditionally at tick head | `kernel/sched/cputime.c:503` (`account_process_tick()`) |
| `sched_clock()` on x86 is a static call, pointed at `kvm_sched_clock_read` on this guest | `arch/x86/include/asm/paravirt.h:35`, `arch/x86/kernel/kvmclock.c:89,99` — so `is_cpu_preempted()` **is** PV-sourced, as claimed |
| `get_steal_and_preemptions()` returns `rq->preemptions` and `paravirt_steal_clock(cpunum)` | `kernel/sched/core.c:191-199`, exported at `:245` |
| `/proc/vcap_info` wire format = 4 lines/CPU (`CPU %d:\n%llu\n%llu\n%llu\n`), explicitly frozen | `custom_modules/vsched_module.c:294-345` |
| `x86_perf_rdpmc_index()` exists; **requires IRQs disabled** and a prior `perf_event_read_local()` validity check in the same section | `arch/x86/events/core.c:1263-1283`, decl `arch/x86/include/asm/perf_event.h:634` |
| In-tree template for kernel-created counter + `rdpmc` | `arch/x86/kernel/cpu/resctrl/pseudo_lock.c:308-420` |
| `perf_event_create_kernel_counter()` exported GPL | `kernel/events/core.c:13771,13869` |
| `perf_event_read_local()` does its own `local_irq_save()` + `pmu->read()`; `-EINVAL` if per-CPU event's CPU != this CPU; **`-EBUSY` if pinned and not currently `oncpu`** | `kernel/events/core.c:4744-4816` |
| `PERF_COUNT_HW_REF_CPU_CYCLES` = 9, maps to Intel pseudo-encoding `0x0300` = fixed counter 2 (`CPU_CLK_UNHALTED.REF`) | `include/uapi/linux/perf_event.h:76`, `arch/x86/events/intel/core.c:42,60` |
| Fallback encoding `0x013c` is substituted into `intel_perfmon_event_map[PERF_COUNT_HW_REF_CPU_CYCLES]` **only** when architectural fixed REF_CYCLES is not enumerated | `arch/x86/events/intel/core.c:6776-6780` (`intel_pmu_ref_cycles_ext()`) |
| On this CPU family that fallback is **still fixed counter 2**, not a GP counter and not bus clock | this box is `INTEL(R) XEON(R) GOLD 6554S` (Emerald Rapids) → `intel_pmu_init()` case `INTEL_EMERALDRAPIDS_X` (`intel/core.c:7479`) → `intel_pmu_init_glc()` (`:7493`) → `event_constraints = intel_glc_event_constraints` (`:6802`), which carries **both** `FIXED_EVENT_CONSTRAINT(0x0300, 2)` and `FIXED_EVENT_CONSTRAINT(0x013c, 2) /* CPU_CLK_UNHALTED.REF_TSC_P */` (`:337-338`). `0x013c` reads as bus clock only on older parts, where it is `PERF_COUNT_HW_BUS_CYCLES`'s encoding (`:41`) |
| KVM hardware-gates guest PMU across VM exit via atomic PERF_GLOBAL_CTRL switch | `arch/x86/kvm/vmx/vmx.c:7078` (`atomic_switch_perf_msrs`), called at `:7334`; VMCS bits at `:2582` |
| `tsc_khz` is `EXPORT_SYMBOL` | `arch/x86/kernel/tsc.c:41`, decl `arch/x86/include/asm/tsc.h:73` |
| `get_cpu_idle_time_us()` / `get_cpu_iowait_time_us()`, `EXPORT_SYMBOL_GPL`, ktime-resolution (not tick-quantized) | `kernel/time/tick-sched.c:801,827`, decl `include/linux/tick.h:141-142` |
| `idle_sleeptime` and `iowait_sleeptime` are **mutually exclusive** accumulators — total halted time needs both | `kernel/time/tick-sched.c:732-737` |
| `account_idle_ticks()` (the nohz catch-up path) is **tick-quantized** (`ticks * TICK_NSEC`) | `kernel/sched/cputime.c:533-550`, caller `kernel/time/tick-sched.c:1389-1409` |
| `kcpustat_cpu(cpu)` accessor | `include/linux/kernel_stat.h:52` |

### 0.2 Confirmed live on this guest

- `CONFIG_PARAVIRT_SPINLOCKS=y`, `CONFIG_HZ=1000`, `CONFIG_NO_HZ_COMMON=y`,
  `CONFIG_NO_HZ_FULL=y`, `CONFIG_VIRT_CPU_ACCOUNTING_GEN=y`,
  `CONFIG_PERF_EVENTS=y`, `CONFIG_PARAVIRT_TIME_ACCOUNTING` **not** set,
  `CONFIG_LOCK_EVENT_COUNTS` **not** set.
- `/sys/devices/system/cpu/nohz_full` is `(null)` → no CPU is in nohz_full
  mode, so `vtime_accounting_enabled_this_cpu()` is false everywhere and
  the **tick-based** accounting branch of `account_process_tick()` is the
  live one. Idle *dynticks* are still active, which is what matters below.
- Clocksource is `kvm-clock`; `tsc` is present in
  `available_clocksource` (i.e. TSC was **not** disqualified as unstable).
  `tsc: Detected 2200.000 MHz processor`. CPU flags include `tsc`,
  `rdtscp`, `tsc_adjust`, `tsc_known_freq`, `arch_perfmon`.
- `/proc/stat` steal column is nonzero (179645 at the time of the original
  pass; 197813 on re-check — it is cumulative and still climbing, which is
  the point) → `paravirt_steal_enabled` static key is on (enabled at
  `arch/x86/kernel/kvm.c:1042`) despite `CONFIG_PARAVIRT_TIME_ACCOUNTING`
  being unset.
- **The vPMU works and REF_TSC runs at TSC rate.** Measured this pass:
  `perf stat -e ref-cycles,cycles` over a 1.6 s busy loop →
  `3,495,266,016 ref-cycles (2.160 G/sec)` vs `6,347,580,425 cycles
  (3.922 GHz)`. 2.160 G/sec against a 2200 MHz TSC confirms REF_TSC is
  counting at TSC rate. This is the single load-bearing measurement for
  Plan 2 and it is direct, not inferred.
  `/sys/bus/event_source/devices/cpu/caps/pmu_name` = `sapphire_rapids`;
  the CPU is an `INTEL(R) XEON(R) GOLD 6554S` (Emerald Rapids, which shares
  the SPR PMU name), and `journalctl -k` confirms `tsc: Detected 2200.000
  MHz processor`. Note that the rate is what matters here, **not** which
  encoding the PMU driver picked — see the §4 correction on `0x013c`.

### 0.3 Corrections to the record — read these before building

> **Read §0.3.0 first.** There are two divergent copies of
> `vsched_module.c` on this machine and every citation below refers to the
> in-tree one.

### 0.3.0 Two copies of `vsched_module.c` — which one is authoritative

This tripped up both prior reports and it will trip up a build if it is not
settled first.

| | in-tree source | source the running module came from |
| --- | --- | --- |
| Path | `/home/nick/kernels/linux-6.17-rseqport/custom_modules/vsched_module.c` | `/home/nick/vsched_main/vsched_kernel/custom_modules/vsched_module.c` |
| Length | 569 lines | 420 lines |
| `proc_create()` calls | **7** (adds `vcap_preempted`, line 536) | 6 (lines 386-391) |
| `preempted_src` module param | yes (line 389) | **no** |
| `vcap_cpu_preempted_now()` | yes (line 396) | **no** |
| `.ko` srcversion | `7542E3C99689CE80E00D4EB` | `E60CCA8046F6EC090C6CE3C` |

`/sys/module/vsched_module/srcversion` on this guest reads
**`E60CCA8046F6EC090C6CE3C`** — i.e. **the loaded module is the 420-line
`vsched_main` copy**, not the in-tree one. Consistent with that,
`/proc/vcap_preempted` does not exist right now and
`/sys/module/vsched_module/parameters/` does not exist at all (a module
with no params gets no `parameters/` directory).

**All `custom_modules/vsched_module.c:NNN` citations in this document
refer to the in-tree 569-line copy** (that is what `custom_modules/` means
relative to this repo, and it is what a build from this tree produces).
They were re-checked line by line against it and are correct.

**Build consequence:** anything in these plans that touches
`vsched_module.c`, or that assumes `/proc/vcap_preempted` /
`preempted_src` are available, requires rebuilding the module **from the
in-tree copy** and reloading it. That is not the module currently loaded.

### 0.3.1-0.3.6 The rest of the corrections

1. **`vsched_module.c:372-381` says "CONFIG_PARAVIRT_SPINLOCKS is NOT set
   in this kernel" (the assertion itself is at 372-373). That is false for
   the current `.config` (`CONFIG_PARAVIRT_SPINLOCKS=y`, line 388).** The
   module's workaround (calling `pv_ops.lock.vcpu_is_preempted.func`
   directly) is still *correct*, just no longer *necessary* for the stated
   reason. Nothing breaks; but do not use that comment as evidence about
   the build.

2. **`arch/x86/kernel/kvm.c` (the `ivh_pv_wait_trace` comment block) says
   the hardlockup detector reads 0 "almost certainly because this
   nested/cloud KVM guest has no usable vPMU". That inference is now
   disproven** — §0.2 shows a fully working vPMU with fixed counter 2.
   **The real reason is now confirmed and it is not speculative:**
   `kvm_guest_init()` calls `hardlockup_detector_disable()` unconditionally
   at `arch/x86/kernel/kvm.c:888`, with the comment "Hard lockup detection
   is enabled by default. Disable it, as guests can get false positives too
   easily, for example if the host is overcommitted." Every KVM guest
   disables it, regardless of vPMU. (An earlier draft guessed at "a
   `nowatchdog`/sysctl default"; that guess was unnecessary.) This matters
   for Plan 2 in both directions: the PMU is available to us, *and* nothing
   is currently competing for a GP counter — and it will stay that way,
   since re-enabling the detector would require overriding that call.

3. **`/proc/vcap_preempted`: both prior statements were half right.** It
   **is** implemented in the in-tree source, at
   `custom_modules/vsched_module.c:347-438`, registered at line 536, with a
   `preempted_src` module param (0 = KVM steal bit, 1 =
   `is_cpu_preempted()`) declared at line 389 and a two-way selector
   `vcap_cpu_preempted_now()` at line 396. **But it does not exist at
   runtime on this guest**, because the loaded module was built from the
   other copy (§0.3.0). So the 2026-07-25 doc's §3.1 note ("does not exist
   as a separate file", "checked directly") was a correct *runtime*
   observation about the loaded `.ko`, not a stale claim about the source.

   **The in-tree implementation is still the exact validation-harness
   pattern both plans below should copy**, and an A/B of a third source
   there costs one enum value, not a new file — but reaching it costs a
   module rebuild + reload first. Do not plan around it being live today.

4. **`CONFIG_LOCK_EVENT_COUNTS` is not set**, so every `lockevent_inc()` /
   `lockevent_cond_inc()` in `qspinlock_paravirt.h` (including
   `LOCKEVENT(pv_wait_early)`) compiles to nothing. **Plan 1's validation
   counters cannot use `lockevent_*`.** They must be `DEFINE_PER_CPU(u64,
   …)` surfaced through `/proc/ivh_debug` (`ivh_debug_show()`,
   `kernel/sched/fair.c:13576`), the same way `ivh_pv_wait_calls` already
   is (`arch/x86/kernel/kvm.c:1104`, `DECLARE_PER_CPU` at `fair.c:13572`,
   summed at `fair.c:13590`).

5. **Several in-tree comments still reason from `CONFIG_HZ=250`; the build
   is `CONFIG_HZ=1000`.** Specifically `custom_modules/vsched_module.c:44-56`
   ("once per 4ms at CONFIG_HZ=250 … reads 'preempted' for the last 2.5ms
   of every 4ms tick window (62.5% false-positive duty cycle)") and
   `tools/bpf/MY_ivh_atc.bpf.c:176,192-193` ("6ms = 1.5x the HZ=250 tick
   period", "the 1.5ms threshold is BELOW the 4ms tick period"). At
   `CONFIG_HZ=1000` the tick period is **1 ms**, which is *below* the
   1.5 ms `is_cpu_preempted()` threshold, so the sampling-phase duty-cycle
   argument those comments make does not hold on this build: a vCPU that is
   actually ticking cannot go 1.5 ms without a refresh. This matters
   directly to Plan 1 — see §1 and §2.4, where the prior record's
   "tick-granularity false positive" framing is corrected.

6. **The kernel's `is_cpu_preempted()` has an idle blind spot that the BPF
   copy does not.** `kernel/sched/cputime.c:288-293` compares only
   `clock_preempt`, which is written from `account_process_tick()` — and
   that does **not** run on a CPU in nohz idle (`CONFIG_NO_HZ_COMMON=y`,
   idle dynticks active per §0.2). An idle vCPU's stamp therefore ages
   without bound and reads as "preempted". The BPF-side reimplementation
   (`tools/bpf/MY_ivh_atc.bpf.c:183-189`) avoids this by taking
   `max(rq->last_idle_tp, rq->clock_preempt)`. **Plan 1's Step 0 tick
   publish inherits the kernel version's blind spot, not the BPF version's
   fix** — see §2.5 Step 0. This is the most likely real source of the
   false positives the 2026-07-16 report attributed to tick granularity.

---

## 1. Honest framing before Plan 1

The 2026-07-25 doc's verdict on consumer #2 was "not viable." That verdict
was aimed at two specific candidates, and it is **correct and unchanged**
for both:

- Candidate (a), reusing the tick-cadence heartbeat, is 400-750x coarser
  than the ~2-4 µs `pv_wait_early()` check cadence. **Swapping
  `sched_clock()` for `rdtsc()` in that heartbeat does not change its
  resolution at all** — the resolution is set by the *write* frequency
  (`HZ`), not the clock read. A tick-driven TSC heartbeat is
  resolution-identical to `is_cpu_preempted()`, which is the whole reason
  Phase 1 is instrumentation rather than a proposal.

  **Correction to how this document originally cited the prior evidence.**
  An earlier draft said `ivh_adaptive_spinning_final_report_2026-07-16.md`
  §"The `is_cpu_preempted()` question" had already measured this signal and
  that Phase 1 would merely "reproduce a result this project has already
  paid for and rejected." Re-reading that report, **that overstates it in
  three specific ways**, and the difference is load-bearing enough that it
  should not be papered over:

  1. **Different consumer.** The 2026-07-16 A/B ran in *userspace* —
     `NHextend3.c`'s `tpause` backoff loop `pread()`ing
     `/proc/vcap_preempted` with `preempted_src` flipped — not in
     `pv_wait_early()`. The signal is the same; the decision it feeds is
     not.
  2. **Different cost of a false positive.** The report is explicit that
     the throughput damage came from the *re-check*: each extra backoff
     "now also risks an extra real migration syscall … so the noise has a
     real cost: measured throughput fell *below no-opt* in one comparison."
     It states directly that **"before the re-check, the two signals
     performed the same (just different backoff counts)"**. In
     `pv_wait_early()` there is no migration syscall; a false positive
     costs an early `pv_wait()`, a different and much cheaper failure.
  3. **The stated mechanism does not survive `CONFIG_HZ=1000`.** The
     report's explanation — "a busy-but-healthy CPU reads 'preempted' for
     most of every tick" — is inherited from `vsched_module.c`'s and
     `MY_ivh_atc.bpf.c`'s HZ=250-era comments (§0.3.5). That run was at
     `CONFIG_HZ=1000`, where the 1 ms tick is *below* the 1.5 ms threshold
     and sampling phase alone cannot produce a false positive on a ticking
     CPU. The ~2x backoff volume was really measured; its cause was
     probably §0.3.6's nohz-idle blind spot, not tick granularity.

  The report's own forward-looking sentence is the fair summary, and it is
  not a verdict: a kernel-side design needing `is_cpu_preempted()` should
  "expect it to cost real throughput **unless paired with something that
  filters out its tick-granularity false positives** (e.g. requiring N
  consecutive stale reads, or widening the polling period — **untested
  here**)."

  So: Phase 1 is still expected to be a poor *signal*, for the resolution
  reason above, and it should still be built as instrumentation rather than
  shipped. But the 2026-07-16 evidence is **suggestive, not dispositive**,
  for this call site, and Step 2's histogram is what actually settles it.
- Candidate (b), self-elapsed-time heuristics, asks the wrong question.
  Unchanged.

**So what is actually new here is Phase 2, and only Phase 2**: a heartbeat
written from *inside the qspinlock spin loops themselves*, at
microsecond-or-better cadence. That is a genuinely different candidate
from (a) — it is not "candidate (a) with a different clock," it is a new
write cadence — and §2.2 below gives the coverage argument for why it
discriminates correctly where (b) could not.

Phase 1 is still worth building, but **only as a measurement instrument**
(the shadow comparator infrastructure, the histogram, the sysctl,
the `/proc` surface). Its production value is expected to be negative.
Do not let Phase 1 landing be mistaken for the technique working.

**And state the motivation plainly:** the thing being replaced,
`vcpu_is_preempted(prev->cpu)`, costs one `cmpb` on a percpu byte, needs
zero tuning, and is host-authoritative. Phase 2 costs a store in the
hottest spin loop in the kernel plus a threshold that must be tuned per
machine, to buy independence from the `kvm_steal_time` struct. That is a
**portability / research-independence argument, not a performance one.**
If a reviewer asks "what does this make faster," the honest answer is
"nothing; it makes the signal not depend on a paravirt structure."

---

## 2. Plan 1 — per-CPU TSC heartbeat for `pv_wait_early()`

### 2.1 Files touched

| File | Change |
| --- | --- |
| `arch/x86/include/asm/qspinlock.h` | declare the percpu heartbeat var, the reader inline, the threshold and source sysctls (this header is already the declaration home for `ivh_pv_wait_mechanism`, line 107) |
| `arch/x86/kernel/kvm.c` | define the percpu var, the sysctls, add entries to the existing `ivh_pv_sysctls[]` table (lines 1290-1312; three entries today, all `sizeof(unsigned long)` + `proc_doulongvec_minmax`-style handlers). The registering `ivh_pv_sysctl_init()` is 1314-1319 and needs **no** change — new table entries are picked up automatically. Also define the validation counters |
| `kernel/locking/qspinlock_paravirt.h` | Phase 1: swap/shadow at line 317. Phase 2: add the publish call in `pv_wait_node()`'s inner loop (lines 346-353) and `pv_wait_head_or_lock()`'s spin loop (lines 572-576), plus the seed publish in `pv_init_node()` (function body 323-331; the store goes after `pn->state = VCPU_RUNNING` at line 330) |
| `kernel/sched/cputime.c` | Phase 1 only: one extra store next to `clock_preempt` at line 503 |
| `kernel/sched/fair.c` | extend `ivh_debug_show()` (line 13576) with the shadow counters and the latency histogram |

Nothing in userspace changes. Nothing in `vsched_module.c` changes
(optional: add `preempted_src=2` there for a userspace-visible A/B, see
§2.7).

### 2.2 Why Phase 2 discriminates correctly — the coverage argument

This is the part that makes Phase 2 not-candidate-(b), and it needs to be
stated because it is easy to get backwards.

The intuition "prev might be running a long critical section, so it won't
be publishing" is **wrong at this call site**, and that is what saves the
design. In `queued_spin_lock_slowpath()`, the MCS predecessor releases its
successor at the moment it *acquires* the lock (`next->locked = 1` /
`pv_kick_node()`), **not** when it releases. So while our node is spinning
in `pv_wait_node()` on `node->locked`, `prev` is itself still spinning —
in `pv_wait_head_or_lock()` if it is the queue head, or in its own
`pv_wait_node()` if it is not. The long critical section is being executed
by the *current lock owner*, who is not `prev` and is not who we are
asking about.

Enumerate every state `prev` can be in during our spin:

| `prev` state | Publishing? | Detected how |
| --- | --- | --- |
| spinning in `pv_wait_node()` | yes | fresh timestamp → not preempted ✓ |
| spinning in `pv_wait_head_or_lock()` | yes | fresh timestamp → not preempted ✓ |
| halted in `pv_wait()` | no | **caught earlier** by the existing `READ_ONCE(prev->state) != VCPU_RUNNING` check at `qspinlock_paravirt.h:~270`, which returns true before the heartbeat check is ever reached ✓ |
| host-preempted mid-spin | no | stale timestamp → preempted ✓ (this is the target case) |
| guest-preempted | impossible | qspinlock spins with preemption disabled |
| just acquired, now in the CS | no | **the one real hole**, see below |

**The hole:** between `prev` acquiring the lock (stops publishing) and our
CPU observing `node->locked == 1`, `prev`'s timestamp ages. That window is
the store-release of `next->locked = 1` plus our loop's next
`READ_ONCE(node->locked)` — a few hundred cycles at most, and the loop
reads `node->locked` on *every* iteration while the heartbeat is only
consulted every 256th. With a threshold in the thousands-of-cycles range
this is a non-issue; if the threshold is tuned down toward ~1 µs it starts
to matter, and the mitigation is to keep the `node->locked` check
unconditionally first in the loop (it already is).

**A second hole, cold start:** the first time `PV_PREV_CHECK_MASK` fires
after `prev` enters the queue, `prev` may not have published yet, so its
slot holds a stale value from a previous, unrelated spin — an instant
false positive. Fix by seeding: publish once in `pv_init_node()`
(`qspinlock_paravirt.h:325`), which every node runs on queue entry. Cost
is one store per slowpath entry, negligible against the rest of the
slowpath.

### 2.3 New data structures

```c
/* arch/x86/kernel/kvm.c — definition
   arch/x86/include/asm/qspinlock.h — DECLARE_PER_CPU_ALIGNED */
struct ivh_tsc_beat {
    u64 stamp;        /* raw rdtsc() of last publish by this CPU */
} ____cacheline_aligned_in_smp;

DEFINE_PER_CPU_ALIGNED(struct ivh_tsc_beat, ivh_tsc_beat);
```

**Deliberately NOT in `struct rq`.** `rq`'s vSched block
(`kernel/sched/sched.h:1356-1380`) puts `clock_preempt` shoulder-to-shoulder
with `preemptions`, `max_latency`, `last_preemption`, `last_idle_tp`,
`ewma_act_ns` and `prmpt_flags` — all written from the tick path or the
migration path on the owning CPU. A slot that is written at ~kHz-to-MHz
rates by its owner and read remotely by every waiter queued behind it is
exactly the wrong neighbour for those. A dedicated
`DEFINE_PER_CPU_ALIGNED` gives it its own line.

The one-writer/many-readers shape is the benign case for a shared line
(no writer-writer ping-pong), but it is still a **dirtying store in the
hottest spin loop in the kernel**, and every remote read pulls the line
across the interconnect. That cost is the main risk in Phase 2 and is why
the publish must be rate-limited (§2.5) and why the whole thing is
sysctl-gated.

`stamp` is a plain `u64` written with `WRITE_ONCE` and read with
`READ_ONCE`. No seqlock needed: x86-64 aligned 8-byte accesses are
single-copy atomic, and a torn read is impossible. A *stale* read is fine
by construction — staleness is the signal.

### 2.4 New sysctls (all in `ivh_pv_sysctls[]`, `arch/x86/kernel/kvm.c:1290`)

| Name | Type | Default | Meaning |
| --- | --- | --- | --- |
| `kernel.ivh_pv_preempt_src` | `unsigned long` | `0` | 0 = KVM steal bit only (today's behavior, bit-identical); 1 = **shadow**: compute both, count agreement, **return the KVM bit**; 2 = TSC heartbeat is authoritative |
| `kernel.ivh_pv_beat_threshold` | `unsigned long` | `3300000` | staleness threshold in **raw TSC cycles**. 3.3M ≈ 1.5 ms at 2200 MHz — deliberately the exact cycle-equivalent of the `> 1500000` in `is_cpu_preempted()` (`cputime.c:292`), so Phase 1 is a controlled comparison against **`is_cpu_preempted()` as it exists in this tree today** and any divergence between the two is a bug, not a new signal |
| `kernel.ivh_pv_beat_publish_mask` | `unsigned long` | `0xfff` | Phase 2 only: publish when `(loop & mask) == 0`. See §2.5 |

`ivh_pv_preempt_src` needs a `proc_handler` that rejects values > 2 and
rejects 2 unless the heartbeat has actually been seeded — copy the guard
style of `ivh_pv_proc_wait_mechanism()` (`kvm.c:1248`), which already
demonstrates cross-knob validation in a `proc_handler` in this tree.

### 2.5 Sequence of changes — what depends on what

**Step 0 — instrumentation only, zero behavior change (small, low risk).**
Add the percpu var, the three sysctls, and five `DEFINE_PER_CPU(u64, …)`
counters, plus their `/proc/ivh_debug` lines. Add the publish store next
to `clock_preempt` in `account_process_tick()` (`cputime.c:503`):

```c
this_rq()->clock_preempt = sched_clock();
WRITE_ONCE(this_cpu_ptr(&ivh_tsc_beat)->stamp, rdtsc());
```

`rdtsc()` (not `rdtsc_ordered()`) — this is a heartbeat, not a fence, and
`rdtsc_ordered()`'s `LFENCE` in the tick path is pure cost.

Add the reader as a `static __always_inline` in `asm/qspinlock.h`:

```c
static __always_inline bool ivh_beat_stale(int cpu)
{
    u64 beat = READ_ONCE(per_cpu(ivh_tsc_beat, cpu).stamp);
    u64 now  = rdtsc();
    return (s64)(now - beat) > (s64)READ_ONCE(ivh_pv_beat_threshold);
}
```

Signed subtraction so a small negative skew (§2.8) reads as "fresh"
rather than wrapping to a huge positive.

Nothing calls `ivh_beat_stale()` yet. **This step is independently
buildable and bootable and must be booted before Step 1.**

**Known blind spot in this publish site, inherited deliberately.**
`account_process_tick()` does not run on a CPU in nohz idle, so an idle
vCPU's stamp ages without bound and `ivh_beat_stale()` will read it as
preempted (§0.3.6). This is the same blind spot the kernel's
`is_cpu_preempted()` has, and keeping it is what makes Phase 1 a
controlled comparison rather than a new signal — **but it must not be
mistaken for the tick-*granularity* effect the older comments describe,
and it will dominate the `ivh_beat_age_hist_running[]` tail if any sampled
predecessor was recently idle.** Two options if the histogram comes back
bimodal for that reason: publish additionally from the idle entry/exit
path, or take `max(beat, rq->last_idle_tp)` the way
`MY_ivh_atc.bpf.c:183-189` does. Prefer diagnosing it from the histogram
first — at *this* call site `prev` is spinning with preemption disabled,
not idle, so the blind spot may simply never fire here. That question is
itself a useful Step 3 finding.

**Step 1 — shadow comparator (small, low risk, no behavior change).**
At `qspinlock_paravirt.h:317`, replace the bare return with:

```c
{
    bool kvm  = vcpu_is_preempted(prev->cpu);
    unsigned long src = READ_ONCE(ivh_pv_preempt_src);

    if (src) {
        bool beat = ivh_beat_stale(prev->cpu);

        /* 2x2 agreement matrix */
        if (beat == kvm)
            this_cpu_inc(kvm ? ivh_beat_agree_true : ivh_beat_agree_false);
        else if (beat)
            this_cpu_inc(ivh_beat_false_pos);   /* TSC says preempted, host says no */
        else
            this_cpu_inc(ivh_beat_false_neg);   /* host says preempted, TSC missed it */

        if (src == 2)
            return beat;
    }
    return kvm;
}
```

Cost when `src == 0`: one `READ_ONCE` + one predictable branch, on a path
that already does a `READ_ONCE(ivh_pv_wait_mechanism)` two lines above.
That is the same "safe to leave compiled in permanently" posture the
`ivh_pv_wait_trace` knob already documents in `kvm.c`.

**Step 2 — the tuning histogram (small, needed before any threshold is
chosen).** Add two log2-bucketed histograms of `now - beat`, in the same
style as `ivh_obs_cs_hist` (`#define IVH_OBS_CS_HIST_BUCKETS 32` and its
`DECLARE_PER_CPU` live in `include/linux/bpf_sched.h:251-252`; the
accumulate/print side is `fair.c:13585,13605-13606,13673-13679`; raw
counts, one `/proc` line, percentiles computed in userspace — the
userspace reader is `ivh_exec.c:127-182,221`):

- `ivh_beat_age_hist_running[]` — sampled when `vcpu_is_preempted()` is **false**
- `ivh_beat_age_hist_preempted[]` — sampled when it is **true**

**This pair is the whole threshold-tuning method, and it replaces
guessing.** The correct threshold is wherever those two distributions
separate. Concretely: pick the value that puts ≤1% of the *running*
distribution above it while keeping ≥90% of the *preempted* distribution
above it. If they do not separate — if the running distribution's tail
overlaps the preempted distribution's body — **that is the answer, and
the answer is that the signal does not work at that write cadence.** For
Phase 1 that overlap is the *expected* result on resolution grounds
(2-4 µs check cadence against a 1 ms write cadence), and it is the
direction `ivh_adaptive_spinning_final_report_2026-07-16.md` points —
though see §1 for why that report does not actually settle it for this
call site. Getting the overlap from the histogram is cheaper and more
convincing than rediscovering it from a throughput regression, and if the
distributions *do* separate cleanly that is a genuinely new result rather
than a contradiction of the prior record.

**Step 3 — Phase 1 verdict gate.** Run the histogram and the agreement
matrix under this project's established workloads (hackbench and the
spinlock workload — *not* mutex; see the standing note that mutex does
not produce the relevant conditions). Expected outcome: high
`ivh_beat_false_pos`, poor separation. **Do not proceed to `src=2` on
Phase 1.** Write the result down and move to Phase 2.

**Step 4 — Phase 2, node-local publish (medium size, this is where the
real risk is).** Add to `qspinlock_paravirt.h`:

- in `pv_init_node()` (body 323-331), an unconditional seed publish, after
  `pn->state = VCPU_RUNNING` on line 330;
- in `pv_wait_node()`'s inner loop (lines 346-353), gated on
  `(loop & READ_ONCE(ivh_pv_beat_publish_mask)) == 0` and on
  `READ_ONCE(ivh_pv_preempt_src) != 0`;
- the same in `pv_wait_head_or_lock()`'s spin loop (lines 572-576).

`publish_mask` default `0xfff` = one store per 4096 `cpu_relax()`
iterations. At this project's own measured ~50 cycles/`PAUSE` (p50 = 50
cyc for a single `PAUSE`, recorded in
`ivh_tpause_ipi_verification_2026-07-22.md` §1 — the microbench itself,
`scratchpad/tpause_cost.c`, was a prior session's scratchpad and is no
longer on disk, so the number is only as good as that table), that is a
publish roughly every **~200k cycles ≈ 90 µs**
— already ~16x finer than the 1.5 ms tick floor, at 1/16th the store rate
of the `0xff` check mask. Sweeping this mask (`0xfff` → `0xff` → `0x3f`)
against the Step 2 histograms is the second tuning axis: it directly
trades interconnect traffic for detection latency, and unlike the
threshold it has a *measurable cost side*, so sweep it with throughput
instrumented, not just the histogram.

Note the asymmetry: the publish mask should stay **coarser than or equal
to** `PV_PREV_CHECK_MASK` (0xff). Publishing more often than anyone reads
is pure waste.

**Step 5 — re-run Steps 2-3 with Phase 2 active.** Same histograms, same
separation criterion. Only if separation is clean does `src=2` become a
defensible setting, and even then it should be A/B'd for throughput
against `src=0`, because the publish stores are not free.

### 2.6 Threshold: what to start with, and why not to guess

Start Phase 1 at **3,300,000 cycles** (1.5 ms × 2200 MHz) precisely
because it is not a guess — it is the existing `is_cpu_preempted()`
threshold expressed in cycles, which makes Phase 1 a controlled
reproduction. For Phase 2, **do not pick a number up front.** Land
Steps 4 and 2, collect one run of both histograms, and read the threshold
off the separation point. Anything else is calibrating a signal against
an assumption.

Derive the threshold in cycles from `tsc_khz` at init rather than
hardcoding 2200 MHz, so the knob survives a different host:
`threshold_cycles = (u64)tsc_khz * target_us / 1000`.

### 2.7 Optional: userspace-visible A/B

The in-tree `custom_modules/vsched_module.c`'s `vcap_cpu_preempted_now()`
(lines 396-403) already switches between the KVM bit and
`is_cpu_preempted()` on the `preempted_src` module param. Adding
`preempted_src == 2` → the TSC heartbeat there costs ~4 lines and one
exported symbol, and gives `/proc/vcap_preempted` — and therefore
`NHextend.c`'s existing backoff loop — a third source to A/B with **no
userspace rebuild**. That is the cheapest available way to run the
2026-07-16 comparison against the new signal.

**But two prerequisites, both consequences of §0.3.0, and neither is
free.** An earlier draft of this section claimed "no kernel rebuild and no
userspace rebuild"; the userspace half holds, the kernel half does not:

1. The loaded `vsched_module` is the 420-line `vsched_main` copy and has
   **no `vcap_preempted` file and no `preempted_src` param at all**. Even
   `preempted_src=0/1` — the exact A/B the 2026-07-16 report ran — is not
   available on this guest today. The in-tree module must be rebuilt and
   reloaded first, and it is worth confirming the reload does not disturb
   whatever produced the currently-loaded one.
2. `preempted_src=2` needs `ivh_beat_stale()`'s inputs exported to a
   module, which is a **kernel** change, so it lands with a kernel rebuild
   regardless. Only the *subsequent* A/B flips are rebuild-free.

Worth doing at Step 5, not before.

### 2.8 The open caveat: cross-vCPU TSC comparability

The ping-pong measurement (CPU 0 vs CPUs 1/4/8/15, 2000 samples each) put
the *minimum* observed offset at **108-126 cycles for every pair, with no
growth by CPU distance** — consistent with offset-aligned TSCs. Supporting
evidence from this pass: `tsc_adjust` is exposed in guest CPU flags (the
IA32_TSC_ADJUST mechanism KVM uses to keep vCPU TSCs aligned), and `tsc`
remains in `available_clocksource` (the guest never disqualified it).

**But that measurement ran for well under a second, and it does not
establish drift stability over hours.** This is an open risk, not a
closed one. It bites in a specific way: a slow relative drift makes one
CPU's published stamps systematically *look older* to a particular reader,
which is a **silent, one-directional false-positive bias** — it will not
crash anything and it will not show up as a correctness bug, it will just
quietly make one vCPU look permanently preempted to its neighbours.

Two cheap guards, both worth building into Step 0:

1. The signed comparison in `ivh_beat_stale()` already makes negative skew
   read as "fresh" rather than wrapping. Keep it.
2. Add a `/proc/ivh_debug` line reporting, per reader CPU, the **minimum**
   `now - beat` observed since boot. On offset-aligned TSCs this should
   sit near zero (or slightly negative) and stay there. A minimum that
   *drifts monotonically upward over hours* is the drift signature, and it
   costs one `u64` and one compare per check to detect. **Do not declare
   TSC comparability closed until that line has been watched across a
   multi-hour run.**

### 2.9 Size and risk

| Step | Size | Risk |
| --- | --- | --- |
| 0 (percpu var, sysctls, tick publish, reader) | ~120 lines | Low. No behavior change. Only new failure mode is a sysctl typo. |
| 1 (shadow comparator) | ~30 lines | Low at `src=0/1`. `src=2` is the first real behavior change and is opt-in. |
| 2 (histograms) | ~60 lines, mostly `/proc` printing | Low |
| 3 (Phase 1 measurement) | no code | None |
| 4 (node-local publish) | ~30 lines | **Medium-high.** Touches the hottest spin loop in the kernel and adds a remotely-read dirtying store. Perf regression is the expected failure mode, not a hang — but this is qspinlock, and this project has already had one hard freeze from a qspinlock-adjacent change (2026-07-24). Land it gated, boot it at `src=0` first to prove the gate is free, then enable. |
| 5 (Phase 2 measurement + A/B) | no code | None |

**Rebuild needed** at Steps 0, 1, 2, 4 (kernel rebuild; the user manages
the build/boot cycle).

---

## 3. Plan 2 — REF_TSC + idle correction behind an unchanged vcap interface

### 3.1 The interface contract, restated as a hard constraint

Two things do not change, at all:

- `/proc/vcap_info` stays **exactly 4 lines per CPU**:
  `"CPU %d:\n%llu\n%llu\n%llu\n"` = (preemptions, steal_time_ns,
  max_latency). `vsched_module.c:294-345` documents why — `vcap` is a
  compiled C++ binary at `/home/nick/vsched_main/vcapacity/vcap` whose
  parser is hardcoded to 4 lines, and a 5th field crashed it with
  `std::invalid_argument` on 2026-07-13.
- `void get_steal_and_preemptions(int cpunum, u64 *preempt, u64 *steals_time)`
  keeps its signature, its semantics (cumulative-since-boot nanoseconds),
  and its remote-CPU callability.

**Zero userspace changes. `NHextend*.c`, `hotthreads_*.c` and `vcap` are
not recompiled.** The entire change is behind that function.

**One semantic property is load-bearing and easy to break:
`*steals_time` must be monotonically non-decreasing.** Every consumer
deltas it across a window (`NHextend.c:read_vcap_steal()` and friends).
An inferred value that ever steps backwards produces a u64 underflow in
userspace and a nonsense multi-exabyte "steal" reading. `paravirt_steal_clock()`
is monotonic for free; an inferred value composed of three independently-sampled
quantities **is not**, and must be forced monotonic in the kernel (§3.4).

### 3.2 Where the accumulation must happen, and why

`perf_event_read_local()` (`kernel/events/core.c:4744`) returns `-EINVAL`
if a per-CPU event's `event->cpu != smp_processor_id()`. `vcap` reads all
CPUs from one thread. **Therefore the read cannot happen inside
`get_steal_and_preemptions()`** — that function runs on the reader's CPU
and asks about a remote one.

The accumulation must run **on the owning CPU**, publishing a plain `u64`
into `struct rq`, which `get_steal_and_preemptions()` then reads remotely
exactly as it already reads `rq->preemptions`. This is the structural
shape of the design and it is not negotiable.

The natural owning-CPU hook is the scheduler tick.
`account_process_tick()` (`cputime.c:~500`) is where `clock_preempt` is
already written and is the obvious site; it runs with IRQs disabled,
which `perf_event_read_local()` wants anyway.

### 3.3 New data structures

```c
/* kernel/sched/sched.h — inside the existing vSched/IVH block, ~line 1365 */
u64 ivh_ref_prev_tsc;      /* rdtsc() at last accumulate */
u64 ivh_ref_prev_ref;      /* REF_TSC counter value at last accumulate */
u64 ivh_ref_prev_idle_ns;  /* idle+iowait ns at last accumulate */
u64 ivh_ref_steal_ns;      /* THE OUTPUT: cumulative inferred steal, ns, monotonic */
u64 ivh_ref_samples;       /* accumulate() calls that produced a usable delta */
u64 ivh_ref_skipped;       /* accumulate() calls that bailed (-EBUSY, unseeded, …) */
```

Six `u64` in `struct rq` rather than a separate percpu struct: unlike
Plan 1's heartbeat these are written once per tick (not per spin
iteration) and read at ~1 Hz, so they belong next to `preemptions` /
`max_latency`, which have identical access patterns. No false-sharing
concern at this cadence.

```c
/* kernel/sched/core.c — the perf event handle */
static DEFINE_PER_CPU(struct perf_event *, ivh_ref_event);
```

### 3.4 The accumulate step, in full

```c
/* kernel/sched/core.c, called from account_process_tick() */
void ivh_ref_accumulate(void)
{
    struct perf_event *ev = this_cpu_read(ivh_ref_event);
    struct rq *rq = this_rq();
    int cpu = smp_processor_id();
    u64 ref, tsc, idle_ns, d_tsc, d_ref, d_idle_c, steal_c;

    if (!ev || !READ_ONCE(ivh_ref_steal_enabled))
        return;
    if (perf_event_read_local(ev, &ref, NULL, NULL)) {   /* -EBUSY etc. */
        rq->ivh_ref_skipped++;
        return;
    }
    tsc     = rdtsc();
    idle_ns = (get_cpu_idle_time_us(cpu, NULL) +
               get_cpu_iowait_time_us(cpu, NULL)) * NSEC_PER_USEC;

    if (unlikely(!rq->ivh_ref_prev_tsc))
        goto seed;                       /* first sample: no delta yet */

    d_tsc = tsc - rq->ivh_ref_prev_tsc;
    d_ref = ref - rq->ivh_ref_prev_ref;
    d_idle_c = mul_u64_u32_div(idle_ns - rq->ivh_ref_prev_idle_ns,
                               tsc_khz, USEC_PER_SEC);   /* ns -> cycles */

    /* how much wall time this vCPU was neither executing nor idle */
    steal_c = (d_tsc > d_ref) ? d_tsc - d_ref : 0;
    steal_c = (steal_c > d_idle_c) ? steal_c - d_idle_c : 0;   /* clamp, never negative */

    rq->ivh_ref_steal_ns += mul_u64_u32_div(steal_c, USEC_PER_SEC, tsc_khz);
    rq->ivh_ref_samples++;
seed:
    rq->ivh_ref_prev_tsc     = tsc;
    rq->ivh_ref_prev_ref     = ref;
    rq->ivh_ref_prev_idle_ns = idle_ns;
}
```

Three things in there are doing real work and should not be simplified
away:

1. **Both clamps are mandatory** and they are what enforces monotonicity
   (§3.1). `ivh_ref_steal_ns` only ever has a non-negative quantity added
   to it, so it cannot step backwards no matter how the three sampled
   quantities disagree. **The cost of the clamps is a systematic
   *under*-report of steal, never an over-report** — which is the correct
   direction to fail for a signal that gates migrations.

2. **Idle comes from `get_cpu_idle_time_us()` + `get_cpu_iowait_time_us()`,
   not from `kcpustat`.** Both matter:
   - *Both* functions, because `tick_nohz_stop_idle()`
     (`kernel/time/tick-sched.c:732-737`) puts each idle episode into
     `idle_sleeptime` **or** `iowait_sleeptime` exclusively — the literal
     `if (nr_iowait_cpu(smp_processor_id()) > 0) … else …`. Reading only
     `get_cpu_idle_time_us()` silently misses every idle episode that
     happened to have an iowait task on the runqueue, and those show up as
     phantom steal.
   - And summing them does **not** double-count the currently-in-progress
     idle episode, which is the obvious worry: each function passes a
     complementary `compute_delta` predicate down to
     `get_cpu_sleep_time_us()` — `!nr_iowait_cpu(cpu)` for idle
     (`tick-sched.c:805-806`) versus `nr_iowait_cpu(cpu)` for iowait
     (`:831-832`) — so at most one of the two ever adds the pending delta.
     The sum is the correct total halted time.
   - Not `kcpustat_cpu(cpu).cpustat[CPUTIME_IDLE]`, because the nohz
     catch-up path that fills it (`account_idle_ticks()`,
     `cputime.c:533-550`) is **tick-quantized** (`ticks * TICK_NSEC`).
     At `HZ=1000` that is ±1 ms of quantization error per idle episode,
     landing directly in the inferred steal. `get_cpu_idle_time_us()` is
     derived from `ktime_get()` at idle entry/exit and has real
     resolution.
   - Documented caveat, carried over honestly: `get_cpu_idle_time_us()`'s
     own kerneldoc (`tick-sched.c:785-799`) warns it "is partially broken
     due to the counter of iowait tasks that can be remotely updated" and
     that backward values are observable across two reads. The clamp in
     step 1 absorbs exactly this.

3. **`perf_event_read_local()` rather than `rdpmc()`.** The
   `x86_perf_rdpmc_index()` path (`arch/x86/events/core.c:1263`) requires
   IRQs disabled *and* a `perf_event_read_local()` validity check in the
   same section anyway, because the assigned counter index can change if
   the event is rescheduled — so `rdpmc` costs the same validity check
   plus manual index handling, and buys perhaps ~30 cycles once per
   millisecond. **Not worth it.** Use `rdpmc` only if profiling later
   shows the tick-path cost matters, which at 1 kHz it will not.

### 3.5 Counter setup and hotplug

```c
static struct perf_event_attr ivh_ref_attr = {
    .type           = PERF_TYPE_HARDWARE,
    .config         = PERF_COUNT_HW_REF_CPU_CYCLES,
    .size           = sizeof(struct perf_event_attr),
    .pinned         = 1,      /* must not be multiplexed — a rotated-out
                                 counter silently under-counts and would
                                 read as steal */
    .disabled       = 0,
    .exclude_user   = 0,      /* both, we want all guest execution */
    .exclude_kernel = 0,
    .exclude_hv     = 0,
    .inherit        = 0,      /* perf_event_read_local() rejects inherit */
    .sample_period  = 0,      /* counting only, no sampling, no NMI */
};
```

Creation per CPU via `perf_event_create_kernel_counter(&ivh_ref_attr, cpu,
NULL, NULL, NULL)` — the `pseudo_lock.c:308-330` call shape.

Hotplug: register a `cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "ivh/ref:online",
online_cb, offline_cb)` pair (`include/linux/cpuhotplug.h:243`). Online
creates and stores into `ivh_ref_event`; offline
`perf_event_release_kernel()`s it, NULLs the percpu pointer, and **zeroes
`rq->ivh_ref_prev_tsc`** so the next online re-seeds instead of computing
a delta across the offline gap (which would be a giant phantom steal).
Use `CPUHP_AP_ONLINE_DYN` so registration happens after the PMU is up and
the online callback runs on the target CPU.

Do all of this from a `late_initcall()`, matching `ivh_debug_proc_init()`
(`fair.c`) and `ivh_pv_sysctl_init()` (`kvm.c`).

### 3.6 The counter-contention problem — flagging this plainly

**There is exactly one REF_TSC counter (Intel fixed counter 2,
`FIXED_EVENT_CONSTRAINT(0x0300, 2)`, `arch/x86/events/intel/core.c:60`).**
A permanent, `pinned`, per-CPU kernel counter occupies it on every CPU for
the life of the boot.

Note also that on this CPU family the alternate encoding `0x013c`
(`CPU_CLK_UNHALTED.REF_TSC_P`) is constrained to **the same** fixed
counter 2 (`intel_glc_event_constraints`, `intel/core.c:337-338`), so
there is no second way to get a REF_TSC count either.

Consequence: **`perf stat -e ref-cycles` from userspace — the exact
command used to validate this design in the first place — will contend
with it.** The kernel's pinned per-CPU event is scheduled first (CPU-context
pinned events go on before anything flexible), so the userspace event
simply never gets the counter and `perf stat` reports `<not counted>`
rather than the kernel counter being disturbed. (`PERF_EVENT_STATE_ERROR`
is specifically what happens to a *pinned* event that cannot be scheduled
— it applies to the userspace side only if that event is itself pinned,
e.g. a group leader; the visible outcome is the same either way.) Safe,
but confusing, and it removes the ability to sanity-check the mechanism
with the same tool that established it.

The feasibility reports did not surface this. Mitigation, and it should be
built in from the start, not retrofitted:

- Gate creation on a sysctl `kernel.ivh_ref_steal_enabled` (default **0**).
  Writing 1 creates the per-CPU events; writing 0 releases them and frees
  fixed counter 2 back to the rest of the system. That makes "turn it off,
  run `perf stat -e ref-cycles`, turn it back on" a working workflow.
- Note in the sysctl's `MODULE_PARM_DESC`-equivalent comment that
  `nmi_watchdog` is 0 on this guest because `kvm_guest_init()` calls
  `hardlockup_detector_disable()` (`arch/x86/kernel/kvm.c:888`) on every
  KVM guest, so nothing else is holding a counter today and nothing will
  start to — but if the hardlockup detector is ever force-enabled it takes
  a GP counter (not fixed 2) and the two can coexist.

### 3.7 The debug comparator — concrete, before any swap

**File:** the **in-tree** `custom_modules/vsched_module.c` (569-line copy —
see §0.3.0; do not apply this to the `vsched_main` copy, and expect the
rebuilt module to be the one that must be loaded). **New proc file:**
`/proc/vcap_steal_compare`, mode `0444`, registered in the existing
`proc_create()` block (lines 530-536) alongside `vcap_preempted` (line
536), with a matching entry in the `if (!get_info_ent || …)` failure
unwind at 538-545 and in `vsched_cleanup()`. Deliberately a new file, for
the same reason `vcap_preempted` is one: `/proc/vcap_info`'s format is
frozen.

**Format** — one line per online CPU, six space-separated fields, chosen
so a shell one-liner or an `awk` script can diff them without a parser:

```
# cpu real_steal_ns inferred_steal_ns samples skipped delta_ppm
0 179645000000 179402118000 4821990 12 -1352
1 ...
```

- `real_steal_ns` = `paravirt_steal_clock(cpu)` (today's number, unchanged)
- `inferred_steal_ns` = `rq->ivh_ref_steal_ns`
- `samples` / `skipped` = `rq->ivh_ref_samples` / `rq->ivh_ref_skipped`
- `delta_ppm` = signed `(inferred - real) * 1e6 / real`, the single number
  to watch

This needs one new exported kernel accessor in `kernel/sched/core.c`,
sitting next to `get_steal_and_preemptions()` and following its exact
shape:

```c
void get_inferred_steal(int cpunum, u64 *inferred, u64 *samples, u64 *skipped);
EXPORT_SYMBOL(get_inferred_steal);
```

Declared in `kernel/sched/sched.h` next to line 114.

**Acceptance criterion before the swap, stated as a number:** across a
≥30-minute run under both hackbench and the spinlock workload, on every
CPU, `|delta_ppm|` stays within a chosen band **and** `skipped` stays a
negligible fraction of `samples`. Given the clamps bias the inferred value
low, expect `delta_ppm` to be persistently *negative*; a persistently
*positive* delta means idle is being under-subtracted and is a bug, not
noise. A band of ±5% (**= ±50,000 ppm**, mind the units against the field
above) is a reasonable opening target for a signal whose
consumer (`vcap`'s capacity adjustment) is itself coarse, but pick the
band from the first run's spread rather than defending 5% on principle.

### 3.8 Sequence of changes

| Step | Content | Depends on |
| --- | --- | --- |
| 1 | `struct rq` fields, `sched.h` decls, `ivh_ref_steal_enabled` sysctl. No readers, no writers. | — |
| 2 | perf event creation/release + cpuhp callbacks, gated on the sysctl. Verify by toggling the sysctl and watching `perf stat -e ref-cycles` start failing / start working. | 1 |
| 3 | `ivh_ref_accumulate()` + the call from `account_process_tick()`. Now `ivh_ref_steal_ns` is being filled but **nothing reads it**. | 2 |
| 4 | `get_inferred_steal()` export + `/proc/vcap_steal_compare` in `vsched_module.c`. | 3 |
| 5 | **Measure.** §3.7's acceptance criterion. No code. | 4 |
| 6 | Only now: add a `kernel.ivh_steal_source` sysctl (0 = `paravirt_steal_clock`, 1 = inferred) and one branch inside `get_steal_and_preemptions()`. Default 0. | 5 |

Step 6 is three lines. Steps 1-5 are the work, and Step 5 is the one that
decides whether Step 6 is defensible.

### 3.9 Size and risk

| Step | Size | Risk |
| --- | --- | --- |
| 1 | ~20 lines | Low |
| 2 | ~80 lines | **Medium.** Kernel-created per-CPU perf events at boot are a well-trodden path (`pseudo_lock.c`, hardlockup detector) but hotplug teardown ordering is where these go wrong. Test with `echo 0 > /sys/devices/system/cpu/cpuN/online` cycles before trusting it. |
| 3 | ~50 lines | Medium. Adds a `perf_event_read_local()` + two `get_cpu_*_time_us()` seqcount reads to every tick on every CPU. At 1 kHz × 16 CPUs that is cheap, but it is on the tick path, so measure it — this project has been bitten once by "unconditional instrumentation cost" already (see the 2026-07-07 post-lock overhead finding, where 25-28% of an apparent mechanism cost turned out to be tracking overhead). |
| 4 | ~60 lines | Low. New proc file, no existing format touched. |
| 6 | ~5 lines | Low mechanically; the risk is entirely in whether Step 5 justified it. |

**Rebuild needed** at Steps 1, 2, 3, 6 (kernel); Step 4 is a module
rebuild only — but note that the module rebuild must come from the
**in-tree** `custom_modules/` copy and the result must actually replace
the currently-loaded one, which came from elsewhere (§0.3.0). Verify with
`cat /sys/module/vsched_module/srcversion` against
`modinfo -F srcversion custom_modules/vsched_module.ko` after reloading;
if they differ, the old module is still resident and Step 5's numbers will
be measuring nothing.

---

## 4. Where each plan is harder than the feasibility reports implied

Four items, stated plainly.

1. **Plan 1's Phase 1 is not a stepping stone to a working signal.**
   Swapping `sched_clock()` for `rdtsc()` in a tick-cadence heartbeat
   changes the clock source but not the resolution, and resolution is the
   whole problem: a 1 ms write cadence cannot feed a 2-4 µs check. Build
   Phase 1 as instrumentation, expect it to fail as a signal, and treat
   Phase 2 as the actual proposal.

   *(Corrected from an earlier draft of this document, which called Phase 1
   "a reproduction of a known-negative result" and cited
   `ivh_adaptive_spinning_final_report_2026-07-16.md` as having already
   measured it. That overstates the prior evidence — different consumer,
   different false-positive cost, and a stated mechanism that does not hold
   at `CONFIG_HZ=1000`. §1 has the full correction. The resolution argument
   above stands on its own and does not depend on that report.)*

2. **Plan 1's Phase 2 puts a remotely-read dirtying store in the hottest
   spin loop in the kernel.** The coverage argument (§2.2) is sound and is
   genuinely stronger than the 2026-07-25 doc's candidate (b) — but the
   cost side is real and is not a rounding error. It must be rate-limited,
   sysctl-gated, and A/B'd for throughput, and the honest expected outcome
   is "correct signal, net-neutral-to-negative throughput," because the
   thing it replaces is a single `cmpb`.

3. **Plan 2's idle correction is more delicate than "subtract idle."**
   Three specific traps, each of which silently manufactures phantom
   steal: using only `get_cpu_idle_time_us()` and missing every
   `iowait_sleeptime` episode; using `kcpustat`'s `CPUTIME_IDLE` and
   inheriting `account_idle_ticks()`'s 1 ms quantization; and computing a
   delta across a CPU offline gap. All three are addressed above, but none
   of them were visible from the feasibility framing.

4. **Plan 2 permanently occupies the machine's only REF_TSC counter**, and
   in doing so breaks `perf stat -e ref-cycles` — the very command that
   validated the technique. Neither prior report mentions this. The
   sysctl-gated creation in §3.6 is the fix and needs to be in the design
   from day one, not added after someone notices `<not counted>`.

One thing is genuinely *easier* than the reports assumed, and it is worth
recording: **the vPMU on this guest is fully functional and REF_TSC runs
at exactly TSC rate** (2.160 G/sec vs a 2200 MHz TSC, measured this pass).
That was the largest single unknown in Plan 2 — the design's arithmetic
requires REF_TSC and TSC to share a timebase — and the measurement settles
it. The `(TSC delta) - (REF_TSC delta)` arithmetic is unit-clean, with no
calibration anywhere in it.

An earlier draft added that a `0x013c` "bus-clock fallback"
(`intel_pmu_ref_cycles_ext()`, `intel/core.c:6776-6780`) would have forced
a scaling factor and a calibration step. **That is wrong for this CPU
family and the correction is worth carrying**, because it changes what a
port to another host has to check. On Golden Cove and later — which is
this box, an Emerald Rapids Xeon Gold 6554S taking the
`INTEL_EMERALDRAPIDS_X` → `intel_pmu_init_glc()` path (`intel/core.c:7479,
7493`) — `intel_glc_event_constraints` pins `0x013c` to **fixed counter 2
as `CPU_CLK_UNHALTED.REF_TSC_P`** (`:337-338`), i.e. the fallback is still
REF_TSC at TSC rate, and the arithmetic would have been unit-clean either
way. `0x013c` only means bus clock on older parts, where it is
`PERF_COUNT_HW_BUS_CYCLES`'s encoding (`:41`). What a port genuinely needs
to re-verify is the *measured* ref-cycles rate against `tsc_khz`, not
which encoding got selected.

---

## 5. Files referenced

All line numbers below were re-opened and confirmed against this tree
during the 2026-07-26 correction pass.

- `kernel/locking/qspinlock_paravirt.h:38,263-318,323-331,345-353,572-576` — `PV_PREV_CHECK_MASK`, `pv_wait_early()`, `pv_init_node()`, `pv_wait_node()`, `pv_wait_head_or_lock()`.
- `kernel/locking/qspinlock.c:477,485-486` — `set_locked()` then `arch_mcs_spin_unlock_contended(&next->locked)` / `pv_kick_node()`: the proof that the MCS predecessor hands off **at acquire**, which is what §2.2's coverage argument rests on.
- `arch/x86/include/asm/qspinlock.h:105-107` — declaration home for the IVH PV knobs.
- `arch/x86/include/asm/spinlock.h:25` — `SPIN_THRESHOLD`.
- `arch/x86/kernel/kvm.c:888,1042,1104,1118,1174-1178,1248-1320` — `hardlockup_detector_disable()` (**the actual reason `nmi_watchdog` is 0, see §0.3.2 — KVM guests disable it unconditionally; it is not evidence about the vPMU either way**), steal static key, `ivh_pv_wait_calls`, `ivh_pv_wait_mechanism`, the disproven vPMU comment, `ivh_pv_sysctls[]` (1290-1312) / `ivh_pv_sysctl_init()` (1314-1319) and the `proc_handler` precedent (1248).
- `kernel/sched/cputime.c:224-238,256-294,499-527,533-550` — `account_idle_time()`, `steal_account_process_time()`, `is_cpu_preempted()` (288-294, threshold `> 1500000` at 292), `account_process_tick()` (`clock_preempt` store at 503), `account_idle_ticks()`.
- `kernel/sched/core.c:191-199,245,5819` — `get_steal_and_preemptions()` (191-199), its `EXPORT_SYMBOL` (245), `sched_tick()`.
- `kernel/sched/sched.h:113-123,1357-1381` — IVH accessor decls (`get_steal_and_preemptions` at 114), `struct rq` vSched block.
- `kernel/sched/fair.c:13572,13576-13700` — `DECLARE_PER_CPU(ivh_pv_wait_calls)`, `ivh_debug_show()`, `/proc/ivh_debug`, and the `ivh_obs_cs_hist` raw-histogram pattern.
- `include/linux/bpf_sched.h:251-252` — `IVH_OBS_CS_HIST_BUCKETS` (= 32) and its `DECLARE_PER_CPU`; `ivh_exec.c:127-182,221` is the userspace reader.
- `tools/bpf/MY_ivh_atc.bpf.c:175-208` — the BPF-side `is_cpu_preempted()`, its `max(last_idle_tp, clock_preempt)` idle fix, and the stale HZ=250 reasoning in its comments (§0.3.5, §0.3.6).
- `custom_modules/vsched_module.c:294-345,347-438,528-545` — **the in-tree 569-line copy, not the loaded one; see §0.3.0** — frozen `/proc/vcap_info` format (freeze rationale documented at 307-315), `/proc/vcap_preempted` with `preempted_src` (389) and `vcap_cpu_preempted_now()` (396), `proc_create()` block (530-536).
- `kernel/events/core.c:4744-4816,13771,13869` — `perf_event_read_local()` (`-EOPNOTSUPP` on `inherit` at 4762, `-EINVAL` on remote per-CPU event at 4782-4786, `-EBUSY` on pinned-not-oncpu at 4789-4792), `perf_event_create_kernel_counter()` and its `EXPORT_SYMBOL_GPL`.
- `arch/x86/events/core.c:1263-1283` — `x86_perf_rdpmc_index()` and its IRQ constraint; decl `arch/x86/include/asm/perf_event.h:634`.
- `arch/x86/events/intel/core.c:41,42,60,337-338,6776-6780,7478-7493` — `PERF_COUNT_HW_BUS_CYCLES`=`0x013c`, `REF_CPU_CYCLES`=`0x0300`, the fixed-counter-2 constraints, `intel_glc_event_constraints` pinning **both** encodings to fixed 2, `intel_pmu_ref_cycles_ext()`, and the SPR/EMR → `intel_pmu_init_glc()` dispatch.
- `arch/x86/kernel/cpu/resctrl/pseudo_lock.c:308-420` — in-tree kernel-counter + `rdpmc` template.
- `arch/x86/kvm/vmx/vmx.c:2582,7078,7334` — `atomic_switch_perf_msrs()` and the VMCS PERF_GLOBAL_CTRL load bits.
- `kernel/time/tick-sched.c:723-743,755-834,1389-1409` — `tick_nohz_stop_idle()` and the exclusive idle/iowait sleeptime split (732-737), `get_cpu_sleep_time_us()` (755) and the complementary `compute_delta` predicates, `get_cpu_idle_time_us()` (801, kerneldoc + "partially broken" caveat at 784-800), `get_cpu_iowait_time_us()` (827), `tick_nohz_account_idle_time()` → `account_idle_ticks()` (1389-1409).
- `arch/x86/kernel/tsc.c:41` / `arch/x86/include/asm/tsc.h:22,39,73` — `tsc_khz`, `rdtsc()`, `rdtsc_ordered()`.
- `arch/x86/kernel/kvmclock.c:89,99` — `sched_clock()` is `kvm_sched_clock_read` on this guest.
- `include/linux/cpuhotplug.h:243` — `CPUHP_AP_ONLINE_DYN`.
- `tools/bpf/docs/ivh_tsc_replacement_consumers_2_3_design_2026-07-25.md` — feasibility predecessor.
- `tools/bpf/docs/ivh_adaptive_spinning_final_report_2026-07-16.md` §"The `is_cpu_preempted()` question" — a *userspace* A/B of the tick-cadence signal. **Read §1 of this document before citing it**: it does not transfer cleanly to `pv_wait_early()`, and this document originally overstated what it establishes.
- `tools/bpf/docs/ivh_tpause_ipi_verification_2026-07-22.md` §1 — the p50 = 50 cyc/`PAUSE` figure §2.5 Step 4's cadence arithmetic depends on.
- `tools/bpf/docs/ivh_halt_ipi_and_tsc_next_steps_2026-07-23.md` Point 3 — the `clock_preempt` heartbeat as prior art.
