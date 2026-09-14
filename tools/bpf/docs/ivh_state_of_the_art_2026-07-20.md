# IVH: state of the art, 2026-07-20

Kernel: `6.17.0-rseqport54+` (branch `kernel-43-clean`, commit `298be1454` and
this doc's own commit). This supersedes `ivh_adaptive_spinning_final_report_2026-07-16.md`
for anything about eligibility, Hot Threads, or the migration dispatch path —
all three changed substantially after that report was written. The vSpin
(adaptive spinning / paravirt) findings in that report still stand unchanged.

## TL;DR

IVH migrates a thread off a vCPU the host is about to steal, *before* it
grabs a lock, so the lock isn't held across the steal. It works, it's now
system-wide (no wrapper needed), and it has a real, measured, workload-dependent
tradeoff: big wins for lock/mutex-heavy workloads with critical sections in
the hundreds-of-µs-to-ms range (NHextend3, hackbench, PARSEC blackscholes/canneal),
real losses for pipeline/producer-consumer workloads with very frequent, very
short synchronization events (PARSEC dedup, vips). The mechanism itself has
been debugged down to its structural floor — the residual cost is the
inherent price of a synchronous cross-CPU handoff onto a busy target, not
fixable overhead. The one live lever with real headroom left is making the
migration danger-window threshold scale to the caller's actual critical
section instead of using one fixed global constant.

## 1. Architecture

### 1.1 Eligibility: one global switch, not a wrapper

Until this session, IVH only protected processes explicitly wrapped with
`ivh_exec`, which set `PF_IVH_ELIGIBLE` (a `task_struct->flags` bit) via
`prctl(PR_SET_IVH_ELIGIBLE, 1, ...)`. That flag is now **vestigial** —
nothing reads it to decide whether to do IVH work. The sole gate, everywhere,
is the `ivh_universal_eligible` sysctl (default **0**, i.e. off):

```
echo 1 | sudo tee /proc/sys/kernel/ivh_universal_eligible
```

When on, every task's contended `raw_spinlock` acquisitions, the Hot Threads
EWMA feed, `sys_ivh_cs_enter()` (syscall 470), and the mutex-spin
observability counters all engage, system-wide, unconditionally. Verified
live: dozens of unrelated real processes (kworkers, `systemd`, `gnome-shell`,
`redis-server`, `sshd-session`, even `claude` itself) genuinely engage the
mechanism with zero opt-in of their own when this is on.

Two new per-task bits fill in what the old flag used to provide:

- **`ivh_exclude`** (`task_struct`, `include/linux/sched.h`) — per-process
  opt-*out* of the global switch. `ivh_universal_eligible` has no other way
  to exclude one specific process. Set via `prctl(PR_SET_IVH_ELIGIBLE, 3, ...)`.
  Checked as `ivh_universal_eligible && !ivh_exclude` at all 6 gating sites.
- **`ivh_observe`** — per-process opt-in to *observation only* (steal-during-hold
  tracking via `cs_enter()`/`cs_exit()`), independent of eligibility and of
  `ivh_exclude`. Set via `prctl(PR_SET_IVH_ELIGIBLE, 2, ...)`. This is what
  lets `ivh_exec -v -n` report real HP% for a process with IVH genuinely off.

All 6 places that used to check `PF_IVH_ELIGIBLE` and now check
`ivh_universal_eligible`/`ivh_exclude` instead (each has a dated comment
explaining the change):

| File | Function |
|---|---|
| `kernel/locking/spinlock.c` | `ivh_pre_lock()` (main gate) |
| `kernel/locking/spinlock.c` | `cs_enter()` |
| `kernel/locking/spinlock.c` | `cs_exit()` (×2: outer gate, EWMA-feed inner gate) |
| `kernel/locking/qspinlock.c` | pending-bit spin wait-event feed |
| `kernel/locking/qspinlock.c` | MCS queue wait-event feed |
| `kernel/sched/bpf_sched.c` | `sys_ivh_cs_enter()` (syscall 470) |
| `kernel/locking/mutex.c` | `mutex_spin_on_owner()` observability |

`PR_SET_IVH_ELIGIBLE`/`PR_GET_IVH_ELIGIBLE` (`include/uapi/linux/prctl.h`,
79/80) semantics, `kernel/sys.c`:

```
arg2 == 1  full eligibility flag -- vestigial, kept for introspection/back-compat
arg2 == 2  ivh_observe (stats only, no protection, no eligibility needed)
arg2 == 3  ivh_exclude (opt out even if ivh_universal_eligible is on)
arg2 == 0  clear all three
```

### 1.2 Hot Threads (selectivity gate, off by default)

A per-task EWMA classifier, `ivh_wait_decay` (`task_struct`), meant to gate
migration to only threads that actually contend locks. Off by default
(`ivh_hot_threads_enabled=0`) because in every test this session, ungated
IVH matched or beat it. Left in the tree because the gate itself works
correctly (see §3.3) — it's a genuine, validated, but currently unneeded
selectivity tool, not a broken feature.

- Threshold recalibrated **512 → 128** (`ivh_hot_wait_threshold`,
  `kernel/sched/bpf_sched.c:187`) after live bpftrace measurement showed the
  real hot-worker `wait_decay` distribution centers *below* the old cutoff.
- A sticky one-way latch variant (`ivh_hot_wait_latch_enabled`, default off)
  fixes a feedback-loop instability the plain EWMA gate has under
  `ivh_universal_eligible` (protection itself is what feeds the gate's own
  signal — cutting migration starves the evidence needed to stay "hot").
  Live-tested: real improvement over the unlatched gate, but still measurably
  worse and noisier than no gate at all (6.99% avg HP% vs ~1% ungated, 5
  rounds). Not currently worth enabling.

### 1.3 Migration dispatch (`bpf_sched_pre_lock_migrate()`, `kernel/sched/fair.c`)

Gate 1 (capacity) → Gate 2 (time-left) → concurrency budget (`ivh_max_concurrent`,
default 3, tuned to 8 in testing) → dispatch. `ivh_migrate_mechanism` sysctl
selects the dispatch method:

- **0 (default)**: `set_cpus_allowed_ptr(current, {target})`, which *itself*
  blocks (via `affine_move_task()`'s `wait_for_completion()`, `core.c:3231`)
  until the task is actually running on `target_cpu`, then
  `set_cpus_allowed_ptr(current, saved_mask)` to restore original affinity.
  **The restore call is already cheap** — `affine_move_task()` has an early
  exit (`core.c:3116`, "can the task run on its current CPU? if so, we're
  done") that fires immediately once the mask is widened back to include
  the CPU you're already on, skipping the whole stopper/wait machinery.
  2026-07-20: dropped a redundant explicit `schedule()` call that used to
  sit between the two `set_cpus_allowed_ptr()` calls — it did nothing but
  add one more wasted context-switch round trip, since the first call
  already blocks until landed. Real, reproducible improvement measured
  (see §3.5), though modest, since the removed call was never the dominant
  cost.
- **1**: `migrate_task_to()` (`core.c:8320`) → `stop_one_cpu()` +
  `migration_cpu_stop()`. **Broken, root-caused, do not use as-is.** Because
  the caller blocks in `stop_one_cpu()`'s own wait before the stopper runs,
  `current` is `on_rq == 0` by the time `migration_cpu_stop()` executes, so
  it falls into `migration_cpu_stop()`'s `else` branch (`core.c:2779`,
  `p->wake_cpu = arg->dest_cpu`) — a **hint**, not a hard placement.
  `select_task_rq_fair()` overrides it on wakeup, and because the wakeup is
  signaled from the *source* CPU, wake-affine logic usually pulls the task
  right back home. Live-measured: 78% of "successful" mechanism-1 migrations
  landed on the wrong CPU, 71% bounced straight back to source. A working
  fix would have to replicate mechanism 0's cpus_mask-restriction trick to
  force the landing — i.e. it can't be a cheaper *distinct* mechanism, only
  a longer path to the same one. Not recommended to invest further here.

### 1.4 `sys_ivh_cs_enter()` (syscall 470) — userspace pre-lock trigger

NHextend3's own lock (a userspace cmpxchg spinlock, not a kernel primitive)
calls this before every attempt, mirroring `ivh_pre_lock()`'s role for real
kernel locks. Three fixes landed this session (`kernel/sched/bpf_sched.c`,
full dated comments on the function):

1. Now honors `ivh_universal_eligible`/`ivh_exclude` (previously only
   checked the vestigial `PF_IVH_ELIGIBLE` — a plain unwrapped NHextend3
   relying solely on the universal sysctl got **zero** protection via this
   syscall; any measured benefit in that configuration was coming from
   elsewhere).
2. Gained the per-vCPU eval cooldown (`ivh_eval_cooldown_ok()`) the kernel
   path already had — previously every single lock attempt paid the full
   evaluation regardless of how recently the same vCPU had already been
   checked.
3. **The big one**: `ivh_steal_imminent()`'s (`fair.c`) time-left formula has
   a CS-length-aware term, `current->last_cs_ns`, but it was never fed this
   task's real userspace CS duration — only the *kernel* spinlock path
   (`cs_exit()`) ever wrote it. NHextend3 self-reports its real CS duration
   via `rseq::last_cs_overall_ns` at every unlock; nothing read it. Fixed:
   `sys_ivh_cs_enter()` now does a `copy_from_user_nofault()` read of the
   caller's own rseq field right before the gate evaluation.

### 1.5 vSpin / adaptive spinning (unchanged this session, see the 07-16 report for detail)

`CONFIG_PARAVIRT_SPINLOCKS=y` plus IVH's own non-hypervisor-cooperative
substitute for the paravirt wait/kick pair (`ivh_pv_wait_mechanism` sysctl,
default 0/off). When on: waiters check the *predecessor's* live KVM steal
bit (`vcpu_is_preempted(prev->cpu)`, the in-kernel analogue of the userspace
`/proc/vcap_preempted` check) and back off with bounded `TPAUSE` naps instead
of a real `HLT`+hypercall-kick pair — never traps to the host, never needs
the host to cooperate. Confirmed real (~10% relative host-preempted
reduction on NHextend3-style workloads), no benefit on hackbench (its kernel
lock holds resolve too fast to matter). Not evaluated against PARSEC this
session.

## 2. Current workflow

### 2.1 One-shot setup: `/home/nick/IVH`

```bash
sudo /home/nick/IVH
```

Runs `setup.sh` (cgroups + loads the pre-cached `vsched_module.ko` for the
running kernel), applies the confirmed-good sysctl combo (see §4), starts
`vcap -p 200 -s 5000` (host-steal injector) + `MY_ivh_atc` (BPF scheduler
extension daemon). If the module vermagic doesn't match the running kernel
(new kernel build, same or different version string — **check actual CRCs,
not just the string**, they can coincidentally repeat across builds on this
project's local-version scheme):

```bash
sudo ./build_cached_module.sh   # extracts real CRCs from /boot/vmlinuz-$(uname -r)
```

Check daemons before restarting: `ps -ef | grep -E "MY_ivh_atc|vcap"` —
duplicates cause D-state hangs and double BPF hook firing.

### 2.2 `ivh_exec` — now kernel-native, no BPF, no `sudo`

```
ivh_exec <prog>              -- opt in (vestigial unless ivh_universal_eligible is on)
ivh_exec -v <prog>           -- + report real host-preemption stats on exit
ivh_exec -n <prog>           -- exclude (IVH off even if universal is on)
ivh_exec -v -n <prog>        -- exclude + stats -- "IVH off, but still measured"
```

2026-07-20: the `-v` stats mode used to load a BPF program with fentry/fexit
hooks on the whole system's `_raw_spin_lock*`/`_raw_spin_unlock*` family.
That inflated a true ~12s hackbench run to ~16-21s, and its shadow per-task
depth tracking couldn't see rq-lock ownership transfer across context
switches (acquired by prev, released by next in `finish_lock_switch()`) —
every cross-CPU reschedule of the observed thread manufactured a fake
"stolen hold," and since IVH migrations are themselves extra forced
reschedules, this made the tool briefly report **IVH making HP% worse**,
which was backwards. Replaced with `ivh_observe` (§1.1): reads
`/proc/ivh_debug`'s `ivh_obs_total_holds`/`ivh_obs_stolen_holds` before fork
and after `waitpid`, reports the delta. No BPF, no `sudo`, no system-wide
overhead, and it's now verifiably more correct (reuses `lock_depth`'s
already-fixed context-switch accounting instead of a shadow tracker).
**Limitation**: these are global per-CPU counters, not TGID-scoped — don't
combine `-v` with unrelated concurrent `ivh_universal_eligible` traffic if
you want a clean single-workload measurement.

### 2.3 Standard test sysctls (validated combination, this session)

```bash
echo 1010    | sudo tee /proc/sys/kernel/ivh_capacity_threshold
echo 4000000 | sudo tee /proc/sys/kernel/ivh_time_left_threshold_ns   # see §4 caveat
echo 8       | sudo tee /proc/sys/kernel/ivh_max_concurrent
echo 1       | sudo tee /proc/sys/kernel/ivh_time_left_source          # last_active_time formula
echo 0       | sudo tee /proc/sys/kernel/ivh_selection_trylock         # blocking lock
echo 0       | sudo tee /proc/sys/kernel/ivh_migrate_mechanism         # 0, NOT 1 (see §1.3)
echo 1       | sudo tee /proc/sys/kernel/ivh_universal_eligible        # the actual on/off switch
```

### 2.4 Methodology trap that cost real time this session — always use ≥20s test runs

`vcap`'s steal/capacity data (`/proc/vcap_info`) refreshes in discrete steps
roughly every 5-6s. A 5s test run has close to a coin-flip chance of landing
entirely inside one stale window vs straddling a refresh boundary, producing
genuinely bimodal, misleading single-run results (the *same* condition
measured 1.6% and 27% host-preempted in different 5s rounds this session).
**Always use `NHEXTEND_DURATION=20` or longer**, and for anything borderline,
run at least 2-3 rounds — even at 20s, single-round outliers still happen
occasionally (confirmed live this session, a threshold=25µs test showed -75%
on round 1, -18%/-21% on rounds 2-3; the first was the outlier).

## 3. Key findings, this session

### 3.1 Universal eligibility works, is stable, and generalizes

No crashes across the entire session (hackbench, NHextend3, PARSEC, mixed
concurrent load) under `ivh_universal_eligible=1`. `PF_IVH_ELIGIBLE`'s own
context guards (`in_task()`, `preemptible()`, `lock_depth == 0`,
`TASK_RUNNING`) already excluded the genuinely dangerous contexts
(IRQ/atomic/nested) independently of eligibility — widening eligibility only
widens *which tasks* are considered, never *which contexts* are legal. One
real, benign edge case found and fixed: a `migrate_disable()`'d kworker
tripped a kernel `WARN_ON_ONCE` (harmless, declined migration, not a crash)
because `ivh_pre_lock()` didn't check `is_migration_disabled()`/`PF_KTHREAD`
— never mattered under the old `ivh_exec`-only opt-in since only userspace
processes were ever eligible.

The shared concurrency budget (`ivh_max_concurrent=8`) is never actually
contested even with real cross-process traffic present: live-measured
occupancy peaked at 5/8 across ~37k-45k arrivals, both with and without
~24% of slot-holders being genuinely unrelated processes (a JS heap helper,
gnome-shell, redis, `vcap` itself). Two throttles upstream absorb ~100x the
raw traffic before it ever reaches the budget (per-vCPU 50µs eval cooldown,
then Gate 1 capacity check) — a workload dominates purely by asking far more
often than anything else on the system, not by any special-cased priority.

### 3.2 The Hot Threads regression saga (mostly historical context, gate is off)

Symmetric EWMA AND-gate → severe feedback-loop regression → asymmetric
cooldown → looked fixed in simulation, regressed live (signal-source
mismatch: `preempt_decay` fed from sub-µs kernel lock holds, host steal
arrives in ms-scale chunks, essentially never overlaps by chance) →
wait-only design → **also regressed live**, root-caused this session to the
512 threshold sitting *above* the real hot-worker `wait_decay` distribution's
mode → recalibrated to 128 → **confirmed working** (matches IVH-alone within
noise) → then found the same feedback instability reappears specifically
under `ivh_universal_eligible` (protection itself feeds the classifier's own
signal) → latch variant fixes the instability but costs more than no gate at
all. Current state: gate exists, is calibrated correctly, is not needed for
any workload tested so far. Real value if a future workload needs to
arbitrate between *multiple simultaneous* heavy contenders competing for the
`ivh_max_concurrent` budget — never actually tested, since nothing this
session generated that scenario.

### 3.3 NHextend3: the syscall was never the bottleneck; migration economics is

Below ~100-200µs critical sections, IVH used to lose net throughput despite
reducing HP% (the mechanism was still "working," just not paying for
itself). Two independent live investigations confirmed the ~193ns syscall
trap is 20-600x too small to explain the losses, and a decisive controlled
experiment (forcing zero migrations via `ivh_capacity_threshold=0` while
running the *entire* detection/eval path) came back statistically identical
to IVH-off. The real cost: each migration drains ~250-560µs of aggregate
lock throughput at a ~100µs-CS workload (2.5-5.5 CS-equivalents), because a
migrating thread stalls every other waiter on the same lock while it's gone.
That's inherent to what a migration *is*, not fixable by a cheaper trigger.

After the 3 syscall-path fixes (§1.4) landed, the crossover point moved
substantially:

| loop_spin (CS length) | before fixes | after fixes, 4ms threshold | after fixes, 50µs threshold |
|---|---|---|---|
| 600,000 (~1.6ms) | +21% | +21-30% | +21-35% |
| 100,000 (234µs) | -54% | -42% to -54% | **-15%** |
| 50,000 (102µs) | -55% to -66% | -66% (1 run, noisy) | **-16% to -32%** |
| 10,000 (22µs) | -16% | -1.2% to -1.6% | (not retested) |
| 5,000 (13µs) | not tested | +0.8% (net win, 2 rounds) | +0.26% (net win) |

Threshold sweeping below 50µs plateaus (25µs, 10µs land in the same -15% to
-21% band, not better) — that plateau is the intrinsic migration cost
itself, not a threshold-tuning artifact (see §3.4).

**Removing the redundant `schedule()` call** (§1.3) bought a further real,
reproducible improvement on top of the threshold work: at loop_spin=100,000,
threshold=4ms, -54% → **-42%/-41%** (2 rounds). Modest, as expected — the
dominant cost was always the target-CPU pickup wait, which this doesn't
touch, not primitive overhead.

### 3.4 No single threshold value is free for every workload

`ivh_time_left_threshold_ns` (danger-window size before a predicted steal)
defaults to 4ms — sized, apparently, for something close to NHextend3's
original 1.6ms default CS, not for shorter workloads. Shrinking it to 50µs
closes most of the gap at 100-234µs CS lengths (above table) and even
*improves* the 1.6ms case slightly — but the same change measurably costs
**hackbench** part of its own benefit: protected-vs-excluded margin shrank
from ~19% faster (at 4ms) to ~5.4-5.8% faster (at 50µs), still positive but
much smaller. Root cause, confirmed by reading `ivh_steal_imminent()`: the
formula already has a CS-length term (`current->last_cs_ns`, now correctly
fed per §1.4-3), but it's only used as a *subtractive term inside* the gate
check — never used to *scale the threshold itself*. The threshold is one
fixed global constant serving every workload's very different CS length.

**Concrete next lever, not yet implemented**: replace the fixed
`ivh_time_left_threshold_ns` constant with something derived from
`current->last_cs_ns` (e.g. `threshold = k × last_cs_ns`) so the danger
window auto-scales per-workload instead of needing hand-tuning. Should, in
principle, dissolve the hackbench/NHextend3 tension — not yet validated
live.

### 3.5 PARSEC results (16 threads, native input, this session — first time tested)

`ivh_universal_eligible=1` vs plain unwrapped, `ivh_time_left_threshold_ns=4ms`
(the validated default, not the 50µs NHextend3-tuned value):

| benchmark | no-opt | IVH on | delta |
|---|---|---|---|
| blackscholes | 15.839s | 15.391s | **-2.8% (faster)** |
| canneal | 47.587s | 46.268s | **-2.8% (faster)** |
| dedup | 6.416s, 7.495s | 8.516s, 8.677s | **+16% to +33% (slower)**, both rounds |
| vips | 4.764s, 4.776s | 6.342s, 6.652s | **+33% to +39% (slower)**, both rounds |

**Real, reproducible mixed result — not a clean win.** blackscholes and
canneal (embarrassingly-parallel / atomic-CAS-based annealing, minimal
lock-based synchronization) see modest, consistent gains, matching the
"low lock exposure, migration cost easily amortized" story. dedup and vips
(both pipeline/producer-consumer architectures — bounded-queue stage
handoffs for dedup, region-based worker synchronization for vips) see real,
consistent *regressions*, both confirmed across 2 rounds each. This is
consistent with, and extends, the short-CS finding from §3.3: pipeline
handoff synchronization looks architecturally similar to "very frequent,
short critical sections," the exact regime where migration cost currently
outweighs the benefit. **Not live-profiled to confirm the exact mechanism**
(no bpftrace/`/proc/ivh_debug` breakdown was run for these 4 specifically) —
this is a well-motivated hypothesis from the pattern, not a confirmed root
cause the way §3.3's NHextend3 findings are. Worth a real investigation
before citing as settled.

## 4. Best known configuration, as of this doc

```bash
sudo /home/nick/IVH                                                    # module, cgroups, daemons
echo 1010    | sudo tee /proc/sys/kernel/ivh_capacity_threshold
echo 4000000 | sudo tee /proc/sys/kernel/ivh_time_left_threshold_ns    # see caveat below
echo 8       | sudo tee /proc/sys/kernel/ivh_max_concurrent
echo 1       | sudo tee /proc/sys/kernel/ivh_time_left_source
echo 0       | sudo tee /proc/sys/kernel/ivh_selection_trylock
echo 0       | sudo tee /proc/sys/kernel/ivh_migrate_mechanism          # NOT 1, see §1.3
echo 0       | sudo tee /proc/sys/kernel/ivh_hot_threads_enabled        # not currently useful
echo 1       | sudo tee /proc/sys/kernel/ivh_universal_eligible         # turn IVH on
```

**Threshold caveat**: 4ms is the safer, more broadly-validated default
(real wins on NHextend3-default/hackbench/blackscholes/canneal, though dedup
and vips lose regardless of threshold in this session's testing). 50µs
measurably helps short-CS NHextend3 further but costs hackbench some of its
margin — there is currently no single value confirmed best for every
workload; pick based on what you're actually running, or implement the
CS-scaled adaptive threshold from §3.4 before trusting one global number
across a mixed workload fleet.

## 5. Open questions / next steps, in rough priority order

1. **CS-scaled adaptive `ivh_time_left_threshold_ns`** (§3.4) — the one lever
   with clear, motivated headroom left; not yet implemented or tested.
2. **Why dedup and vips regress** (§3.5) — live-profile with
   `/proc/ivh_debug` deltas and bpftrace the way NHextend3's regression was
   diagnosed, rather than relying on the pattern-match hypothesis.
3. Hot Threads' latch variant, live-validated against `ivh_universal_eligible`
   with a genuine multi-heavy-contender scenario (never tested) — the one
   condition where its selectivity value could actually show up.
4. `ivh_migrate_mechanism=1` is not worth further investment as a *distinct*
   mechanism (§1.3) — any correct fix converges to mechanism 0's own trick.
5. vSpin (`ivh_pv_wait_mechanism`) not yet tested against `ivh_universal_eligible`
   or PARSEC at all this session — was validated earlier against NHextend3
   and hackbench only, under the old wrapper-only eligibility model.
