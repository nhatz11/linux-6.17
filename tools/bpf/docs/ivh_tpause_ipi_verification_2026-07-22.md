# IVH TPAUSE + IPI verification — why `ivh_pv_wait_mechanism=1` regresses ~36%

Date: 2026-07-22
Kernel: `6.17.0-rseqport56-tpauseIPI+`, branch `kernel-43-clean`, commit `298be1454`
Host: INTEL(R) XEON(R) GOLD 6554S, guest TSC = 2.200 GHz, 16 vCPUs, WAITPKG exposed.
Co-runner: separate VM running sysbench → **~46% host steal measured during the run** (see §4).

## TL;DR verdict

**STRUCTURAL LIMIT, not an implementation bug, and not a TPAUSE-intercept problem.**

Under real host oversubscription the non-halting TPAUSE-poll waiter cannot get
out of the host's way, so a host-preempted lock holder stays preempted for a
full host timeslice (>1.36 ms). The widened ~1.36 ms window is the ceiling the
majority of waits then hit, and **the IPI cannot rescue them because there is
no kick to send — the holder never released.** The IPI is architecturally
functional but operationally almost useless (it races a 270 ns self-poll and,
for the dominant stuck case, has nothing to wake to). Narrowing the window back
to 22 µs would NOT fix it either: the mechanism never yields the pCPU in either
size. The thing that makes `mechanism=0` win is precisely the HLT/yield that the
IVH thesis rejects.

The prompt's headline hypothesis ("the 1 ms window was only safe because the IPI
cuts waits short; the IPI doesn't, so we widened the ceiling 45x with no safety
net") is **directionally right about the symptom but wrong about the mechanism**:
the IPI isn't failing to wake ready waiters — the waiters are not ready. The word
they poll stays legitimately false for the whole window because the holder is
descheduled by the host and the poll-waiter is helping keep it that way.

---

## 1. TPAUSE is NOT intercepted by KVM (rules out the vmexit hypothesis)

Userspace microbench (`scratchpad/tpause_cost.c`), pinned CPU 2, mechanism-independent:

| test | result |
|---|---|
| expired-deadline `_tpause` (would-be free if native) | **p50 = 82 cycles (37 ns)**, min 78, max 132 |
| back-to-back `rdtsc` baseline | p50 = 26 cyc |
| 512-cycle `_tpause` (= kernel `IVH_PV_TPAUSE_CYCLES`) | p50 = 590 cyc (~268 ns), requested ~233 ns |
| single PAUSE (`cpu_relax`) | p50 = 50 cyc |

An intercepted TPAUSE would cost a full VM-exit round trip (thousands of cycles).
37 ns ⇒ **TPAUSE runs natively in this guest, no vmexit.** The per-nap cost is
not the problem, and the inner nap is only 512 cycles, so the wait loop
**re-reads the polled word every ~590 cycles (~270 ns)** regardless of any IPI.
That 270 ns self-poll granularity is why the IPI can save at most ~270 ns even
when it does fire — see §3/§5.

## 2. Wait-duration histogram: the majority of waits run out the full ~1.36 ms deadline

`bpftrace` kprobe+kretprobe on `ivh_pv_wait`, `mechanism=1`, during
`hackbench -g4 -l20000`, 20 s window (`scratchpad/wait_hist.bt`).
Deadline `IVH_PV_ADAPTIVE_TSC = 3,000,000 cyc ÷ 2.2 GHz = 1.364 ms`.

```
@count (calls in 20s): 141370
 <1us   :    970  (0.7%)
 <5us   :   9887  (7.0%)
 >22us  : 116849  (82.7%)   <- old window ceiling
 >100us : 116121  (82.1%)
 >900us : 107159  (75.8%)   <- near the 1.36ms ceiling

ns histogram (dominant mode in bold):
[2K,4K)      4571
[4K,8K)     12511   <- the "fast handoff" satisfied cluster
[8K,16K)     4973
...
[512K,1M)    7683
[1M,2M)   ► 87738 ◄  62% of ALL calls — sitting at the ~1.36ms deadline
[2M,4M)      8009
[4M,8M)      7885
```

**75.8% of calls run > 900 µs; the single dominant bucket (62%) is the
[1 ms, 2 ms) bucket straddling the 1.364 ms deadline.** The deadline — not the
IPI, not an early condition-change — terminates the majority of waits.

## 3. WHY they run to the deadline: the condition is still false when the timer fires

Same probe, but deref the polled word `*ptr` at return and compare to the awaited
`val`, split by wait-site (`scratchpad/wait_why.bt`, 20 s):

```
wait-site split (by val):
  val==1  (pv_wait_node, waiting pn->state==VCPU_HALTED) : 59923  (85%)
  val==3  (pv_wait_head, waiting lock->locked==_Q_SLOW_VAL): 10938 (15%)

LONG waits (>900us): 60501 total
  STILL unsatisfied at return (*ptr == val) : 50011  (82.7%)   <-- pure timeout
  satisfied at return       (*ptr != val)  : 10490  (17.3%)

SHORT waits (<900us): 10322 total
  satisfied at return : 10240  (99.2%)   <-- normal fast handoff
  still unsatisfied   :    82  (0.8%)

ivh_pv_kick (path-2 unlock IPI) calls: 22133
```

The decisive line: **82.7% of long waits return with the polled word STILL equal
to the awaited value.** They did not miss a wake — the event they wait for
(predecessor MCS handoff / lock release) simply never happened during the entire
~1.36 ms. An IPI cannot help a waiter whose condition is legitimately still
false: for a holder that never released, **no kick is ever sent.** Kicks fire on
real handoffs, which are the *short, already-fast* waits (99.2% of short waits
end satisfied). So the IPI accelerates the waits that were never the problem and
is inapplicable to the waits that are.

The dominant wait-site is `pv_wait_node` (85%): a queued MCS waiter parked on its
predecessor. When the predecessor's vCPU is host-preempted, `pn->state` stays put
until the host reschedules it — nothing the guest-side IPI can do.

## 4. The premise is real: ~46% host steal during the run

Clean `/proc/stat` field-9 (steal) delta over one full `hackbench -g4 -l20000`:

```
steal jiffies delta = 17163 ; total jiffies delta = 37654
host steal during run = 45.6% of all CPU time
```

Nearly half of guest CPU time is stolen by the co-runner. A host CFS timeslice
is typically several ms, so a lock holder that loses its pCPU can easily stay off
for > 1.36 ms — matching the histogram exactly.
(`/proc/vcap_preempted` per-cpu pread returned read-errors this session and was
not usable; `/proc/stat` steal is the reliable ground-truth signal and it is
unambiguous.)

## 5. The IPI: functional, ~13x more of them, negligible benefit, minor cost

Reschedule-IPI (`/proc/interrupts` RES, summed all CPUs) delta over one run:

| mechanism | hackbench time | RES-IPI delta |
|---|---|---|
| 0 (halt/kick) | 15.06 s | 2604 |
| 1 (TPAUSE + IPI) | 20.16 s | **33604** (~13x, +31000) |

- **As a benefit:** +31k IPIs yet 82.7% of long waits still time out. Even when
  an IPI *does* land on an in-flight TPAUSE (architecturally it does — TPAUSE
  exits on an external interrupt, and RESCHEDULE_VECTOR is one; verified
  indirectly by the fast satisfied-wait behavior), it races the 270 ns self-poll
  and saves at most ~270 ns out of a typical 4–8 µs real handoff. Operationally
  pointless.
- **As a cost:** +31k IPIs over ~20 s ≈ 1.5k/s ≈ a few ms of aggregate CPU even
  if every send vmexits — far too small to explain a ~5 s regression. So the IPI
  is **not the primary cost either.** It is a wash-to-slightly-negative red
  herring in both directions.

### The prompt's second sub-hypothesis (IPI recipient itself host-preempted)
This is true in the cases it applies to, but it is *subsumed* by the bigger
finding and is not the mechanism of the regression: for 82.7% of long waits **no
kick is sent at all** (the holder never released), so there is no IPI whose
delivery a preempted recipient could spoil. Where a kick *is* sent to a preempted
recipient, the guest TSC has usually already passed the deadline by the time the
host reschedules that vCPU, so the wait ends on its own — again, the IPI is moot.

## 6. Root cause (brutal-honesty version)

Two changes shipped together (widened window 22 µs → ~1.36 ms, and real
`smp_send_reschedule` kick). Neither is the true cause; both are downstream of a
structural mismatch:

**`mechanism=1` never yields the pCPU.** TPAUSE parks the core in C0.2 without a
vmexit (§1), so from the *host* scheduler's view the waiting vCPU is still
running and consuming its slice. Under 46% steal (§4), a lock holder that the
host has descheduled cannot be re-run while its waiters keep every vCPU "busy"
spinning for it — the waiters are actively holding the pCPUs the host would
otherwise hand to the holder. So the holder stays preempted for a full host
timeslice, the polled word stays false (§3), and the waits run to the deadline
(§2). `mechanism=0` wins by ~36% for exactly the reason IVH set out to avoid:
its HLT yields the vCPU → host runs the preempted holder → the critical section
completes → short waits.

- The **window widening** amplifies this (each stuck waiter now denies its pCPU
  up to 1.36 ms per call instead of 22 µs) but is not the root: at 22 µs the
  waiter still never yields — it just churns pv_wait_node's spin/re-hash loop
  ~60x more often, non-yielding the whole time. Narrowing it back is not a fix.
- The **IPI** neither causes nor cures the regression (§5).

## 7. Bug vs. structural — and what (if anything) is fixable

- **Implementation bug?** No. Kick targeting (`pn->cpu`, `pv_unhash` cpu),
  ordering (cmpxchg before IPI), and TPAUSE usage are all correct. Nothing here
  is a mis-targeted CPU, missing barrier, or lost wake.
- **Structural limit?** Yes. A non-cooperative, non-yielding waiter fundamentally
  cannot free the pCPU a host-preempted holder needs. No guest-only code change
  removes that. The only mechanisms that do (HLT/PV_UNHALT, a directed-yield
  hypercall, `KVM_HC_SCHED_YIELD`) are exactly the host cooperation the IVH
  thesis rejects.
- **TPAUSE wake semantics in this KVM env?** Verified fine (§1): native, wakes on
  external interrupt. Not the problem.

### Recommendation
For contended *kernel* spinlocks under real host oversubscription, the
non-halting substitute is structurally disadvantaged and **`ivh_pv_wait_mechanism`
should stay 0 (the default)**. The ~1.36 ms window paired with this kick was a
mistake, but not for the reason hypothesized — shrinking the window will not
recover the loss because the mechanism never yields. If a non-cooperative win is
wanted here, it requires a *yield* primitive (some way to return the pCPU to the
host when the awaited vCPU is preempted), which crosses back into host
cooperation. That is the honest ceiling. (This paragraph is analysis, not a
request to rebuild; the measurements above needed no kernel change.)

## Reproduce
- `scratchpad/tpause_cost.c` (`gcc -O2 -mwaitpkg`) — §1
- `scratchpad/wait_hist.bt` (`sudo bpftrace ...`) — §2
- `scratchpad/wait_why.bt` — §3
- `/proc/stat` field-9 delta around a run — §4
- `/proc/interrupts` RES delta, mech 0 vs 1 — §5
- Live A/B this session (interleaved, same load): mech0 14.28/14.16 s vs mech1 19.76/19.00 s.

Sysctls restored on exit: `ivh_pv_wait_mechanism=0`, `ivh_universal_eligible=0`.
