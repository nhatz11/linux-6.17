# IVH state of the art and next steps (2026-07-16)

## Where the project actually is right now

Branch `kernel-43-clean` (pushed to `origin/kernel-43-clean`), forked clean from
commit `84f1e5fcc` ("6.17.0-rseqport34+") with two commits on top:

1. `cee0dde3d` — `kernel.config` updated to `CONFIG_HZ=1000` (see below, this is
   the single most important finding of the last 24 hours), plus the fixed
   `tools/bpf/MY_ivh_atc.bpf.c` (destination-selection gates, `is_cpu_preempted()`
   threshold, a real `GATE_LOCKHOLDER` fix — all described below). Kernel built
   and installed as `6.17.0-rseqport43-1000+`.
2. `d3ca8770f` — restores `NHextend2.c`, `install_module.sh`,
   `build_cached_module.sh`, `tools/bpf/scripts/ivh_apply_optimal.sh`, which
   existed as uncommitted work on a different branch (`ivh-toggle-matrix`) and
   didn't carry over when this branch was checked out fresh.

This branch intentionally contains **none** of the toggle-matrix machinery
(`ivh_migrate_mechanism`, `ivh_selection_trylock`, `ivh_gate4_strict`,
`ivh_time_left_source`, Hotlock, the TOCTOU concurrency-cap fix, veto
instrumentation) that existed on the `ivh-toggle-matrix` / `rseq-port` lines.
That's deliberate — the plan (see "Next steps" below) is to re-add these one at
a time against a *confirmed-good* baseline, now that the baseline is understood.

## The headline finding: CONFIG_HZ, not IVH's code, explained most of one entire
night's confusion

For roughly 24 hours, a huge amount of effort went into chasing why a kernel
built from this same tree, with the same BPF selector, only achieved a modest
~1.5-1.6x host-preempted-CS improvement with a real iteration-count cost
(-7% to -18%), while the *original*, already-installed `34+` kernel
consistently delivered **<1% host-preempted CS and a positive iteration
count** on the identical `NHextend2` test recipe.

Every code-level hypothesis was tested and ruled out: destination-search lock
type (trylock vs blocking), migration dispatch mechanism (`stop_one_cpu()` vs
`set_cpus_allowed_ptr()+schedule()`), Gate 1/2 formula and structure, Hotlock
overhead (turned out not to even exist on this branch's line of history),
`kernel/sched/cputime.c`'s `is_cpu_preempted()` (byte-identical between the two
trees).

The actual answer, found by diffing `/boot/config-6.17.0-rseqport34+` against
`/boot/config-6.17.0-rseqport43+`: **the original 34+ kernel was built with
`CONFIG_HZ=1000`; every "pseudo-34" rebuild since then (42+, the first 43
build) was built with `CONFIG_HZ=250`.** That was the *only* substantive
difference in the entire `.config` (14-line diff, one line being the
cosmetic `LOCALVERSION`).

Why this matters mechanically: `is_cpu_preempted()`'s heartbeat
(`rq->clock_preempt`) is only refreshed once per scheduler tick. At HZ=1000
(1ms tick), a staleness threshold in the 1-6ms range is sane. At HZ=250 (4ms
tick), the same threshold reads "stale" on a perfectly healthy, busy CPU most
of the time purely from sampling phase, independent of any real host
preemption — this is exactly the false-rejection bug spent hours fixing
(300µs -> 1.5ms -> 6ms, see the BPF file's own inline comments for the full
experimental history). The coarser tick also degrades load-balancing
responsiveness generally, which is why `cache_nice_tries`, ivh_eval_cooldown_ns`,
and `rseq_sched_extend_usec` all needed hand-tuning on HZ=250 to claw back even
a modest result — none of that tuning was a fix to a real IVH bug, it was
compensating for the wrong tick rate.

**Status of that fix stack now that HZ=1000 is confirmed correct:** untested.
It was derived and tuned entirely under HZ=250. It's plausible some or all of
it is now unnecessary, redundant, or even counterproductive under HZ=1000 —
this needs to be re-validated once 43-1000+ is confirmed reproducing the
original 34+ result (next step 1, below).

## Current `MY_ivh_atc.bpf.c` state on this branch

Gates: `LOCKHOLDER=1, SPINNER=0, CAPACITY_LOW=1, NOT_BETTER=1, PREEMPTED=1,
BURST_ORDER=0, BURST_BUDGET=0`. `IVH_CAP_FLOOR=850` (confirmed well-calibrated
against the live `cpu_capacity` signal — measured stolen-half CPUs averaging
~425-494, clean-half averaging ~1021-1022 out of 1024 max).

`is_cpu_preempted()` staleness threshold: `6000000ULL` (6ms). This was tuned
under HZ=250 and needs re-validation under HZ=1000 — the original rationale
(1.5x the tick period) would suggest ~1.5ms is more appropriate now, but this
has not been tested.

`GATE_LOCKHOLDER` was fixed to check the target's rseq critical-section
counter via `bpf_probe_read_user()` instead of kernel `lock_depth` — the old
check was **always 0** for a userspace lock like `NHextend2`'s, meaning this
gate never actually protected the holder in any prior session. This is a
correctness fix independent of the HZ finding and should be kept regardless of
what else changes.

A `cap_sum`/`cap_cnt` instrumentation pair exists for sanity-checking live
`cpu_capacity` per candidate CPU (`bpftool map dump name cap_sum` /
`cap_cnt`) — harmless to leave in, useful for any future capacity-floor
question.

An unused `ivh_rr_cursor` global and its associated (currently dead)
rotating-search-start code remain in the file for reference — a rotating
destination-search-start experiment measured *worse* (thundering-herd was not
actually the driver of holder-preemption cost) and was reverted, but the
cursor variable itself was left in place with a comment explaining why. Safe
to delete in a future cleanup pass; not currently wired into anything.

## Next steps, in order

1. **Confirm `43-1000+` actually reproduces the original 34+ result.** Run the
   standard A/B (`NHEXTEND_DURATION=8 /home/nick/NHextend2 -n -l 16` vs
   `NHEXTEND_DURATION=8 /home/nick/ivh_exec /home/nick/NHextend2 -n -l 16`,
   sysctls `ivh_capacity_threshold=1010`, `ivh_time_left_threshold_ns=4000000`,
   `ivh_max_concurrent=8`) for 2-3 rounds. This has been spot-checked once
   already (11.4% -> 7.35% host-preempted, -11% iterations) on the *previous*,
   HZ=250 `43` build — it has **not yet been re-tested on the HZ=1000
   rebuild**. This is the single most important thing to check before anything
   else in this list.
2. **Re-evaluate the HZ=250-era fix stack under HZ=1000**: `cache_nice_tries`
   (debugfs, was 1->10000), `ivh_eval_cooldown_ns` (was 50000->0),
   `rseq_sched_extend_usec` (was 50->4000), and the `is_cpu_preempted()`
   threshold itself (currently 6ms, was tuned against a 4ms tick — try 1.5ms
   first given HZ=1000's 1ms tick). Test each in isolation against the
   HZ=1000 baseline from step 1 — some or all may now be unnecessary or
   actively wrong.
3. **Once a clean HZ=1000 baseline is confirmed, re-add the toggleable
   mechanisms one at a time**, per the plan already agreed: start with
   `stop_one_cpu()`-based dispatch (vs. the original
   `set_cpus_allowed_ptr()+schedule()`, which has a documented lost-wakeup
   hang class) and trylock-vs-blocking for the destination-search lock. Test
   each individually against the HZ=1000 baseline rather than assuming
   HZ=250-era conclusions about them still hold — several already-tested
   conclusions from tonight (e.g. "trylock doesn't matter", "dispatch
   mechanism doesn't matter") were reached while HZ=250 was confounding
   everything and deserve a second look.
4. Only after 1-3: revisit the TOCTOU concurrency-cap fix, veto
   instrumentation, and Gate 2 formula (`ewma_act_ns` vs `last_active_time`) —
   these were "confirmed good" or "neutral" under HZ=250 and are lower
   priority to re-litigate, but shouldn't be assumed identical under HZ=1000
   without at least a spot check.
5. Hotlock (post-lock observability) does not exist on this branch's line of
   history at all (it was committed on `rseq-port`, which is not an ancestor
   of `kernel-43-clean`/`ivh-toggle-matrix`). If it's wanted again, it needs
   to be cherry-picked or reimplemented from `rseq-port`, not assumed present.

## Standing operational notes (unchanged, still apply)

- Sysctls (`ivh_capacity_threshold`, `ivh_time_left_threshold_ns`,
  `ivh_max_concurrent`, and any of the fix-stack sysctls in step 2 above) reset
  to compiled defaults on every reboot. `tools/bpf/scripts/ivh_apply_optimal.sh`
  applies the known HZ=250-era stack — **do not run it as-is against a
  HZ=1000 boot without re-deriving the values per step 2 first.**
- Never modify `/proc/vcap_info`'s output format — frozen at exactly 4 lines
  per CPU, the external `vcap` binary has a hardcoded parser that crashes on
  any change.
- Always check `pgrep -fa "MY_ivh_atc$\|vcap -p"` before starting either;
  never end up with two simultaneous instances of either (confirmed to cause
  double-BPF-hook-firing corruption).
- Test one change at a time, manually, reading full output before the next
  step — this project has been burned before by unattended sweep scripts
  leaving stale parameter values in place.

## Verdict on existing docs in this directory

- **`ivh_state_of_art.md`** (2026-06-26) — architecture description (gates,
  tiers, the `ivh_pre_lock()` entry point, the recursion guard, the JIT
  banlist crash fix) is still structurally accurate for how the mechanism
  works today. Specific tuning numbers in it (capacity threshold 900/500,
  time-left default 500000ns) are stale — this branch runs 1010/4000000.
  **Keep for architecture, don't trust its numbers.**
- **`lock_and_wait.md`** (2026-06-21) — a coverage audit of which kernel
  locking primitives `lock_depth`/`wait_depth` do and don't track
  (`bit_spin_lock`, `mutex_spin_on_owner`, etc.). This is about kernel
  internals that haven't changed and isn't superseded by anything since.
  **Still valid, worth reading if extending lock/wait tracking to new code
  paths.**
- **`MY_ivh_atc_reference.md`** (2026-06-18) — describes the *old*,
  tick-path-based lockholder detection architecture
  (`running_migration()`/`cfs_sched_tick_end`) that was replaced by the
  synchronous pre-lock design `ivh_state_of_art.md` documents.
  **Outdated, already explicitly superseded** (per `ivh_state_of_art.md`'s
  own note).
- **`experiment_plan.md`** (2026-06-11) — the original implementation report,
  predates pre-lock migration entirely ("threshold-sweep experiments... have
  not yet begun"). **Outdated, already explicitly superseded**, same note as
  above.
- **`session_handoff.md`** (2026-06-27) — describes a hang investigation
  (trylock experiments, `schedule()` hang hypothesis) whose open questions
  were later resolved differently (the project moved to `stop_one_cpu()`-based
  dispatch specifically because of this hang class). Its "next steps" are
  stale and already acted on. **Outdated.**

Net: only `ivh_state_of_art.md` (architecture) and `lock_and_wait.md`
(coverage audit) are worth reading in a new session, and this document should
be read first regardless since it has the current numbers, the HZ finding,
and what to actually do next.
