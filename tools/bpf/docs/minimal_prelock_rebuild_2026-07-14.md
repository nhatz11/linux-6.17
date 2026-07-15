# Minimal pre-lock-only rebuild plan (from a clean 84f1e5fcc checkout)

## Context

`84f1e5fcc` ("6.17.0-rseqport34+") is the last commit in this repo, and the
kernel it produced is the one all of tonight's A/B testing has been
comparing against. Verified via a clean `git worktree add` checkout
(`/home/nick/kernels/rseqport34-clean`) that this commit contains **only**
locus (a), pre-lock migration:

- `bpf_sched_pre_lock_migrate()` exists, is the only migration mechanism.
- No `bpf_sched_post_lock_migrate()`, no Hotlock, no freeze-yield fix, no
  `ivh_scan_stuck_waiters()` -- none of these exist at this commit at all.
- Destination search uses a **blocking** `raw_spin_lock_irqsave(&my_spinlock)`.
- Migration dispatch uses `set_cpus_allowed_ptr(current, {target}) + schedule()`,
  **not** `stop_one_cpu()`-based `ivh_migrate_self()`.
- Gate 2 (time-left) uses `rq->ewma_act_ns`, an externally-smoothed value
  written by `vcap` via `/proc/vact_write` -- not the native
  `rq->last_active_time` calculation.

Everything below the current (uncommitted) tree added on top of this is
locus (a)-adjacent infrastructure, locus (c) (post-lock, fully excluded per
explicit instruction), or diagnostics. This doc sorts all of it into what to
carry forward vs. what to leave out, based on what's actually been tested
tonight -- not on when it was written.

---

## Confirmed good — carry forward

1. **`is_cpu_preempted()` last-idle-timestamp fix** (`kernel/sched/cputime.c`).
   34+'s version only reads `rq->clock_preempt`, which stops refreshing on a
   tickless-idle CPU -- confirmed 270/270 false "target preempted" vetoes
   against genuinely healthy idle targets before this fix. The fix takes
   `max(clock_preempt, last_idle_tp)`. Small, isolated, proven correctness
   fix with no behavioral downside found. **Include.**

2. **TOCTOU fix on the concurrency-cap gate** (`bpf_sched_pre_lock_migrate()`,
   `kernel/sched/fair.c`). 34+'s gate is `atomic_read()` then, much later
   (after the destination search + veto check), a separate `atomic_inc()` --
   a real race letting concurrent callers all pass the read before any of
   them commit. The fix reserves via `atomic_fetch_inc()` immediately after
   the gate, with every exit path rolling back via `atomic_dec()`. Real
   correctness fix, not yet shown to regress anything. **Include.**

3. **Veto instrumentation** (`ivh_veto_count`, `ivh_veto_target_cap_sum`,
   `ivh_veto_fleet_cap_sum`, `ivh_veto_target_still_capacity_healthy`).
   Pure counters around the existing `is_cpu_preempted(target_cpu)` veto
   check -- zero behavior change, useful visibility. **Include.**

4. **Real trylock for the destination-search lock** (`raw_spin_trylock_irqsave`
   instead of blocking `raw_spin_lock_irqsave`). Counter-intuitive given the
   framing of tonight's investigation, but the actual A/B result: reverting
   *this* tree's trylock back to a blocking lock made iteration count
   *worse* (-42% to -47%) than the trylock version (-5.6% to -12%), not
   better -- see "confirmed does NOT fix things" below. **Keep trylock.**
   (34+ itself uses blocking lock and performs well -- but 34+ also pairs it
   with the old dispatch mechanism, item 5 below. The two are not
   independently swappable; see "still open" section.)

5. **`stop_one_cpu()`-based `ivh_migrate_self()` dispatch** (`kernel/sched/core.c`),
   in place of 34+'s `set_cpus_allowed_ptr()+schedule()`. **Decision: keep
   this, do not revert.** The old mechanism has a documented, real bug, not
   just different latency characteristics: the diff's own comment describes
   an "on_rq=0 lost-wakeup" hang class the bare `schedule()` call could hit
   by consuming a foreign sleep state belonging to some other subsystem's
   prepare-to-wait sequence -- serious enough that it was the original
   motivation for switching to `stop_one_cpu()` in the first place (which
   also never touches `current->cpus_mask`, so no restore call and no race
   window against a concurrent legitimate affinity change). The ~67-72%
   `ivh_timeout_count` rate under this project's contention pattern is a
   real cost, but it's an accepted safety-for-latency tradeoff, not a bug to
   fix by reverting. See the "still open" section below for what *is* worth
   investigating about this cost, short of reverting the mechanism itself.

6. **Hotlock, in shadow mode only** (`kernel/locking/spinlock.c`'s table/EWMA,
   `kernel/locking/qspinlock.c`'s waiter-count bracketing,
   `ivh_pre_lock_hotlock_enabled` and `ivh_post_lock_enabled`, both default
   off). Including this now avoids a second rebuild later purely to add
   observation. Mostly inert by construction: the master-switch-gated logic
   (`ivh_post_lock()`'s Hotlock sampling, locus (a)'s optional selectivity
   gate) returns after 2-3 cheap checks when its sysctl is off. **One real
   caveat, not fully inert**: the qspinlock.c waiter-count bracketing
   (`ivh_hotlock_note_waiter_enter/exit()`) is gated only on
   `PF_IVH_ELIGIBLE`, *not* on either Hotlock sysctl -- it does a real
   hash + atomic inc/dec whenever an eligible process hits a genuinely
   *contended* kernel spinlock (not our own userspace lock), regardless of
   whether Hotlock's output is used. Likely negligible for a workload that
   mostly contends on one userspace lock rather than kernel locks, but it is
   not literally zero-cost the way the switch-gated parts are -- worth
   knowing if a future A/B ever looks slightly off with Hotlock compiled in.

---

## Confirmed does NOT fix things — do not re-apply as-is

1. **Reverting the destination-search lock from trylock back to blocking,
   in this tree, alone.** Tested directly tonight on a real rebuild+reboot
   (`6.17.0-rseqport41+`): `ivh_trylock_misses` dropped to 0 as expected,
   but iteration count got *worse*, not better (no-opt vs. IVH-only went
   from -5.6%/-12% to -42%/-47%), and host-preempted improvement stayed weak
   (~1.4-1.6x, nowhere near 34+'s 4-6x). Raising `ivh_max_concurrent` from 3
   to 8 alongside it made no difference. **This specific fix, in isolation,
   is not the explanation for the gap to 34+, and should not be reapplied
   without also addressing item 2 below.**

2. **`ivh_scan_stuck_waiters()`** (deleted 2026-07-07, never in 84f1e5fcc).
   Called unconditionally every tick on every CPU, took a single global lock
   32 times per call. Confirmed root cause of a real VM freeze this project
   hit. **Never re-add.**

---

## Structural differences found, not yet tested in isolation — the real open question

1. **Why is `stop_one_cpu()` dispatch so often slow (~67-72% of migrations
   >1ms) under this project's contention pattern, and is there anything
   short of reverting it worth trying?** Given the decision above to keep
   `ivh_migrate_self()` rather than revert to the buggy old mechanism, the
   open question is narrower than "which mechanism" -- it's whether the
   stopper-kthread dispatch can be made to land faster specifically when the
   *source* CPU (not just the target) is itself under heavy host contention,
   since the stopper has to run on the source CPU to hand the task off. Not
   yet investigated. This -- not the lock type -- is the most likely
   explanation for why 34+ measures 4-6x host-preempted improvement while
   this tree measures ~1.4-1.6x under identical sysctls and workload.

2. **Gate 2 formula: native `last_active_time` (current tree) vs. externally-
   smoothed `ewma_act_ns` (34+).** A runtime toggle (`ivh_time_left_source`,
   0=native/default, 1=ewma) already exists in the current tree specifically
   so this can be A/B'd without a further rebuild. Not yet tested. Isolated
   measurement tonight showed Gate 2 rejects ~39% of evaluations when Gate 1
   is disabled (`ivh_capacity_threshold=1024`) -- so it's a real, substantial
   gate, not a no-op, and worth resolving, but it's a secondary question
   behind the dispatch-latency question above.

---

## Explicitly excluded (per direct instruction — do not include)

- **Locus (c) / post-lock migration entirely** (`bpf_sched_post_lock_migrate()`,
  `ivh_post_lock()`'s dispatch tail). Not in 84f1e5fcc; was added, caused a
  real VM freeze (see next item), fixed, then removed entirely from the tree
  as of tonight's earlier rebuild. Stays removed.
- **The qspinlock freeze-yield fix** (`ivh_virt_spin_lock()`,
  `ivh_spin_yield_enabled`, `ivh_migrate_boost`, `kernel/locking/qspinlock.c`).
  This exists *only* to fix a deadlock class that requires post-lock's
  specific failure mode (a confirmed lock holder migrated mid-hold, landing
  behind a non-yielding spinner on the destination CPU). Locus (a) never
  holds a lock during its own migration, so this deadlock class cannot arise
  from pre-lock alone. With post-lock excluded, this fix protects nothing.
  **Exclude for a true minimal pre-lock-only build.**
- **Stage A1 kernel export** (`ivh_get_vcpu_preempted()`, `core.c`). Not
  strictly pre-lock, but small (one function + one export), harmless, and
  already validated as useful and working tonight (feeds
  `vsched_module.c`'s `/proc/vcap_preempted`, which NHextend's adaptive-
  spinning mode 2 uses with confirmed good results). Optional, low-risk to
  include if convenient; skip if minimizing surface area is the priority.

---

## Recommended next build, in order

1. Start from the clean `84f1e5fcc` checkout (already done:
   `/home/nick/kernels/rseqport34-clean`).
2. Apply, in this tree: the `is_cpu_preempted()` fix, the TOCTOU concurrency-
   cap fix, veto instrumentation, the trylock change, `ivh_migrate_self()`/
   `stop_one_cpu()` dispatch (keep, do not revert -- see above), and Hotlock
   in shadow mode (both master switches default off).
3. **Do not** port forward post-lock or the freeze-yield fix -- neither has
   any purpose once locus (c) is excluded, and `ivh_scan_stuck_waiters()`,
   which caused a real VM freeze, must never be re-added regardless.
4. Add the `ivh_time_left_source` runtime toggle (already written, see
   `include/linux/bpf_sched.h`/`kernel/sched/bpf_sched.c`/`kernel/sched/fair.c`
   in the current tree) so Gate 2's formula can be A/B'd post-boot without a
   second rebuild.
5. Re-run the exact `loop_spin=600000`, `ivh_capacity_threshold=1010`,
   `ivh_time_left_threshold_ns=4000000` A/B (no-opt vs. IVH-only, NHextend2,
   `-n -l 16`) as the pass/fail check against 34+'s 4-6x host-preempted
   improvement and positive iteration delta. If it still doesn't match 34+,
   the dispatch-latency question (item 1 above) is the next thing to dig
   into -- not a reason to revert the dispatch mechanism itself.
