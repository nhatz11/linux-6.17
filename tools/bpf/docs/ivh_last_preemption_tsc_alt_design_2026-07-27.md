# Alternative design: TSC heartbeat staleness as the `last_preemption` signal

Status: **not implemented**. This is a design note capturing an alternative
to the fix Opus already built and compiled clean (see
`kernel/sched/cputime.c`, `ivh_account_preemption_event()`, 2026-07-27). Not
yet decided which of the two should ship.

## The gap this addresses

`rq->last_preemption` / `rq->last_active_time` / `rq->preemptions` /
`rq->max_latency` (written in `steal_account_process_time()`,
`kernel/sched/cputime.c`) were 100% derived from real
`paravirt_steal_clock()` regardless of `ivh_steal_source`, even though the
steal *magnitude* fed to vcap (`get_steal_and_preemptions()`, `core.c:261`)
was already switchable. `last_preemption` is Gate 2's trigger term
(`ivh_steal_imminent()`, `fair.c:13214`), so this was the one remaining real
PV dependency in the actual migration decision path.

## Opus's shipped fix (for contrast)

New function `ivh_account_preemption_event()` (`cputime.c:251-402`+): at
`ivh_steal_source=1`, computes a per-tick delta against
`rq->ivh_ref_steal_ns` (the already halt-corrected, already carry-corrected
REF_TSC inference from `ivh_ref_accumulate()`, `core.c`) instead of against
real `paravirt_steal_clock()`. Reuses the existing corrected signal rather
than deriving a fresh one, specifically to avoid reintroducing the
lock-path-halt phantom-steal bug (the ~90-100x bimodal bug from earlier
2026-07-27 work). Known caveat: inherits `ivh_ref_carry=0`'s 63-74%
under-load under-report bias unless `ivh_ref_carry=1` is also set.

## The alternative: piggyback on the TSC heartbeat's own publish site

Idea (from conversation, not yet built): instead of consuming
`ivh_ref_steal_ns` at all, use the per-CPU TSC heartbeat
(`struct ivh_tsc_beat`, `arch/x86/include/asm/ivh_tsc_beat.h`) itself as the
preemption-event detector. Every time a CPU is about to overwrite its own
heartbeat stamp (`ivh_tsc_beat_publish()`, called from the tick in
`account_process_tick()`, `cputime.c:530`), check the age of the stamp it is
about to overwrite:

```
age = now - old_stamp
if (age > threshold && old_stamp > rq->last_idle_tp):
    # genuine preemption event, not idle
    rq->last_preemption = now
    steal_estimate = age        # for last_active_time's arithmetic
rq->stamp = now                 # normal publish, unchanged
```

### Why this is structurally better than reusing REF_TSC

The REF_TSC-based signal (`ivh_ref_steal_ns`) sees a lock-path `safe_halt()`
as lost time, because REF_TSC genuinely stops counting during any HLT --
that is the whole reason `ivh_ref_halt_correct` had to exist. The
tick-based heartbeat publish does NOT have this problem: mechanism 0/2's
`safe_halt()` halts with `IF=1`, so the LAPIC timer still fires on schedule
during the halt and still runs `account_process_tick()` -- republishing the
stamp -- while the halt is still in flight (this is exactly the case
`ivh_lock_halt_flush()` exists to handle on the REF_TSC side, `core.c:639`).
So a lock-path halt does not create a gap in the tick-published heartbeat at
all. A staleness check at that publish site would never misattribute a
lock-path halt as a preemption event -- no correction analogous to
`ivh_ref_halt_correct` is needed for this design.

### The one real gotcha: nohz idle

Unlike a halt, genuine CPU idle deliberately *stops* the periodic tick
(`tick_nohz_idle_enter`) and does not reprogram it until there is real work
again, so the gap between heartbeat publishes can be arbitrarily long on a
legitimately idle CPU. A naive staleness check would misread every
idle-to-active transition as a preemption event. This is the exact blind
spot already documented in `cputime.c`'s existing Phase-1 heartbeat-write
comment ("an idle vCPU's stamp ages without bound and reads as preempted...
fixes: (a) publish from idle entry/exit, or (b) take
max(beat, rq->last_idle_tp)"). `rq->last_idle_tp` already exists and is
already updated at idle exit (`cputime.c:233`), so the fix is cheap: only
treat staleness as a real preemption if `old_stamp > rq->last_idle_tp`,
i.e. the gap is not explained by the CPU having legitimately been idle.

### The remaining open question

`last_active_time`'s existing formula needs a magnitude (`steal`), not just
a yes/no. Under this scheme the natural estimate is
`age = now - old_stamp` at the moment staleness is detected -- same shape
`is_cpu_preempted()` already uses, just reusing the heartbeat's own stamp
instead of a separate `clock_preempt` field. Not yet validated whether this
magnitude estimate is accurate enough for `last_active_time`'s arithmetic,
or whether it needs its own correction term.

### Why this is worth doing over the shipped fix

- Simpler: no dependency on `ivh_ref_steal_ns`'s REF_TSC perf-event
  machinery, no `ivh_ref_carry` bias to inherit.
- Unifies both TSC consumers around one primitive (the heartbeat), instead
  of consumer #3's magnitude path and this fix using two different TSC
  mechanisms.
- Structurally immune to the halt-phantom bug class without needing a
  correction knob.

Next step if pursued: send back to Opus to implement and validate against
`/proc/vcap_info`'s `preempts` field the same way the shipped fix was
validated (confirm it advances under load, doesn't freeze, and doesn't spike
on idle-to-active transitions).
