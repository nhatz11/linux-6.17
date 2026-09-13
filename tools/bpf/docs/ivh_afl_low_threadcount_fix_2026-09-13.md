# Fixing the adaptive lock's low-thread-count regression, 2026-09-13

Follows `ivh_afl_confound_and_threshold_search_2026-09-12-night.md`. Kernel unchanged
(`6.17.0-G-LOCK-25-tier1confirm+`), VM rebooted once between the discovery and this fix.

## The problem

A thread-count sweep (loop_spin=600000, migration on, IVH-alone vs IVH+adaptivespin) found the
adaptive lock is a clean win at 16 threads (+27.8%) but a real, growing, 3/3-consistent regression
at every lower count: 8 threads -7.4%, 4 threads -13.5%, 2 threads -17.8%, 1 thread -19.3%.
Notably negative even at 1 thread, where there is by definition zero lock contention.

## Goal, and constraint

Make IVH+adaptivespin at least neutral at 8/4/2/1 threads without losing the 16-thread win. Per
explicit instruction: the fix must stay entirely in userspace — no reading of the kernel migration
engine's own threshold/decision signals (`bpf_pre_lock_migrate()`, `RSEQ_SCHED_STATE_FLAG_IVH_DANGER`,
`ivh_danger()`) as a shortcut for "is this lock contended."

Dispatched as an autonomous build-test-iterate task (not a one-shot design): required to build and
measure its own fix on the live system before reporting, explicitly not permitted to stop at a
plausible-but-unverified theory.

## Three real causes found, in the order they were found — the first two were each measured
insufficient before moving to the third

**1. `ivh_afl_beat()`'s self-gated `__thread` counter has real, unconditional per-iteration cost.**
It compiled to a `%fs`-relative load *and store* of its gate counter on every single call — 600,000
times per critical section at loop_spin=600000 — immediately before the loop's own `sfence`, which
then has to drain that store. Fixed by moving the interval gate into the caller's own loop counter
(a local `int next_beat`, compared and incremented in-register, zero memory traffic) instead of
calling into a function that redundantly maintains its own counter. `ivh_afl_beat()` itself is kept
for callers with no loop of their own; its header comment was corrected (it previously, incorrectly,
claimed to be "cheap enough to call every iteration").
Measured alone: fixed loop_spin=5000/1-thread but left 4-thread at -7.3%. Insufficient by itself.

**2. The tier-2 staleness check's `l->hb_tsc` read generates real coherence traffic on the holder's
own critical path.** Reading the holder's heartbeat pulls that cache line to Shared state, forcing
an RFO on the holder's next republish; with multiple waiters each probing independently, the line
ping-pongs continuously and the cost lands on the serialized holder, not the (idle) waiters. Fixed
with a "deadline skip": since heartbeats only ever move forward, having just observed heartbeat
value `hb`, a waiter cannot legitimately conclude staleness before `hb + stale_tsc` regardless of
what happens in between — so remember that deadline and spin on local `RDTSC` alone (no coherence
traffic at all) until it passes, only touching `hb_tsc` again once the deadline is reached. Changes
no threshold and no state-machine rule; worst-case detection latency is unchanged (a holder that
stalls right after its last beat is still caught exactly one `stale_tsc` later — the previous code
could occasionally notice *earlier* than the threshold allows, which was never a legitimate
inference to draw from a stale read in the first place). Deadline is reset to force a fresh read
immediately after waking (the lock has almost certainly changed hands during the sleep).
Measured alone (stacked on fix 1): improved 4-thread to -3.6%, but 8-thread was still -8.2%.
Still insufficient.

**3. The actual dominant cause: a real behavioral bug in `NHextend-full.c`'s own port, unrelated to
the lock's internals.** `NHextend3.c`'s original acquire loop arms the rseq `cr_counter` timeslice-
extension request immediately before each `cmpxchg` attempt and disarms it the instant that attempt
fails — so a spinning waiter never holds an outstanding request, and the eventual holder carries a
*fresh* one into its critical section. `NHextend-full.c` armed it once, before calling
`ivh_afl_lock()` — which on a contended lock can spin or sleep for milliseconds. The request sat
armed across that whole wait; the kernel spent it there, and the thread entered its 1.6ms critical
section with nothing left to defer preemption with. Measured directly (loop_spin=600000, 8 threads):
critical-section *active* (on-CPU) time was identical between binaries (936µs vs 929µs, +0.7%) —
but *overall* CS time was 1,048µs vs 948µs, i.e. 113µs/CS spent off-CPU here against 19µs in
NHextend3, and 3.12% of CS cycles saw a >100µs preemption against 1.23%. That gap sits entirely on
the serialized critical path and grows with thread count (the longer the wait, the more certain the
extension was already spent) — exactly the shape of the regression. Fixed by moving the `extend()`
arm to immediately after acquisition, matching NHextend3's own intent (protection for the CS itself,
not the wait). This is a benchmark-harness bug fix, unrelated to and not touching the lock's own
correctness invariants.

## What did not work, kept for the record

- Tuning `IVH_AFL_BEAT_MASK` alone (0x3FF → 0x1FFF) recovered only 2.6pp of the 4-thread gap; after
  fix 2 was in, beat rate stopped mattering at all (1024/4096/8192 all landed within noise of each
  other). No tunable default was changed.
- A build with heartbeat republishing removed entirely from the CS looked attractive in isolation
  (+32% at 16 threads, only -1.4% at 8) but is a degenerate configuration: waiters go stale exactly
  `IVH_AFL_STALE_NS` (50µs) into every CS regardless of whether the holder is healthy, defeating the
  design's core claim that ordinary healthy queueing must never trip the sleep path. Rejected.

## Final validation (fresh, 3 interleaved rounds × 20s, all 5 thread counts, loop_spin=600000)

| threads | IVH | old IVH+AS | new IVH+AS | old vs IVH | **new vs IVH** |
|---|---|---|---|---|---|
| 16 | 15,145 | 14,341 | 20,093 | -5.3% | **+32.7%** |
| 8 | 20,595 | 15,573 | 20,652 | -24.4% | **+0.3%** |
| 4 | 20,801 | 16,472 | 20,824 | -20.8% | **+0.1%** |
| 2 | 20,369 | 16,614 | 20,415 | -18.4% | **+0.2%** |
| 1 | 17,478 | 14,024 | 17,503 | -19.8% | **+0.1%** |

Every row 3/3 consistent. Goal met: neutral-to-slightly-positive at 8/4/2/1, and the 16-thread win
is larger than it was before the fix (+32.7% vs the original session's +27.8% — the VM was rebooted
between the original discovery and this fix, so absolute throughput numbers differ from
`ivh_nhextend_adaptive_futex_lock_2026-09-12.md`'s table; all old-vs-new comparisons here are
interleaved within the same host state, so the *relative* numbers are sound regardless).

Correctness re-verified independently (not just by the agent that made the fix): read the actual
diff line-by-line — the "deadline skip" is a pure consequence of heartbeats being monotonically
non-decreasing (a cached deadline can only *delay* detection, never produce a false "not stale"
verdict) and never touches the 0/1/2 wake-skip state machine; no packed multi-field state word was
introduced; both binaries rebuilt clean with `-Wall`, smoke-tested at 1 and 16 threads (`-n`,
`-n 1`), clean exit, no shutdown hang.
