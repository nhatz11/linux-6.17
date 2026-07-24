# IVH final assessment — mechanism, scope, config audit, publishability

Date: 2026-07-20. Written after a live config audit (found and fixed a real
gap — §0), a brief PLE-off re-check (§1), and as a synthesis of every
measurement in `ivh_state_of_the_art_2026-07-20.md`,
`ivh_benchmark_search_2026-07-20.md`, and `ivh_hp_correlation_analysis_2026-07-20.md`.

## 0. Config audit: were we running the optimized configuration?

**No — not by default, until this session's fix.** The compiled-in sysctl
defaults in `kernel/sched/bpf_sched.c` did not match the values every
reported measurement actually used. Found live, before the PLE-off re-check:

| sysctl | compiled default (stale) | validated value (every reported test) |
|---|---|---|
| `ivh_capacity_threshold` | 512 (~50% capacity) | **1010** (~1.4% capacity — fires far closer to true exhaustion) |
| `ivh_time_left_threshold_ns` | 500,000 (500µs) | **4,000,000** (4ms) |
| `ivh_max_concurrent` | 3 | **8** |
| `ivh_time_left_source` | 0 (original ewma formula) | **1** (later `last_active_time` formula, fed by real rseq CS duration) |

Anyone building this tree fresh — including a reproduction attempt by a
reviewer — would have gotten meaningfully different, unvalidated behavior
without knowing to `echo` the right values first. **Fixed this session**:
the compiled defaults in `bpf_sched.c` now match the validated values
(needs a rebuild to take effect; the running system already has them set
live). The four items you listed as already correct were checked and
**are** correct as shipped:

- **Process/vCPU capacity gate** — `rq->cpu_capacity > ivh_capacity_threshold`
  reject in `ivh_steal_imminent()`, `kernel/sched/fair.c:13218`. Working as
  designed; only the threshold value was stale (now fixed).
- **Blocking irqsave lock, not trylock** — `ivh_selection_trylock = 0`
  (default) uses blocking `raw_spin_lock_irqsave()` for destination
  selection; `1` would switch to trylock-and-skip. Already correct.
- **2 `set_cpus_allowed_ptr()` calls, no extra `schedule()`** —
  `ivh_migrate_mechanism = 0` (default): restrict, migrate (blocks
  internally via `affine_move_task()`'s `wait_for_completion()`), restore.
  The redundant bare `schedule()` was found and removed earlier this
  session. Already correct.
- **8 concurrent migrations, 50µs per-vCPU cooldown** —
  `ivh_max_concurrent` (now 8, was stale at 3) and `ivh_eval_cooldown_ns =
  50000` (already correct as shipped). Both confirmed live.

**Bottom line**: the mechanism's logic was never wrong, but the numbers
behind every result in this session's reports were never the compiled
defaults — they had to be set by hand each session. That's now fixed at
the source level.

## 1. Brief PLE-off re-check

You disabled PLE at the host/hypervisor level (not guest-visible — no
`ple_gap`/`ple_window` sysctl exists inside this VM to confirm from here,
so this trusts your own toggle). Re-ran swaptions only, 2 rounds each, with
the now-corrected sysctls:

| | wall time | stolen/total holds |
|---|---|---|
| base (IVH off) | 31.10s, 30.58s (avg 30.84s) | 0 / ~7,800 |
| protected (IVH on) | 30.62s, 29.91s (avg 30.27s) | 0 / ~7,500 |

**+1.9% with PLE off**, vs. the earlier -2.0% (noisy/neutral) with PLE on.
Direction flipped positive and — notably — the run-to-run spread collapsed
from 22.5–37s (with PLE on) to 29.9–31.1s (with PLE off). That's real
evidence PLE itself was injecting host-side timing noise into these
measurements, but it does **not** get swaptions back as a genuine win:
stolen holds are still ~0 out of ~7,500-8,000 total. There is essentially no
lock-holding to protect here regardless of PLE state — the +1.9% is not
LHP mitigation, and with only 2 rounds this is not confirmed at the
rigor bar the rest of the session used (≥3 rounds, ideally more given how
much this specific benchmark has moved around all night). Treat as "PLE was
adding noise, not hiding a win" rather than "swaptions recovered."

## 2. How IVH actually works (one paragraph)

IVH is a proactive, guest-side, per-lock-acquisition intervention: at
spinlock entry (`ivh_pre_lock()`), before the lock is taken, it checks
whether the current vCPU is in danger of being stolen soon (capacity gate +
time-left-in-burst estimate) and, if so, migrates the *about-to-become
lock holder* to a healthier vCPU before it ever takes the lock — preventing
the preemption-while-holding event from happening at all, rather than
reacting to it. This is mechanistically distinct from PLE, which is
reactive and waiter-side: a hypervisor feature that detects a vCPU spinning
past a threshold and yields it, addressing the *symptom* (wasted spin
cycles) after a holder has already been preempted. IVH tries to prevent the
disease; PLE treats it after the fact. Because they act on different
signals at different times, they are structurally complementary rather
than redundant — confirmed anecdotally tonight (ebizzy/hackbench/dbench
have non-overlapping benefit profiles between the two mechanisms) though
not yet with a rigorous paired 2×2, see §5.

## 3. Weakness — why IVH is not a jack-of-all-trades

Two structural, not tuning-fixable, limits, both proven this session:

1. **Migration has an irreducible cost (~250–560µs+ per migration)**,
   proven via a zero-migration control (forcing zero actual migrations while
   running the full detection/eval path came back statistically identical
   to IVH-off — the cost lives entirely in the target-CPU pickup, not
   overhead in the decision path). Any workload whose synchronization
   events are more frequent than this cost can amortize over will lose net
   throughput even with perfect detection.
2. **Migrating a lock holder only pays off if the CPU it lands on is
   actually free and the lock it was holding actually gates other threads'
   progress ("leverage").** Live CS-length tracing this session
   (`ivh_command_runbook_2026-07-20.md` §3) directly falsified the
   simpler "short CS = can't help" story that was assumed earlier tonight:
   hackbench (a clear win) and vips (a clear loss) have nearly identical
   mean/median lock-hold durations (~2.3µs vs ~1.7µs); dedup (a loss) has
   the *longest* mean hold time of the whole set (278.7µs, heavy-tailed).
   Duration alone does not predict the outcome. What separates the wins
   from the losses, per the HP-correlation report, is that HP% reduction
   (the mechanism doing its narrow job) is **uniformly high (88-100%)
   across winners and losers alike** — the win/loss split is set by
   whether that protected hold actually had leverage over the workload's
   throughput and whether there was slack to migrate into, not by whether
   protection "worked." This is a real, currently only partially
   understood gap (see §5's proposed next experiment).

The corollary: any workload with (a) very frequent, short, low-leverage
synchronization (dedup/vips pipeline stage handoffs, memtier's client-side
locks behind a single-threaded server bottleneck) or (b) no CPU slack
(hackbench-g20's 50x oversubscription, where protection actively backfires
— stolen holds *rise* under IVH, 6211→6582) is structurally out of reach,
not just under-tuned.

## 4. Strength — the ≥20% claim, checked against real numbers

Confirmed wins, using the more conservative paired/isolated measurement
where available and the whole-system-toggle number otherwise (both are in
`ivh_hp_correlation_analysis_2026-07-20.md`):

| workload | paired (isolated) Δ | whole-system Δ | ≥20%? |
|---|---|---|---|
| ebizzy mmap | +61.8% | +136% | **yes, by a wide margin either way** |
| dbench fsync 8c | +18.1% | +20.2% | borderline on the conservative number, yes on the reported one |
| dbench fsync 16c | +12.1% | +33.7% | not on the conservative number, yes on the reported one |
| hackbench -g4 | +15.5% | +28.6% | not on the conservative number, yes on the reported one |

Honest framing: **one workload (ebizzy mmap) clearly and robustly clears
20% under either measurement design; the other three clear 20% only under
the whole-system-toggle measurement**, which the HP-correlation work this
session showed likely bundles in some system-wide migration collateral
alongside the workload's own protection effect. The defensible claim for a
paper is "one workload with a >60% win, a small cluster of workloads with
real, reproducible 12-34% wins depending on measurement isolation" rather
than "many workloads, consistently ≥20%." That's still a genuine, useful
result — mmap_lock-across-page-faults and disk-blocking I/O locks are
common, real patterns, not a contrived corner case — but the ≥20%-across-
many-workloads framing is not yet what the data shows.

## 5. What could broaden IVH's reach

In priority order, matching the state-of-the-art doc's open-questions list:

1. **CS-scaled adaptive `ivh_time_left_threshold_ns`** instead of one
   global constant — the threshold formula already has a CS-length term
   (`current->last_cs_ns`), it's just never scaled per-caller. This is the
   single most concrete, already-half-built lever.
2. **A leverage/slack-aware admission check** — before migrating, estimate
   whether the specific lock instance actually has waiters that would
   benefit (leverage) and whether the target CPU has real spare capacity
   right now (slack), rather than firing on any qualifying capacity/time-left
   gate. This directly targets the §3 finding that HP% reduction alone
   doesn't predict payoff. Not designed yet — this is the natural next
   research question, not an engineering backlog item.
3. **Selective disablement under oversubscription** — hackbench-g20 shows
   protection can go net-negative under heavy oversubscription; a
   nr_running-aware or load-average-aware cutoff could recover neutrality
   there without giving up the g4-style win.

## 6. Is this still publishable, and does the PLE-agnostic framing hold?

Yes, with a specific, honest framing — and the "host/hypervisor-agnostic
alternative to PLE" angle is a real, legitimate opening, not oversold, on
one specific condition:

**What's solid:** IVH is guest-only (no hypervisor cooperation, no
hardware feature, no host configuration) and mechanistically distinct from
PLE (proactive/holder-side vs. reactive/waiter-side). It has at least one
robust, large, reproducible win (ebizzy mmap, +62-136% depending on
measurement design) driven by a real, common kernel primitive (`mmap_lock`
across page faults), plus a small cluster of real, if smaller and
measurement-sensitive, wins (dbench, hackbench-g4). It has honest,
mechanistically-explained losses (dedup, vips, memtier, hackbench-g20),
not just unexplained noise — and the HP%-decoupled-from-throughput finding
is a genuine, non-obvious systems result on its own, independent of
whether IVH "wins" broadly.

**What the paper needs before the PLE-agnostic framing is fully earned:**
a real, rigorously paired PLE-on/off × IVH-on/off measurement on the
flip-pair benchmarks (ebizzy, hackbench, dbench) with the same rigor as
the HP-correlation work — not the memory-based "results roughly matched
the last big run" comparison this session's brief re-check partially
substituted for. Right now "IVH and PLE have non-overlapping benefit
profiles" is a plausible, mechanistically-motivated claim with one data
point (swaptions, PLE-off) behind it, not a demonstrated result. That
2×2 is the single highest-value experiment left before submission.

**Recommended framing for a preprint**: not "a general LHP fix" (the
losses are too real and too structural for that to survive review), but
"a guest-side, hypervisor-agnostic mechanism that provably eliminates
lock-holder preemption for a specific, common, identifiable class of
synchronization (long-held, high-leverage locks with available migration
slack — exemplified by `mmap_lock` and disk-blocking I/O paths), with a
rigorous characterization of where the same mechanism fails and why."
That framing is defensible against the numbers you actually have, is more
interesting to a reviewer than an oversold universal claim, and doesn't
require the PLE 2×2 to be true to still be a complete, honest paper —
though getting that 2×2 would meaningfully strengthen the "why does this
matter relative to existing hypervisor mitigations" section.
