# Adaptive spinning (tier-2) on GLOCK-13: what actually got established, 2026-09-03

Session goal going in: does IVH's adaptive-spinning early-bail (tier-2) beat stock PV spinlock
(mechanism 0) on wall-clock throughput? Short answer: the spin-work reduction is real and
reproduces every time; wall-clock parity is unresolved, and for a more interesting reason than
"it doesn't work" — most of this investigation ended up being about whether the *detector* and
the *test environment* could even answer the question at all.

## 1. The steal bit is dead on this TDX guest — confirmed, not suspected

`ivh_pv_preempt_src=0` (the KVM steal bit, `vcpu_is_preempted()`) never fired once, in any run,
across the whole investigation: `t2fire=0`, `host_said_preempted=0`, every arm, every round,
~24M lock attempts/round.

Confirmed via **host-side ground truth** (SSH to the KVM host `mars`, read-only checks on this
CVM's actual vCPU threads under the qemu PID): every one of the 16 vCPU threads shows real,
substantial involuntary preemption —
`nonvoluntary_ctxt_switches`=22k-76k per thread, `run_delay` (`/proc/<pid>/task/<tid>/schedstat`
field 2, cumulative ns spent runnable-but-waiting) = 84-119 **seconds** per thread. Real
preemption is definitely happening; the guest's steal bit reports zero regardless.

Also confirmed the standard (non-IVH) Linux steal-time counter is equally dead: `/proc/stat`'s
`steal` field stayed at exactly 0 across a live 3-second sample while every other field advanced
normally.

**Root cause, confirmed against Intel's own TDX guest hardening spec, not guessed**: TDX guest
kernels deliberately never allocate the shared `kvm_steal_time` page that both channels depend
on — "the required memory structures are not shared between the host and the guest"
([Intel TDX guest hardening security spec](https://intel.github.io/ccc-linux-guest-hardening-docs/security-spec.html)).
This is a guest-kernel hardening policy choice, not a hardware TDX limitation, and it is **not**
gated by the TD debug attribute — a debug TD running the same guest kernel would still show
`steal=0`, because the guest kernel itself never wires up the shared page regardless of what the
host is allowed to inspect. There is no sanctioned alternative host→guest preemption-reporting
channel documented for TDX guests at all.

**Implication**: any IVH configuration relying on `preempt_src=0` was never actually exercising
tier-2's real logic. Every G0-arm result earlier in this investigation was measuring mechanism-2's
chassis (halt/wake dynamics) with the detector permanently silent, not adaptive spinning.

## 2. The TSC-heartbeat detector (`preempt_src=2`) actually works

Built earlier in this project specifically as a workaround for steal-time unreliability. Re-run
on GLOCK-13 (arms H1/H1K), it genuinely fires: `t2fire`=4,381-19,400 per round depending on run,
`t2fire/node_halts`≈7-8.5% consistently across every run. Unlike the steal bit, this is a real,
non-silent signal — it's the only working preemption channel available inside this TDX guest.

## 3. The IRQ-off fallthrough fix (this session's kernel patch)

**Finding**: mechanism 2's wake vehicle (`smp_send_reschedule()`, a maskable IPI) cannot wake a
vCPU halted with IRQs already disabled (x86 architectural fact — maskable interrupts don't
un-halt an IF=0 HLT). So mechanism 2 only takes a real `halt()` when `!irqs_disabled()` on entry;
when IRQs are already off (~37% of hackbench's waits — `rq->lock` and waitqueue locks are taken
via `*_irqsave`), it fell through to a **fixed-ceiling (~1.36ms) TPAUSE/PAUSE busy-poll**
(`IVH_PV_ADAPTIVE_TSC` = 3,000,000 TSC cycles, `arch/x86/kernel/kvm.c`).

**Patch applied** (`kvm.c`, mechanism-2 IRQ-off fallthrough block): replaced the fixed-ceiling
poll with an immediate, unbounded, uninstrumented `while (READ_ONCE(*ptr) == val) cpu_relax();`
spin — mirroring the existing mechanism-3 "pure native spin" pattern already in the file. Built
as `6.17.0-GLOCK-13+`, installed, rebooted, confirmed working via `irqoff_fallthrough_probe.sh`:
poll population dropped from ~37% pre-patch to **0.00%** post-patch, across every round.

**But it couldn't have fixed wall-clock, and didn't need to be "wrong" to fail**: this CPU has no
WAITPKG (`grep -c waitpkg /proc/cpuinfo` = 0), so the old bounded poll was *already* using plain
`cpu_relax()` internally — the patch removed a per-iteration `rdtsc()`, the fixed ceiling, and
instrumentation, but there was no cheaper instruction available to substitute. The 821µs/event
"poll tax" measured pre-patch wasn't padding on top of the spin — it *was* the spin, correctly
measured. Confirmed by independent code review (Opus): patch itself is correct and safe (verified
`pn->state`/`pv_kick_node()` termination guarantees hold; `ivh_pv_kick_node_ipi=0` doesn't create
a stall risk since the wake-side `cmpxchg` is unconditional, ahead of the IPI-send gate), no bug
found, kept as-is.

## 4. `node_iters` reduction (~24-51% across runs) is real but is NOT mostly tier-2

The script's own ceiling formula: `t2fire × 32768 / node_iters` — the maximum possible spin-
iteration reduction tier-2's actual detection can cause, given how often it fires. Observed
reductions were 14-20x over that ceiling in every run that computed it. The placebo check
(`head_iters`, a code path tier-2 structurally cannot reach) moved in the *same direction* as the
primary metric in multiple runs — meaning D and the mechanism-2 arms aren't even running
comparable contention profiles; `node_iters` isn't a clean read of tier-2's own mechanism here.

**What actually drives it**: D halts unconditionally on a fixed `SPIN_THRESHOLD` timer regardless
of whether the predecessor is really preempted; mechanism-2 arms (`rearm_max=0`) halt far less
often. Fewer halts on the predecessor side → fewer `prev->state != VCPU_RUNNING` (tier-1) signals
for everyone queued behind it → more early bails → a self-reinforcing loop that has nothing to do
with tier-2 correctly detecting real host preemption. D halting 2.3-3.2x more often than the
mechanism-2 arms, for the *same total workload*, is the actual explanation for most of the
node_iters gap.

## 5. Wake-vehicle cost gap — largely architectural, not a bug

Mechanism 2's real halts (the `!irqs_disabled()` case, unaffected by the patch above) cost
~2.6-2.9x more per event than D's (measured: D≈218-267µs, mechanism-2≈614-629µs mean per real
halt, reproduced across multiple probe runs). Traced to the actual difference: mechanism 2's wake
is a genuine `smp_send_reschedule()` IPI (full interrupt entry + ISR + EOI + IRET on the target),
while D's hypercall wake (`KVM_HC_KICK_CPU` → `pv_unhalted`) retires the HLT with no ISR at all.
Largely irreducible from the guest side.

**Node-level wake is symmetric with stock, not a hidden IVH disadvantage** — checked against the
actual pre-IVH vanilla source (`git show ivh-step-0-vanilla-base:kernel/locking/qspinlock_paravirt.h`):
true upstream `pv_kick_node()` *also* sends zero wake signal at the node level, by design ("avoids
a wake/sleep cycle"). D and every mechanism-2 arm rely equally on the next timer tick for node-
level real halts; `ivh_pv_kick_node_ipi` is an IVH-exclusive addition beyond stock, not a stock
capability IVH arms were missing.

The one real, isolable asymmetry is the **queue-head wake**: D takes the hypercall branch,
mechanism-2 arms were configured `kick_pure_ipi=1` (pure IPI) by choice. Tested a variant (`H1K`
= H1 with `kick_pure_ipi=0`, hypercall for the head-wake only) — see §6, inconclusive due to
noise, but plausible that this is the more promising lever than the halt-frequency dynamics.

## 6. Wall-clock: reproduces a small regression under low noise, unresolvable under high noise

**Cleanest single result** (D vs H1, 6 rounds, `-l 400000`, low background noise): **+4.8%
regression, t=2.30 after outlier correction, 6/6 rounds negative** — real detector firing on
genuine signal, still didn't net a wall-clock win.

**A co-running sysbench VM on the host was later found to introduce large, bursty load** (the
host is shared — `virsh list` shows other VMs, host loadavg ~16-18 on 72 cores normally). Once
that VM was active/restarted, D's own wall-clock swung 68% round-to-round (70.8s-118.9s) on an
*unchanged* config — this broke the paired-round methodology (per-round diffs tracked the round's
overall noise level, not the arm). A 10-round run (`D`/`H1`/`H1K`, all three same-session) under
this noise gave: H1-D=+6.47% (t=0.70), H1K-D=+8.15% (t=1.08) — neither significant, only 3/10
rounds favored either mechanism-2 variant. Outlier trimming (drop 1 low + 1 high per arm) did not
converge them to a clean answer: H1-D got slightly *worse* (7.40%), H1K-D moved closer to the 5%
line but stayed just above it (5.93%). Direct H1K-vs-H1 pairing: +1.57%, t=0.25 — no real
difference between them at this noise level.

**A speculative but real-looking secondary finding, not established**: mechanism-2 arms appeared
less volatile than D across three separate smaller runs (CV comparisons: 13.0/13.3%, 12.4/5.6%,
20.3/7.7-14.3%). This did **not** replicate at n=10 — CVs converged to ~16-17% for all three arms.
Treat as a 6-round-sample artifact, not a real effect; explicitly walked back after the larger run.

## 7. Bottom line as of this write-up

- **Tier-2's detection mechanism genuinely works** (fires on real signal, unlike the
  architecturally-dead steal bit) — this is real, validated progress independent of the
  throughput question.
- **The spin-iteration reduction is real and reproduces every run**, but is mostly a side effect
  of mechanism-2's different halt-frequency dynamics, not tier-2's detection doing the heavy
  lifting (ceiling check: tier-2's own direct contribution is ~2-6% of what's observed).
- **Wall-clock parity is not established.** The cleanest measurement available says a small
  (~5%) regression persists even with a genuinely-firing detector. Noisier runs (contaminated by
  a co-tenant VM) can't distinguish this from zero, and shouldn't be read as either confirming or
  refuting the clean result.
- **Untested candidate fix**: routing the queue-head wake through the hypercall (matching stock)
  instead of pure IPI — real lever, not yet cleanly measured, needs low-noise conditions to
  resolve.
- **A structurally separate, completely untested mechanism was found in the process**:
  `bpf_sched_pre_lock_migrate()` (proactive pre-lock vCPU migration) has been 100% dormant this
  entire investigation — gated by a static key confirmed off via direct kernel-memory read and a
  kprobe recording exactly 0 hits across a full hackbench run. Activating it (Step 8 of the
  rebuild plan: `ivh_universal_eligible=1` + load `MY_ivh_atc` + start `vcap_probe`) requires no
  kernel rebuild, and is a genuinely different lever from anything tested above.
