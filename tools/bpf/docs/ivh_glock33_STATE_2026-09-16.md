# G-LOCK-33 eviction: exact state, 2026-09-16 (handoff)

Kernel `6.17.0-G-LOCK-33-evict+` **#47**, branch `ivh-rebuild-main`.
Fallback that works: `6.17.0-G-LOCK-30-csfast+` (grub `saved_entry`).

## THE OPEN BUG (this is the only thing blocking everything else)

**Eviction leaks `pv_hash` entries.** Reproduced and quantified 2026-09-16:

| arm | `ivh_pv_hash_live` after a 5 s hackbench |
|---|---|
| PV | **0** (balanced) |
| EVICT | **10** |
| EVICT again | **17** (+7) |

~7-10 entries leaked per run, monotonic. 256 slots => `pv_hash()`'s bare
`BUG()` after ~25-35 runs. That BUG is the crash seen three times:

    Oops: invalid opcode: 0000 [#1] SMP NOPTI
    RIP: 0010:pv_hash+0xd5/0xe0

**Critically: the leak occurs with `strand=0, lockups=0, averted=0`.** So it is
NOT "a stranded waiter holds an entry" -- that theory is dead. Entries leak
directly. Narrower bug, easier to find.

### How to reproduce in 60 seconds
    /root/linux-6.17/cvm_setup/goto_mode.sh pv kernel     # REQUIRED after every boot
    /root/ivh_tools/evict/arm.sh pv    ; hackbench -T -g1 -f8 -l50000; python3 /root/ivh_tools/evict/gauge.py
    /root/ivh_tools/evict/arm.sh evict ; hackbench -T -g1 -f8 -l50000; python3 /root/ivh_tools/evict/gauge.py

### Where to look next
Entries are created in exactly two places and released in exactly two places:
- create: `pv_kick_node()` (promoting a HALTED node) and `pv_wait_head_or_lock()`
- release: `pv_unhash()` (unlock slowpath) and the direct
  `WRITE_ONCE(*lp, NULL)` in `pv_wait_head_or_lock()`

The invariant (`pv_hash()`'s own comment): *every blocked lock only ever
consumes a single entry*, and *the lock owner unhashes before it releases*.
So the bug is either (a) the same lock hashed twice without an intervening
unhash, or (b) `lock->locked`'s `_Q_SLOW_VAL` being lost so the unlock takes
the fast path and never unhashes. **Suspect (b) first**: `pv_kick_node()`
writes `_Q_SLOW_VAL` with a plain `WRITE_ONCE`, and the requeue path's
`queued_spin_trylock()` writes `_Q_LOCKED_VAL` over `lock->val`.
Instrument both create sites with separate counters and diff them against the
two release sites -- that localises it in one run.

## WHAT IS ALREADY FIXED AND PROVEN
- `acec38328f90` halt-clobber: `pv_wait_node`'s halt is now
  `try_cmpxchg(RUNNING -> HALTED)`, so it cannot overwrite a committed
  `VCPU_SKIPPED`. `ivh_evict_halt_averted` fired 3x in real runs => the race
  was real and is caught. **Invariant: every transition out of VCPU_RUNNING on
  a queued node must be a cmpxchg, never a plain store.**
- `ivh_pv_evict_hop_cap` (default 1) caps evictions per handoff.
- `30f73e556305` leak gauge: balanced across BOTH release sites, and NO printk
  anywhere in the locking path (a WARN in `pv_hash()` deadlocked boot, #46).

## MEASUREMENTS THAT STAND
- Eviction vs its own control (tier2 off, evict off), n=6 paired, l=100000:
  **-7.2%, t=-3.85 => significant.** Eviction's own contribution is real.
- Eviction vs PV: **NOT ESTABLISHED.** Best data 5 runs, PV 13.23 vs EVICT
  13.13 -- a wash at n~2. Every longer attempt crashed on the leak first.
- Control vs PV: +3.8%, t=+1.40, ns (cost of running tier2 off).
- PV reference, l=400000, cap 585->516: **103.56 s**.

## DO NOT REPEAT THESE
- **tier2 + eviction are ANTAGONISTIC** (user's insight, confirmed by data:
  `stop_halted` outnumbers `acted` 2.2:1). tier2 halts the very waiters
  eviction is allowed to act on. Combining them crashed the VM. Keep
  `ivh_pv_tier2_enable=0` in the eviction arm.
- Short hackbench runs do NOT scale linearly: `-l20000` x20 predicted 51 s
  where the real `-l400000` took 103.56 s. Use short runs for safety checks
  only, never for performance.
- `RIP: 0033` in a lockup = userspace, guest starved by the host. At 40-70%
  steal **stock PV also trips soft lockups**, including with kernel RIPs in
  `__pv_queued_spin_lock_slowpath`. Not a bug signature on its own.
- After a crash, reconstruct the arm from `journalctl -b -1` (the
  `adaptive_mode=2` line is the arm marker). Post-reboot sysctls are boot
  defaults and say nothing about the crash.

## HARNESS (durable, survives reboot)
`/root/ivh_tools/evict/`: `arm.sh <pv|control|evict>`, `ab.sh` (ABBA, every run
`timeout`-bounded, aborts on strand), `capsnap.py`, `gauge.py`.

## UPDATE, 08:50 -- leak localisation instrumented, tax identified

### The leak is 100% eviction-specific (proven)
`ivh_pv_hash_live` moves ONLY on eviction runs. Across every PV run it is
**flat**: 17->17, 70->70, 70->70, 168->168. Eviction adds +15..40 per run.
It occurs with `strand=0, lockups=0, averted=0`, so the "stranded waiter holds
the entry" theory is DEAD -- entries leak directly.

### Next step is a subtraction, not a search
Kernel commit `ed5b3feee98d` instruments all four sites. **Needs build +
reboot.** Then one short eviction run and:

    python3 /root/ivh_tools/evict/hashacct.py

`ins_kick + ins_head - rel_unhash - rel_lp` must equal the live gauge; the
create site with no matching release is the leak. Five attempts to find this by
reading the code were each wrong -- do not reason, measure.

### THE TAX (answers "something expensive that isn't eviction")
`preempt_src=2` makes `pv_init_node()` call `ivh_tsc_beat_publish()` on EVERY
contended queue entry: an rdtsc plus a store to a cacheline other CPUs read
remotely. PV skips it entirely. Paid by 100% of acquisitions; eviction acts on
0.05-0.46%. Prime suspect for control's +3.8% over PV.

Arithmetic from the n=6 paired data: **PV 100, CONTROL 103.8, EVICT 96.3.**
Eviction's own contribution is real (-7.2%, t=-3.85) but it first has to pay
back the ~3.8% infrastructure tax, so net vs PV is ~-3.6% and not significant.

`arm.sh control_nobeat` (new) prices the tax exactly: identical to control but
`preempt_src=0`. control_nobeat vs control = the tax; control_nobeat vs pv =
what adaptive_mode=2 + tier1 cost alone. Eviction cannot run there (its gate
requires preempt_src=2), which is the point.

### Live sysctl knobs to sweep once the leak is fixed (no rebuild)
- `ivh_pv_beat_publish_mask` (4095): higher = publish less often = less
  coherence traffic, but staler heartbeats.
- `ivh_pv_beat_threshold` (220000 cyc): controls how readily a waiter reads as
  preempted, i.e. the eviction rate.

### Latest A/B (contaminated, do not cite)
PV 9.76 mean; EVICT 13.0 (10.34 excluding a 20.95 s outlier), 5 new lockups.
The outlier and the lockups ARE the leak showing up inside the measurement.
**No PV-vs-EVICT number is trustworthy until the leak is fixed.**

### Machine hygiene
The gauge was left at **168/256**; a reboot clears it. Eviction BUGs at 256.
`skip_point=0` throughout -- eviction runs at PROMOTION, not at unlock; the
G-LOCK-32 deferral/hash path never executes.
