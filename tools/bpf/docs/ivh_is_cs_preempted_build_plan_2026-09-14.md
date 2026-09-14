# `is_cs_preempted()` — adaptive spinning for the queue head, staged build plan
### 2026-09-14. Nothing here is built. No patch, no build, no reboot, no benchmark.

Target tree: `/root/kernels/linux-6.17-vanilla`, branch `ivh-rebuild-main`, HEAD
`f8c10aaf1ba3`. Running kernel `6.17.0-G-LOCK-28-rotate+` matches it
(`CONFIG_LOCALVERSION="-G-LOCK-28-rotate"`).

---

## 0. Corrections to the premises this plan was commissioned on

Four of them. Each changes something concrete; none kills the idea.

### 0.1 This feature already exists on paper, under this exact name, and a third of it is already merged

`include/linux/ivh_lock_holder.h:14-23` says, verbatim:

> `is_cs_preempted()` cannot be asked "is the holder preempted?" until something
> first records WHO the holder is, and that is the entire job of this file.

The full prior specification is `ivh_tsc_full_redesign_build_plan_2026-07-29.md`
§1.2, §3.2, §3.3.4 — including an enumerated ownership-transfer site table
(A1–A9) and release table (R1, R2, R2b, R3, R4), and two predicate *forms*. What
is already **merged and compiled** on `ivh-rebuild-main`:

| thing | where | state |
|---|---|---|
| `struct ivh_holder_slot` side hash table, 65536 × 64 B = 4 MB | `arch/x86/kernel/ivh_lock_holder.c:33-137` | allocated at `late_initcall`, **inert** |
| `ivh_lock_set_holder()` / `_clear_holder()` / `ivh_lock_holder_cpu()` | `include/linux/ivh_lock_holder.h:116-143` | gated on `ivh_lock_holder_enabled` |
| `ivh_lock_holder_enabled` | `arch/x86/kernel/ivh_lock_holder.c:41` | `0`, **and there is no sysctl to arm it** — confirmed against live `/proc/sys/kernel/` |
| stamp site **A1** (uncontended fastpath) | `include/asm-generic/qspinlock.h:151` | present |
| stamp site **A2** (`queued_spin_trylock`) | `include/asm-generic/qspinlock.h:121` | present |
| stamp site **A9** (TAS virt fallback) | `arch/x86/include/asm/qspinlock.h:384` | present, runtime-dead |
| clear sites **R1 / R2 / R2b** | `include/asm-generic/qspinlock.h:180`, `arch/x86/include/asm/qspinlock.h:90`, `:140` | present; **R2b is the only live one** |

Sites **A3–A8 were never added**. So the merged table stamps the *uncontended*
acquisitions and misses every *contended queue-head* acquisition — precisely the
ones this feature needs. It is not merely unarmed, it is incomplete for this
purpose.

**This plan does not use that table.** The `prev`-plumbing route is strictly
better here: no hash, no collisions, no `ivh_holder_bits` sweep, and — decisively
— no stamp on `queued_spin_lock()`'s uncontended fastpath. Recommendation:
leave `ivh_lock_holder.c` allocated (4 MB of `vzalloc`, zero runtime cost, and
deleting it is a separate change with its own risk) and add a one-line pointer
from its header to this document.

### 0.2 Today's design doc reaches the opposite conclusion, because its role table has a hole

`ivh_head_waiter_adaptive_spinning_design_2026-09-14.md` §1 tabulates three
waiter roles and concludes the identity problem is "a data-structure fact, not a
tuning gap". That table lists role B ("first thread at `queue:`, `xchg_tail()`
returned no prior tail") and role C ("second and later queued waiters"), but it
does **not** have a row for *a role-C waiter that has been promoted to queue
head*. That promoted waiter is the common case, it holds a live `prev`, and its
`prev` is the lock holder. §1's conclusion is correct for role A and role B and
wrong for the promoted role-C head, which is where all the traffic is.

That doc's §3 Option 1 (delegate detection to the successor) and this plan are
**not** alternatives — Option 1 rescues a stalled *head*, this rescues a stalled
*holder*. They compose. But note that Option 1's own open question ("measure the
duration before building the action") is exactly the discipline §5 below imposes
here.

### 0.3 The prior plan's "form 1" predicate is broken for the reason the brief gives, and the brief's fix is genuinely new

`ivh_tsc_full_redesign_build_plan_2026-07-29.md` §1.2 offers:

- **form 0**: `cs_stamp != 0 && (rdtsc() - cs_stamp) > threshold` — measures CS
  *duration*, not preemption. The doc says so itself.
- **form 1** (its recommendation): `cs_stamp != 0 && ivh_beat_stale(holder_cpu)`.

Form 1's argument is *"the tick is a hardirq and fires regardless of
`preempt_disable()`, so `ivh_tsc_beat.stamp` stays fresh on a running CPU even
while it holds a spinlock"*. True — but "fresh" for a **holder** means at tick
cadence, 1 ms, whereas `ivh_beat_stale()` compares against
`ivh_pv_beat_threshold`, which is **220 000 cycles = 100 µs live**
(`/proc/sys/kernel/ivh_pv_beat_threshold`, set by `spin_mode 2`). A perfectly
healthy holder reads stale for ~90 % of every tick period. Form 1 is a false
positive generator in the tuned configuration. The doc missed this because it
reasoned against the *compiled* default of 3 300 000 cycles = 1.5 ms
(`arch/x86/kernel/kvm.c:1395-1396`), at which the argument does hold.

**This is a live trap.** `ivh_pv_beat_threshold` resets to 3 300 000 on every
reboot and is only moved to 220 000 by `spin_mode 2`/`4`. Anything that reads it
gets a different answer before and after `spin_mode` runs. The predicate
specified below deliberately **does not read `ivh_pv_beat_threshold` at all**.

The brief's predicate — call it **form 2** — is the correct fix and supersedes
both:

```
held_for    = now - lock_acq_tsc
missed_tick = holder_beat < lock_acq_tsc     /* no beat since acquisition */
fire        = held_for > OWED_TICKS * TICK_PERIOD && missed_tick
```

It is immune to long healthy critical sections by construction, and its
threshold is a property of `HZ` and `tsc_khz`, not of a tuned staleness knob.

### 0.4 `CONFIG_NO_HZ_FULL=y` is real but runtime-inert on this boot — and the brief's assumption about *why* is wrong

The brief asks whether a spinlock holder can be on a tickless CPU. **Yes, in
general, and not only on the return-to-userspace path.** `Documentation/timers/
no_hz.rst:139-142`:

> "Normally, a CPU remains in adaptive-ticks mode as long as possible. **In
> particular, transitioning to kernel mode does not automatically change the
> mode.**"

Mechanism: the only caller of `tick_nohz_full_update_tick()` is
`tick_nohz_irq_exit()` (`kernel/time/tick-sched.c:1295`), reached from
`tick_irq_exit()` (`kernel/softirq.c:639-650`) whose only context gate is
`!in_hardirq()` — which tests `HARDIRQ_MASK` and says nothing about
`PREEMPT_MASK`. `can_stop_full_tick()` (`kernel/time/tick-sched.c:358-375`)
checks six dependency bits (POSIX_TIMER, PERF_EVENTS, SCHED, CLOCK_UNSTABLE,
RCU, RCU_EXP) and has **no** `preempt_count()` check and no lockdep check.
So: task takes `raw_spin_lock()` → any IRQ lands → `__irq_exit_rcu()` drops
`HARDIRQ_OFFSET` → tick is stopped → the CPU returns to kernel code still
holding the lock, publishing nothing. Nothing restarts it: `tick_nohz_full_kick()`
fires only from `tick_nohz_dep_set*()`, and taking a spinlock sets no dependency
bit.

**But on this boot it cannot happen, for two independent reasons:**

1. `/proc/cmdline` has **no `nohz_full=`**. `tick_nohz_full_running` is set only
   by `tick_nohz_full_setup()` (`tick-sched.c:599-605`) via `housekeeping_setup()`
   (`kernel/sched/isolation.c:177`). Confirmed:
   `cat /sys/devices/system/cpu/nohz_full` → `(null)` (an unallocated
   `tick_nohz_full_mask` printed by `%*pbl`, `drivers/base/cpu.c:304-309`), and
   `dmesg` has no `"NO_HZ: Full dynticks CPUs:"` line.
2. `/proc/cmdline` has **`nohz=off`**. `setup_tick_nohz()`
   (`tick-sched.c:683`) clears `tick_nohz_enabled`, so `tick_nohz_activate()`
   bails at `:1492-1494` and `TS_FLAG_NOHZ` is never set on any CPU — even the
   *idle* tick is never stopped (`can_stop_idle_tick()` returns false at
   `:1172-1174`).

Every one of the 16 vCPUs publishes a beat every 1 ms unconditionally today.

**That is a boot-parameter property, not a code property.** The predicate must
not depend on it. §3.4 specifies an unconditional `tick_nohz_full_cpu(holder)`
guard, which is a NOP-patched static branch on this boot (gated on
`context_tracking_key`, `DEFINE_STATIC_KEY_FALSE_RO`) and therefore free, plus a
counter (`ivh_cs_abstain_nohz`) that must read **exactly 0** in every run — a
nonzero value means someone changed the command line and every number in the run
is suspect.

### 0.5 Two other ways a running CPU goes beat-silent for > 1 tick

Not NO_HZ, and not fixable — they must be named as a known false-positive class:

- **`stop_machine()`**: `kernel/stop_machine.c:232-234` puts every online CPU in
  `local_irq_disable(); hard_irq_disable();` until all 16 rendezvous. Triggered
  by ftrace/kprobe text patching, static-key updates, module load/unload, CPU
  hotplug. Routinely 1–20 ms on a 16-vCPU guest, and it silences *all* CPUs at
  once. Note `hrtimer_forward()` (`tick-sched.c:309`) advances past every missed
  interval in one call, so an N-ms IRQ-off window produces **one** beat, not N —
  the gap equals the full window.
- **printk to the serial console**: this box boots `console=tty1 console=ttyS0`.
  At 115200 baud one 80-char line is ~7 ms; a backtrace is hundreds of ms. This
  is an operational hazard for the A/B in §6, not just for the predicate.

Neither is a correctness bug — the holder genuinely is not making progress we can
wait out — but both must be excluded from measurement runs (§6.4).

---

## 1. The soundness theorem

Everything below rests on this. **Amended 2026-09-14 (second pass).** The first
version of part (c) assumed H always sets the pending bit before `prev` can
release. That is false for every successor that was **halted at handoff**, which
is a normal path, not a narrow race. The corrected theorem has two airtight
cases and one case that is only *bounded*. Of the three ways to close the bounded
case, the only airtight one is the release-side clear, so the clear's call site
now ships in the Stage A build, still off by default (§1.4).

### 1.1 Statement

> **Theorem.** Let H be a queue head in `pv_wait_head_or_lock(lock, node, prev)`
> with `prev != NULL` (it reached the call through `if (old & _Q_TAIL_MASK)` at
> `kernel/locking/qspinlock.c:376`) and `ivh_pv_rot_enable == 0`. Let **W** be
> the moment H's `set_pending(lock)` store (`qspinlock_paravirt.h:1548`) is
> *committed*, i.e. drained from H's store buffer by an explicit `smp_mb()`.
>
> **Case CLR (airtight, any tenure).** If `ivh_cs_owner_clear == 1`, then
> whenever the per-CPU slot of `prev->cpu` has `->lock == lock`, that CPU is
> still holding `lock` and `->tsc` is the TSC of this hold. (A second `->lock`
> read after the beat read closes the read-side window.)
>
> **Case HASHED (airtight, tenure 0, no clear needed).** If H entered with
> `pn->state == VCPU_HASHED` and `READ_ONCE(lock->locked) == _Q_SLOW_VAL` read
> after W, then `prev` holds `lock` for all of H's first spin loop. If the stamp's
> `->lock == lock`, its `->tsc` is this hold's.
>
> **Case RUNNING (NOT airtight, tenure 0, no clear).** If H entered with
> `pn->state == VCPU_RUNNING`, the conclusion holds **unless `prev` completed its
> critical section and released before W**. A promptness gate
> (`rdtsc()_at_W − stamp.tsc ≤ ivh_cs_prompt_cycles`) bounds that window but does
> not close it.
>
> In all other cases (no `prev`; tenure ≥ 1 without the clear; HASHED with the
> witness failing; RUNNING with the gate failing) H **abstains**.

### 1.2 Proof

**(a) `prev` acquired the lock.** H linked behind `prev` at `qspinlock.c:380`
(`WRITE_ONCE(prev->next, node)`) and waited at `:383`
(`arch_mcs_spin_lock_contended(&node->locked)`). The only writer of
`node->locked` is `arch_mcs_spin_unlock_contended(&next->locked)` at `:487`,
executed by `prev` immediately after `set_locked(lock)` at `:462`. So `prev`
**was** the lock owner when it released H. *Whether it still is* when H starts
spinning is exactly what (c) has to establish, and what the first version got
wrong.

**(b) Every MCS-handoff predecessor passes through `:462`.** The three ways to
acquire and then hand off:
- contended queue head → `set_locked()` at `:462` directly;
- PV head via `trylock_clear_pending()` (site A7, `qspinlock_paravirt.h:1552`)
  → `goto gotlock` → returns nonzero → `goto locked` at `qspinlock.c:417` →
  `:452`'s `(val & _Q_TAIL_MASK) == tail` **fails** because H is the tail →
  falls through to `set_locked()` at `:462`;
- PV head via `xchg(&lock->locked, _Q_SLOW_VAL) == 0` (site A8,
  `qspinlock_paravirt.h:1591`) → same path.

The `:453` uncontended `atomic_try_cmpxchg_relaxed` path (site A4) does
`goto release` and never touches `next`, so it can never be an H's `prev`.
Stealers via `pv_hybrid_queued_unfair_trylock()` (site A6, called at
`qspinlock.c:324` and `:352`) also `goto release`. **`:462` is the single site
that covers every predecessor**, and the stamp there is ordered before `:487`
(consequence 3 below).

**(c) After W, nobody but H can acquire.** While pending is set and committed:
- `pv_hybrid_queued_unfair_trylock()` refuses. It requires
  `!(val & _Q_LOCKED_PENDING_MASK)` in its `atomic_read` (`qspinlock_paravirt.h:146`);
- the native pending-bit path is skipped at runtime under PV
  (`qspinlock.c:216-217`, `if (pv_enabled()) goto pv_queue`);
- queued waiters behind H cannot acquire before H.

*Commitment is required, not decorative.* A stealer's `atomic_read` on another
CPU does not see H's pending store while that store is still in H's store buffer.
`set_pending()` is a plain `WRITE_ONCE`, so without a drain a stealer can read
`pending == 0` "after" H logically set it. On x86-64 `smp_mb()` is
`lock addl $0,-4(%rsp)` (`arch/x86/include/asm/barrier.h:53`). That is a
LOCK-prefixed, serialising instruction, so it drains the buffer. The plan
therefore places an explicit `smp_mb()` immediately after `set_pending()`,
**only when `ivh_cs_head_probe` is armed** (§3.6).

**The hole, verified against this tree.** (c) says nothing about releases that
happen **before W**, and there is a common path on which one does:

1. H set `pn->state = VCPU_HALTED` (`qspinlock_paravirt.h:873`,
   `smp_store_mb`) and entered `pv_wait(&pn->state, VCPU_HALTED)` (`:880`). In
   `IVH_MODE_ADAPTIVE` that is `ivh_pv_wait()` → `safe_halt()` with IF=1
   (`arch/x86/kernel/kvm.c:2176-2179`).
2. `prev` acquired, stamped at `qspinlock.c:462`, set `node->locked = 1` at
   `:487`, then ran `pv_kick_node()` (`qspinlock_paravirt.h:916-956`). That
   **does not wake H**. It only does `try_cmpxchg_relaxed(&pn->state, HALTED,
   HASHED)`, `WRITE_ONCE(lock->locked, _Q_SLOW_VAL)` and `pv_hash()`, and its
   comment says so ("Vanilla upstream sends no wake here, on purpose").
3. H stays in HLT until either
   (i) `prev`'s unlock: the `PV_UNLOCK_ASM` `LOCK cmpxchg %dl,(%rdi)` fails on
   `_Q_SLOW_VAL` → `__pv_queued_spin_unlock_slowpath()` → `pv_unhash()` →
   `smp_store_release(&lock->locked, 0)` → `pv_kick(node->cpu)`. The kick is
   **after** the release. Or
   (ii) any other interrupt, most commonly the 1 ms tick, because the HLT was
   taken with IF=1.
4. In case (i), between the release and H's `set_pending()`, `locked == 0` and
   `pending == 0`, so any camper in `pv_hybrid_queued_unfair_trylock()` steals.
5. H wakes. `cmpxchg(&pn->state, VCPU_HALTED, VCPU_RUNNING)` (`:889`) **fails**
   because the state is HASHED. `node->locked == 1`, so H leaves
   `pv_wait_node()` and enters `pv_wait_head_or_lock()` with `waitcnt == 0` and
   `pn->state == VCPU_HASHED`. That is exactly the `lp = (struct qspinlock **)1`
   branch at `:1511`. H then sets pending and spins **against the stealer**.
6. With no release-side clear, `prev`'s slot still reads `->lock == lock`. The
   tag check passes on a stale stamp.

The false positive still needs `missed_tick`. `prev` just ran the unlock slowpath
and so was running, so it fires only if `prev` is host-preempted shortly after
acquiring without ticking. That population is the same size class as the true
positives, so it would **directly pollute** Stage A's duration and
false-positive numbers. Verdict: **the coordinator's report holds.** One
refinement: in case (ii), a tick-woken H enters HASHED while `prev` still holds.
Blanket-abstaining on HASHED would throw those tenures away needlessly, and the
witness below keeps them.

**(c-HASHED) The `_Q_SLOW_VAL` witness is airtight.** On a HASHED tenure-0
entry, after W, H reads `lock->locked`:
- `_Q_SLOW_VAL` can have been written only by `prev`'s `pv_kick_node()` for
  *this* handoff. The only other writers are (1) a *later* `pv_kick_node()`,
  which requires a later MCS handoff, and the next one is performed by H itself;
  and (2) H's own `xchg(&lock->locked, _Q_SLOW_VAL)` at `:1591`, which is skipped
  when `lp == 1` and in any case follows the spin loop. Stealers write
  `_Q_LOCKED_VAL`.
- `_Q_SLOW_VAL` is cleared only by `prev`'s unlock slowpath (`smp_store_release(
  &lock->locked, 0)`).
- So reading `_Q_SLOW_VAL` at time *t_r* > W means `prev`'s release store was
  not yet committed at *t_r*. Any stealer's successful `atomic_read` must follow
  that release, hence follow W, hence see `pending == 1` and refuse. From (c),
  `prev` then holds `lock` until H itself acquires. ∎
- Reading anything else (`0` or `_Q_LOCKED_VAL`) means `prev` released before
  *t_r*. H abstains for the whole tenure (`ivh_cs_abstain_hashed`).

**(c-RUNNING) No equivalent witness exists.** If `pv_kick_node()`'s cmpxchg
failed (H was running at handoff), `lock->locked` holds `_Q_LOCKED_VAL` both
while `prev` holds and after a stealer took over, so the lock byte cannot tell
them apart. The residual race: after exiting `:383`, H is delayed (host
preemption, an IRQ, or just the path to `:1548`). In that gap `prev`'s critical
section finishes, `prev` releases through the asm fastpath, which runs no C code,
and a camper steals before W. The **promptness gate** measures `age = rdtsc() −
stamp.tsc` right after W and abstains if `age > ivh_cs_prompt_cycles`
(`ivh_cs_abstain_late`).

**Is the gate airtight? No.** It shrinks the window to `ivh_cs_prompt_cycles`,
default 20 000 cycles ≈ 9 µs, tuned from `ivh_cs_prompt_hist` (§3.6). It cannot
close it, because a critical section shorter than the bound can finish inside it.
Hot spinlocks routinely have sub-µs holds, so short holds are common. For a false
positive, *all* of these must happen:
1. `prev`'s hold was shorter than the gap;
2. a steal landed inside the gap;
3. `prev` was then host-preempted and published no beat for
   `ivh_cs_owed_ticks` periods after its stamp.

That is rarer than the HASHED hole, but it is the same population class. **Only
the release-side clear closes it** (Case CLR).

**(d) The stamp, if it names `lock`, is current.** In each airtight case `prev`
holds `lock`, so it has not re-stamped `lock` since `:462`. If it nested into an
inner lock M it stamped `{M, …}`, and the `->lock != lock` test abstains. In
Case CLR the clear at R2b (`arch/x86/include/asm/qspinlock.h`
`queued_spin_unlock()`) commits `->lock = NULL` *before* the releasing store.
Under TSO, a reader that sees `->lock == lock` therefore read before the release
was committed. The second `->lock` read after the beat read makes the whole
`{lock, tsc, beat}` observation fall inside the hold. ∎

### 1.3 Consequences for the design

1. **One stamp site suffices**: `qspinlock.c:462`. Not the uncontended fastpath.
2. **The stamp's ordering is free.** The stamp at `:462` is a plain store after
   another plain store, and `:487`'s `arch_mcs_spin_unlock_contended()` is an
   `smp_store_release`. Under x86-TSO the stamp is ordered before the successor
   can observe `node->locked == 1`. (This is the store→store argument, *not*
   "the locked RMW fences it". `set_locked()` is a plain `WRITE_ONCE`,
   `kernel/locking/qspinlock.h:196-199`, which is the correction
   `ivh_tsc_full_redesign_build_plan_2026-07-29.md` §0.2.2 records.)
3. **The pending store is NOT free.** It needs one `smp_mb()` per head tenure,
   only when the probe is armed.
4. **Without the clear, Stage A is sound only on HASHED+witness tenures and
   bounded on RUNNING+gate tenures.** The clear costs the uncontended unlock
   fastpath one gate branch when off and an out-of-line call plus tag compare
   when on. §1.4 decides how to stage that.

### 1.4 Decision: the clear's call site moves into the Stage A build (default off)

The clear is the only airtight fix, and the residual race pollutes precisely the
numbers Stage A exists to produce. So the call site in `queued_spin_unlock()`
(§3.5) is **compiled into Stage A**, still behind `ivh_cs_owner_clear = 0`. No
extra boot is needed to try it. Stage A is then measured in **two
configurations inside one boot**:

| config | soundness | role |
|---|---|---|
| **A-clr** — `ivh_cs_owner_clear=1` | airtight on every tenure; gates bypassed | **authoritative**: the kill criterion (§5.7) is evaluated on these numbers |
| **A-gate** — `ivh_cs_owner_clear=0` | airtight on HASHED+witness; bounded on RUNNING+gate; tenure ≥ 1 abstains | the zero-unlock-cost candidate; its numbers are compared against A-clr |

In A-clr the gate is still *computed* in shadow. That yields a direct measurement
of the residual race: `ivh_cs_shadow_gate_pass_released` counts tenure-0 RUNNING
entries where the tag had **already been cleared** at W (`prev` released before
W) but the promptness gate **would have passed**. Those are exactly the
false-positive-eligible tenures A-gate would have admitted. If that count is
negligible against `ivh_cs_ep_events`, A-gate is good enough and Stage B can ship
without touching the unlock fastpath. If not, Stage B ships with the clear, and
its cost is acceptance check A7b.

The shadow count is an estimate, with an error in each direction. Report both
bounds alongside it:
- **Undercount.** `prev` released `lock` before W and then acquired, and still
  holds, some other contended lock M. Its slot reads `{M, …}`, not NULL, so the
  `!tag` test misses a genuine residual-race tenure.
- **Overcount.** `prev` stamped `lock`, nested into M (overwriting the slot),
  released M (clearing it), and still holds `lock`. H then sees a NULL tag with a
  young `->tsc` (M's) even though no release of `lock` occurred. Bound this with
  `ivh_cs_stamp_overwrote / ivh_cs_stamps`: if nesting is rare, the overcount is
  rare.

### 1.5 Gap table

| gap | why | Stage A response |
|---|---|---|
| `!(old & _Q_TAIL_MASK)`: H had no predecessor (role B) | `prev` is an **uninitialised local** at `qspinlock.c:202` | Initialise `prev = NULL`; abstain; `ivh_cs_abstain_noprev` |
| **H halted at handoff, woken by `prev`'s unlock kick; a steal happened before H's `set_pending()`** | `pv_kick_node()` does not wake; `pv_kick()` follows the release; H enters HASHED with a stale stamp tag | **A-gate:** `_Q_SLOW_VAL` witness after `smp_mb()`; abstain on failure, `ivh_cs_abstain_hashed`. **A-clr:** tag already NULL, abstains on the tag |
| H halted at handoff, woken by a tick while `prev` still holds | IF=1 HLT wakes on any interrupt | Witness passes, detection proceeds (a blanket HASHED abstain would lose these) |
| **H running at handoff, delayed before `set_pending()`; `prev` released and a steal landed in the gap** | no lock-byte witness exists for this case | **A-gate:** promptness gate, `ivh_cs_abstain_late` (**bounded, not airtight**). **A-clr:** closed. Residual measured by `ivh_cs_shadow_gate_pass_released` |
| pending store not yet committed when a stealer reads `val` | TSO store buffer | explicit `smp_mb()` after `set_pending()` when probe is armed |
| `waitcnt >= 1` | H halted; `clear_pending()` ran; steals possible | **A-gate:** abstain, `ivh_cs_abstain_tenure`. **A-clr:** allowed |
| `ivh_pv_rot_enable != 0` | `pv_handoff_rotate()` (`qspinlock_paravirt.h:1159`) **rewrites `->next`**, so H's releaser need not be `prev` | abstain `ivh_cs_abstain_rot`; sysctl handlers refuse the combination both ways |
| `prev` nested into an inner lock | tag mismatch | abstain `ivh_cs_abstain_tag`; §5 decides whether a 2-deep stack is warranted |
| lock memory freed and reallocated at the same address | ABA on the tag | cannot happen while H waits on it; the pointer is **compared, never dereferenced** (`ivh_tsc_beat.h:483-490`) |

---

## 2. Storage: clone `ivh_rot_rel`, do not reuse it

The brief asks whether to reuse `struct ivh_rot_rel`. **Clone the shape, new
symbol.** Three reasons, any one sufficient:

1. **Opposite data direction.** `ivh_rot_rel` is written *remotely* (by whoever
   releases a lock *to* this CPU) and read locally — see
   `ivh_rot_stamp_release()`, `qspinlock_paravirt.h:1365-1384`, which does
   `r = &per_cpu(ivh_rot_rel, node->cpu)`. The new slot is written *locally* by
   the acquirer and read *remotely*. Sharing one cacheline between a
   local-write/remote-read and a remote-write/local-read producer would
   false-share by construction.
2. **`ivh_rot_rel` is not actually dead.** `ivh_pv_rot_enable` is 0, but
   `ivh_pv_rot_probe` is a separate, live, writable sysctl (`kvm.c:1371`) and it
   is what gates `ivh_rot_stamp_release()`. A human flipping the probe would
   silently corrupt both features.
3. **The tag means the opposite thing.** In `ivh_rot_rel`, `->lock` means "this
   lock was *released* to you". Here it means "I am *holding* this lock".
   Overloading one field with inverted semantics is how a future reader gets it
   backwards.

---

## 3. The code, site by site

### 3.1 `arch/x86/include/asm/ivh_tsc_beat.h`

**(i)** Add the include, at line 43 after `#include <asm/tsc.h>`. The file-level
comment in `<linux/ivh_lock_holder.h>:61-64` claims this include already exists;
it does not — verified. It is safe: that header's entire dependency set is
`<linux/compiler.h>` + `<linux/types.h>`.

```c
#include <asm/tsc.h>
/*
 * For ivh_cs_owner_enable and the ivh_cs_owner_stamp()/_clear() gates. That
 * API deliberately lives in an arch-neutral, dependency-free header because
 * its call sites are <asm/qspinlock.h> and kernel/locking/qspinlock.c, neither
 * of which can reach this file -- see that header's own writeup. (The claim
 * there that this include already exists was aspirational; it is added here.)
 */
#include <linux/ivh_lock_holder.h>
```

**(ii)** After the `ivh_rot_rel` block (i.e. after `ivh_tsc_beat.h:496`), add the
storage. Note `void *lock`, not `struct qspinlock *`: this header must not need
the qspinlock type, exactly as `ivh_rot_rel` avoids it.

```c
/*
 * ---------------------------------------------------------------------------
 * IVH critical-section owner stamp -- is_cs_preempted()'s input
 * ---------------------------------------------------------------------------
 *
 * Written by a vCPU at the instant it acquires a CONTENDED qspinlock through
 * the MCS queue-head path (kernel/locking/qspinlock.c:462, the one site every
 * MCS-handoff predecessor provably passes through -- see
 * tools/bpf/docs/ivh_is_cs_preempted_build_plan_2026-09-14.md sec 1 for the
 * proof). Read remotely by the NEXT queue head, which reaches this CPU's slot
 * through its own `prev->cpu`.
 *
 * ->lock does double duty. It is the identity of the hold AND its validity
 * flag: a reader that finds a different pointer here knows its `prev` has
 * moved on to some other lock and must abstain. It is COMPARED, NEVER
 * DEREFERENCED -- __pv_queued_spin_unlock_slowpath() documents that lock
 * memory may be freed and reused the instant the releasing store lands.
 *
 * NOT a reuse of struct ivh_rot_rel above, deliberately: that one is written
 * remotely and read locally (the exact opposite direction, so sharing a line
 * would false-share every access), its ->lock means "released TO you" rather
 * than "held BY me", and it is still armed by the separate, live
 * ivh_pv_rot_probe sysctl.
 *
 * Own cacheline for the same one-writer/many-remote-readers reason as
 * struct ivh_tsc_beat.
 */
struct ivh_cs_owner {
	void	*lock;	/* the qspinlock this CPU is holding; NULL == none */
	u64	tsc;	/* raw rdtsc() at the moment of acquisition */
} ____cacheline_aligned_in_smp;

DECLARE_PER_CPU_ALIGNED(struct ivh_cs_owner, ivh_cs_owner);

/*
 * One scheduler tick in raw TSC cycles, and the margin in ticks.
 *
 * DERIVED at late_initcall from tsc_khz and HZ (kvm.c), never hardcoded: the
 * same kernel must answer correctly on a host with a different TSC, and
 * ivh_pv_beat_calibrate() (kvm.c:1556) is the standing precedent. At
 * tsc_khz = 2200000 and HZ = 1000 this is 2200000 cycles.
 *
 * ivh_cs_owed_ticks is the margin, default 2 rather than 1. A hold that began
 * one cycle after tick N is owed tick N+1 within one full period, so 1 period
 * is the theoretical floor; the second period is slack for hrtimer jitter,
 * for tick_sched_do_timer()'s MAX_STALLED_JIFFIES=5 forced-update behaviour
 * (kernel/time/tick-sched.c:204,236-239), and for cross-vCPU TSC skew. It
 * costs reaction time -- ~2-3 ms -- and buys the "no false positives from
 * long critical sections" property that is this predicate's entire claim.
 */
extern unsigned long ivh_cs_tick_period;
extern unsigned long ivh_cs_owed_ticks;

/*
 * Promptness bound for the RUNNING-at-handoff tenure-0 gate (build plan sec
 * 1.2 c-RUNNING): a head abstains for the whole tenure if, at the moment its
 * pending bit is committed, more than this many cycles have passed since its
 * predecessor's acquisition stamp. Derived at late_initcall as
 * IVH_CS_PROMPT_US microseconds from tsc_khz (default 9 us = ~20000 cycles
 * here), and meant to be re-set from ivh_cs_prompt_hist[] once Stage A has
 * data. It NARROWS the residual race; it does not close it. Only the
 * release-side clear (ivh_cs_owner_clear) does.
 */
extern unsigned long ivh_cs_prompt_cycles;
```

**(iii)** Counter declarations. House convention verified: plain
`DECLARE_PER_CPU(u64, …)` with a block comment stating what each measures and
what its denominator is. Append after `ivh_tsc_beat.h:705`.

```c
/*
 * is_cs_preempted() Stage A -- DETECT ONLY. Two exhaustive partitions, and the
 * harness must assert both with ZERO deviation (these are integer counts taken
 * on one CPU with no sampling between them, so "within 0.1%" is too weak here):
 *
 *   ivh_cs_check_calls == ivh_cs_abstain_tenure + ivh_cs_abstain_hashed
 *                       + ivh_cs_abstain_late
 *                       + ivh_cs_abstain_noprev + ivh_cs_abstain_rot
 *                       + ivh_cs_abstain_tag    + ivh_cs_abstain_skew
 *                       + ivh_cs_abstain_young  + ivh_cs_long_hold
 *   ivh_cs_long_hold   == ivh_cs_abstain_nohz + ivh_cs_abstain_retag
 *                       + ivh_cs_healthy_long + ivh_cs_fired
 *
 * The three TENURE-GATE abstains (tenure/hashed/late) are per-CHECK counts of a
 * verdict taken once per head tenure by ivh_cs_tenure_gate(), so they scale
 * with spin length. To size coverage per TENURE use the ivh_cs_tenure0_*
 * counters below instead.
 *
 * ivh_cs_long_hold is the FORM-0 population -- "this hold is longer than
 * ivh_cs_owed_ticks ticks" with no liveness term -- and exists solely as the
 * denominator of the false-positive audit. ivh_cs_healthy_long is the holds
 * that were long AND had ticked since acquiring, i.e. the ones form 0 would
 * have fired on and form 2 correctly exonerates. If
 * ivh_cs_healthy_long / ivh_cs_long_hold is near zero, the tick term is inert
 * and this predicate has silently degenerated into form 0. See
 * ivh_tsc_full_redesign_build_plan_2026-07-29.md sec 1.2 for why that matters.
 *
 * ivh_cs_abstain_nohz MUST read exactly 0 on this host: neither nohz_full= nor
 * dynticks (nohz=off) is in effect. A nonzero value means the command line
 * changed and every other number in the run is suspect.
 *
 * ivh_cs_abstain_skew counts (now - acq) <= 0, i.e. the remote stamp is in our
 * future. Expected ~0 on a TD with a synchronised TSC; a material rate
 * invalidates the whole design, not just this counter.
 */
DECLARE_PER_CPU(u64, ivh_cs_stamps);
DECLARE_PER_CPU(u64, ivh_cs_stamp_overwrote);
DECLARE_PER_CPU(u64, ivh_cs_check_calls);
DECLARE_PER_CPU(u64, ivh_cs_abstain_noprev);
DECLARE_PER_CPU(u64, ivh_cs_abstain_rot);
DECLARE_PER_CPU(u64, ivh_cs_abstain_tag);
DECLARE_PER_CPU(u64, ivh_cs_abstain_skew);
DECLARE_PER_CPU(u64, ivh_cs_abstain_young);
DECLARE_PER_CPU(u64, ivh_cs_abstain_nohz);
DECLARE_PER_CPU(u64, ivh_cs_long_hold);
DECLARE_PER_CPU(u64, ivh_cs_healthy_long);
DECLARE_PER_CPU(u64, ivh_cs_fired);
DECLARE_PER_CPU(u64, ivh_cs_abstain_tenure);	/* waitcnt >= 1 without the clear */
DECLARE_PER_CPU(u64, ivh_cs_abstain_hashed);	/* HASHED entry, _Q_SLOW_VAL witness failed */
DECLARE_PER_CPU(u64, ivh_cs_abstain_late);	/* RUNNING entry, promptness gate failed */
DECLARE_PER_CPU(u64, ivh_cs_abstain_retag);	/* tag changed between first and second read */
DECLARE_PER_CPU(u64, ivh_cs_clears);

/*
 * Per-TENURE soundness-gate accounting (build plan sec 1.2). Counted once per
 * tenure-0 head entry that has a prev and no rotation, in BOTH clear modes, so
 * the size of each hole is measured directly rather than inferred:
 *
 *   ivh_cs_tenure0_enter           - denominator
 *   ivh_cs_tenure0_hashed          - entered with pn->state == VCPU_HASHED
 *                                    (was halted at handoff)
 *   ivh_cs_tenure0_hashed_released - ...and lock->locked != _Q_SLOW_VAL after
 *                                    the pending commit: prev had ALREADY
 *                                    released. This IS the hole found in the
 *                                    first version of the theorem, counted.
 *   ivh_cs_tenure0_late            - RUNNING entry whose stamp age at the
 *                                    pending commit exceeded
 *                                    ivh_cs_prompt_cycles
 *   ivh_cs_shadow_gate_pass_released - (clear==1 only) RUNNING entry whose tag
 *                                    was ALREADY cleared at the pending commit
 *                                    but whose stamp age would have PASSED the
 *                                    promptness gate: the residual race the
 *                                    clear==0 configuration would have
 *                                    admitted. Approximate; see sec 1.4.
 *   ivh_cs_prompt_hist[]           - log2 stamp age at the pending commit for
 *                                    RUNNING entries with a matching tag: the
 *                                    data ivh_cs_prompt_cycles is tuned from.
 */
DECLARE_PER_CPU(u64, ivh_cs_tenure0_enter);
DECLARE_PER_CPU(u64, ivh_cs_tenure0_hashed);
DECLARE_PER_CPU(u64, ivh_cs_tenure0_hashed_released);
DECLARE_PER_CPU(u64, ivh_cs_tenure0_late);
DECLARE_PER_CPU(u64, ivh_cs_shadow_gate_pass_released);
DECLARE_PER_CPU(u64, ivh_cs_prompt_hist[IVH_BEAT_AGE_HIST_BUCKETS]);

/*
 * EPISODE accounting -- the part this project got wrong once and must not get
 * wrong again.
 *
 * Handoff rotation reported 3104 events/s and was worth approximately nothing,
 * because the DURATION of those events was never measured, and because its
 * event counter was sampled every PV_PREV_CHECK_MASK (0xff) iterations so one
 * stall was counted repeatedly by the same waiter. Both defects are structural
 * here, not incidental, so both are designed out:
 *
 *   - An EPISODE is keyed on the HOLDER'S acquisition TSC. A second, third and
 *     256th fire against the same acq stamp extend the open episode; they do
 *     not open a new one. Over-counting by re-sampling is therefore impossible
 *     by construction, not by convention.
 *   - ivh_cs_fired / ivh_cs_ep_events IS the over-count factor rotation never
 *     computed. Report it.
 *   - A new acq stamp (the holder changed) CLOSES the open episode and opens a
 *     fresh one, so an episode can never span two holds.
 *
 * Closed at three exits, kept separate because they mean different things:
 *   ACQUIRED        - we got the lock. duration = the time we spun after
 *                     detecting a dead holder. THIS IS THE RECOVERABLE TIME
 *                     and the only number the go/no-go in sec 5 turns on.
 *   HOLDER_CHANGED  - a different acq stamp appeared while we still spun. A
 *                     true upper bound on what was recoverable.
 *   EXHAUST         - we ran out of spin budget and are about to pv_wait().
 *                     TRUNCATED: a lower bound, never a measurement.
 */
#define IVH_CS_EP_ACQUIRED	0
#define IVH_CS_EP_HOLDER_CHANGED 1
#define IVH_CS_EP_EXHAUST	2
#define IVH_CS_EP_NR		3

DECLARE_PER_CPU(u64, ivh_cs_ep_events);
DECLARE_PER_CPU(u64, ivh_cs_ep_events_by_end[IVH_CS_EP_NR]);
DECLARE_PER_CPU(u64, ivh_cs_ep_cycles[IVH_CS_EP_NR]);
DECLARE_PER_CPU(u64, ivh_cs_ep_hist[IVH_CS_EP_NR][IVH_BEAT_AGE_HIST_BUCKETS]);

/*
 * CONTROL. Without this the episode numbers are unfalsifiable: a detected
 * episode being 300 us long means nothing unless undetected head tenures are
 * shorter. Index 0 = no detection during this tenure, 1 = at least one. Closed
 * at the same three exits, measuring the WHOLE tenure, not just the episode.
 *
 * ivh_cs_prev_hold_hist is the population-correct denominator for the
 * false-positive audit: every time a head acquires a lock whose predecessor's
 * stamp it could read, it records how long that predecessor actually held it.
 * That is the real distribution of CONTENDED hold durations -- which is the
 * only population this predicate ever judges. If ivh_cs_ep_hist sits inside
 * the bulk of this distribution we are firing on normal long holds; if it sits
 * in a separate mode above its p99.9, we are firing on anomalies. Obtained for
 * free on the observer side, so it needs no release-path instrumentation.
 */
DECLARE_PER_CPU(u64, ivh_cs_tenure_cycles[2]);
DECLARE_PER_CPU(u64, ivh_cs_tenure_hist[2][IVH_BEAT_AGE_HIST_BUCKETS]);
DECLARE_PER_CPU(u64, ivh_cs_prev_hold_hist[IVH_BEAT_AGE_HIST_BUCKETS]);
```

Stage B adds, in the same style:

```c
/* Stage B only. Head halts split by cause, mirroring ivh_node_halt_record(). */
#define IVH_CS_HALT_EXHAUST	0
#define IVH_CS_HALT_CS		1
#define IVH_CS_HALT_NR		2
DECLARE_PER_CPU(u64, ivh_cs_head_bailed);
DECLARE_PER_CPU(u64, ivh_head_spin_iters_bail_sum);
DECLARE_PER_CPU(u64, ivh_head_spin_bail_attempts);
DECLARE_PER_CPU(u64, ivh_head_halt_cycles[IVH_CS_HALT_NR]);
DECLARE_PER_CPU(u64, ivh_head_halt_events[IVH_CS_HALT_NR]);
DECLARE_PER_CPU(u64, ivh_head_halt_hist[IVH_CS_HALT_NR][IVH_BEAT_AGE_HIST_BUCKETS]);
```

### 3.2 `include/linux/ivh_lock_holder.h` — the arch-neutral stamp API

This is the only header reachable from **both** `include/asm-generic/qspinlock.h`
(`:53`) and `arch/x86/include/asm/qspinlock.h` (`:13`), and therefore from
`kernel/locking/qspinlock.c`, which is compiled for every architecture. The
worker must be out of line for the reason already documented at `:66-72`: its
body needs `rdtsc()` and per-CPU access, and pulling those in here reopens the
`<asm/qspinlock.h>` → `<linux/sched.h>` → `vcpu_is_preempted` build break.

Insert after the existing `ivh_lock_clear_holder()` (i.e. after `:143`), inside
the `#if defined(CONFIG_X86) && defined(CONFIG_KVM_GUEST) && defined(CONFIG_PARAVIRT_SPINLOCKS)`
block:

```c
/*
 * IVH critical-section owner stamp -- a SECOND, independent mechanism from the
 * holder side table above, and deliberately not built on it.
 *
 * The table answers "who holds an arbitrary lock" and pays for that generality
 * with a hash, a collision mode, and a stamp on queued_spin_lock()'s
 * uncontended fastpath (site A1). is_cs_preempted() does not need that
 * generality: the queue head already holds a pointer to its predecessor, and
 * its predecessor is the holder under the conditions of build plan sec 1.2. So this is a single per-CPU slot,
 * written at exactly one site on the CONTENDED path, and it costs the
 * uncontended ACQUIRE fastpath nothing; the release side costs one gate
 * branch with ivh_cs_owner_clear == 0 (build plan sec 1.4).
 *
 * Storage and the worker bodies: arch/x86/kernel/ivh_lock_holder.c.
 * The predicate that reads them: kernel/locking/qspinlock_paravirt.h.
 * The proof that one stamp site suffices:
 * tools/bpf/docs/ivh_is_cs_preempted_build_plan_2026-09-14.md sec 1.
 */
extern unsigned long ivh_cs_owner_enable;
extern unsigned long ivh_cs_owner_clear;

void __ivh_cs_owner_stamp(struct qspinlock *lock);
void __ivh_cs_owner_clear(struct qspinlock *lock);

static __always_inline void ivh_cs_owner_stamp(struct qspinlock *lock)
{
	if (likely(!READ_ONCE(ivh_cs_owner_enable)))
		return;
	__ivh_cs_owner_stamp(lock);
}

/*
 * Compiled in from Stage A (default off; build plan sec 1.4 -- it is the only
 * airtight close of the RUNNING-at-handoff race), and gated on its OWN sysctl
 * rather than on ivh_cs_owner_enable, because this is the one call that lands on the
 * uncontended unlock fastpath and its cost must be separable from the stamp's.
 * Placement rule is inherited unchanged from R1/R2/R2b: STRICTLY BEFORE the
 * releasing store, never after -- the instant the lock byte clears, another
 * CPU may already own the lock, and a clear placed after would wipe ITS stamp.
 */
static __always_inline void ivh_cs_owner_release(struct qspinlock *lock)
{
	if (likely(!READ_ONCE(ivh_cs_owner_clear)))
		return;
	__ivh_cs_owner_clear(lock);
}
```

And in the `#else` stub block (after `:149`):

```c
static inline void ivh_cs_owner_stamp(struct qspinlock *lock) { }
static inline void ivh_cs_owner_release(struct qspinlock *lock) { }
```

### 3.3 `arch/x86/kernel/ivh_lock_holder.c` — the workers

Add `#include <asm/ivh_tsc_beat.h>` to the include list (this is a `.c`; the
documented build break applies only to `<asm/qspinlock.h>`). Append:

```c
/*
 * is_cs_preempted()'s owner stamp. See <linux/ivh_lock_holder.h> for why the
 * gate is inlined and the body is not, and the 2026-09-14 build plan sec 1 for
 * why one site suffices.
 *
 * ->tsc must become visible before ->lock: ->lock is the validity flag the
 * remote reader tests, and without this ordering a reader can pair a matching
 * ->lock with a stale ->tsc from a previous hold and compute a wildly wrong --
 * or negative -- held_for. Identical rule and identical reason to
 * ivh_rot_stamp_release() (kernel/locking/qspinlock_paravirt.h:1375-1383). On
 * x86-TSO smp_wmb() is a compiler barrier, so this is free; it is written
 * because the rule is real, not because the instruction is.
 */
void __ivh_cs_owner_stamp(struct qspinlock *lock)
{
	if (unlikely(this_cpu_read(ivh_cs_owner.lock)))
		this_cpu_inc(ivh_cs_stamp_overwrote);

	this_cpu_write(ivh_cs_owner.tsc, rdtsc());
	smp_wmb();
	this_cpu_write(ivh_cs_owner.lock, lock);
	this_cpu_inc(ivh_cs_stamps);
}
EXPORT_SYMBOL_GPL(__ivh_cs_owner_stamp);

/*
 * Tag-checked and therefore idempotent: a release of a lock this CPU never
 * stamped, or of an outer lock whose slot an inner one has since overwritten,
 * finds a mismatch and does nothing. That is what makes partial stamp coverage
 * safe -- the stamps/clears accounting identity is
 *   ivh_cs_stamps == ivh_cs_clears + ivh_cs_stamp_overwrote + (in flight)
 * rather than a raw equality, which is the lesson of the 680000:1
 * stamps:clears ratio recorded at <asm/qspinlock.h>:100-140.
 */
void __ivh_cs_owner_clear(struct qspinlock *lock)
{
	s64 held;

	if (this_cpu_read(ivh_cs_owner.lock) != (void *)lock)
		return;
	this_cpu_write(ivh_cs_owner.lock, NULL);
	this_cpu_inc(ivh_cs_clears);

	/*
	 * Holder-side hold-duration sample. Under clear==1 the observer-side
	 * sample at gotlock: can never fire (this store has already NULLed
	 * the tag it tests), so the population-correct contended-hold
	 * histogram is taken here instead. Only stamped holds reach this
	 * line, and only contended acquisitions are stamped, so this is the
	 * same population either way.
	 */
	held = (s64)(rdtsc() - this_cpu_read(ivh_cs_owner.tsc));
	if (held > 0)
		this_cpu_inc(ivh_cs_prev_hold_hist[held >= (1LL << 31) ?
				IVH_BEAT_AGE_HIST_BUCKETS - 1 : ilog2((u64)held)]);
}
EXPORT_SYMBOL_GPL(__ivh_cs_owner_clear);
```

(`ivh_cs_clears` is declared with the Stage-A block in §3.1 iii. The clear's
call site now ships in the Stage A build, still off by default; see §1.4.
`ilog2` needs `<linux/log2.h>`.)

### 3.4 `kernel/locking/qspinlock.c` — plumbing and the stamp

**Edit 1 — `:202`.** `prev` is an uninitialised local and is read at the new
call site whenever the `if (old & _Q_TAIL_MASK)` branch was not taken. GCC will
warn (`-Wmaybe-uninitialized`) and the value is genuine garbage.

```c
-	struct mcs_spinlock *prev, *next, *node;
+	/*
+	 * prev is NULL-initialised, not merely declared: it is now passed to
+	 * pv_wait_head_or_lock() below, and the `if (old & _Q_TAIL_MASK)`
+	 * branch that assigns it is not always taken (the first thread to
+	 * queue has no predecessor). NULL is the "no identity" value that
+	 * is_cs_preempted() abstains on. Dead-store-eliminated in the native
+	 * build, where __pv_wait_head_or_lock() ignores the argument.
+	 */
+	struct mcs_spinlock *prev = NULL, *next, *node;
```

**Edit 2 — `:417`.** Pass it.

```c
-	if ((val = pv_wait_head_or_lock(lock, node)))
+	if ((val = pv_wait_head_or_lock(lock, node, prev)))
```

**Edit 3 — `:155-157`.** Widen the native stub.

```c
 static __always_inline u32  __pv_wait_head_or_lock(struct qspinlock *lock,
-						   struct mcs_spinlock *node)
+						   struct mcs_spinlock *node,
+						   struct mcs_spinlock *prev)
 						   { return 0; }
```

**Edit 4 — `:462`, the stamp. This is the one required stamp site.**

```c
 	set_locked(lock);
+	/*
+	 * IVH ownership stamp for is_cs_preempted(). Site A5, and the ONLY
+	 * site that matters: every predecessor that ever hands an MCS baton to
+	 * a successor passes through here, including the PV queue head that
+	 * acquired via trylock_clear_pending() (A7) or via the
+	 * xchg(_Q_SLOW_VAL)==0 race (A8) -- both of those `goto gotlock`,
+	 * return nonzero, land at `locked:` above, fail the :452 uncontended
+	 * cmpxchg because the successor is the tail, and fall through to here.
+	 * See the 2026-09-14 build plan sec 1(b).
+	 *
+	 * Placement is AFTER the acquiring store and BEFORE the successor's
+	 * release at :487. The ordering argument is STORE->STORE UNDER x86-TSO,
+	 * not "the locked RMW fences it": set_locked() is a plain WRITE_ONCE
+	 * (kernel/locking/qspinlock.h:196-199), not an RMW. :487's
+	 * arch_mcs_spin_unlock_contended() is an smp_store_release, so any
+	 * successor that has observed node->locked == 1 is guaranteed to see
+	 * this stamp. No barrier is required here.
+	 *
+	 * Cost at the default ivh_cs_owner_enable == 0 is one READ_ONCE of a
+	 * read-mostly global plus one perfectly-predicted branch -- the same
+	 * posture as ivh_beat_publish_in_spin() and ivh_lock_set_holder(). The
+	 * uncontended fastpath is NOT touched: this is reached only after the
+	 * MCS queue has actually formed.
+	 */
+	ivh_cs_owner_stamp(lock);
```

**Edit 5 (optional, recommended) — `:453`, site A4.** Completeness only: an A4
acquirer provably has no MCS successor, so it can never be anyone's `prev`. It
becomes relevant only when `ivh_cs_owner_clear=1` lifts the tenure restriction
(configuration A-clr, §1.4), at which point tenure-≥1 heads read the slot of a
CPU that may have acquired this way. One line, same slowpath, no measurable cost.

```c
 	if ((val & _Q_TAIL_MASK) == tail) {
 		if (atomic_try_cmpxchg_relaxed(&lock->val, &val, _Q_LOCKED_VAL))
+			/* Site A4. See the A5 comment below; relaxed cmpxchg,
+			 * so the same store->store argument applies. */
+			ivh_cs_owner_stamp(lock),
 			goto release; /* No contention */
 	}
```
*(Write this as a proper block rather than a comma expression; shown compactly
here only to mark the position.)*

**Edit 6 — includes.** Add `#include <linux/tick.h>` near `:24`, for
`tick_nohz_full_cpu()`. `kernel/locking/` currently includes it nowhere.
`ivh_cs_owner_stamp()` needs no new include: it arrives via
`<asm/qspinlock.h>:13` → `<linux/ivh_lock_holder.h>` on x86 and via
`<include/asm-generic/qspinlock.h>:53` on every other architecture, where it is
a stub.

### 3.5 `arch/x86/include/asm/qspinlock.h` — the clear (compiled in Stage A, default off)

R2b is documented as "THE release site on this kernel" (`:100-140`) — the fix for
the measured 680 000:1 stamps:clears ratio, because R1 is `#ifndef`'d out, R2 is
unreachable, R3 is not compiled on x86-64 (the real unlock is the hand-written
`PV_UNLOCK_ASM` thunk), and R4 only runs on the `_Q_SLOW_VAL` path. Same site,
same placement rule.

```c
 static inline void queued_spin_unlock(struct qspinlock *lock)
 {
 	kcsan_release();
 	...
 	ivh_lock_clear_holder(lock);
+	/*
+	 * is_cs_preempted()'s owner clear. Gated on its own sysctl, default 0:
+	 * this is the ONE call this feature places on the uncontended unlock
+	 * fastpath, and its cost must be separately measurable from the
+	 * stamp's (acceptance check A7b). It is compiled into the Stage A
+	 * build because it is the only AIRTIGHT close of the
+	 * RUNNING-at-handoff race (2026-09-14 build plan sec 1.2/1.4); Stage A
+	 * is measured with it on (authoritative) and off (gated) in one boot.
+	 *
+	 * Strictly before pv_queued_spin_unlock(), for the reason R1/R2/R2b
+	 * already document: the moment the lock byte clears, another CPU may
+	 * own the lock, and a clear placed after would wipe ITS stamp.
+	 */
+	ivh_cs_owner_release(lock);
 	pv_queued_spin_unlock(lock);
 }
```

### 3.6 `kernel/locking/qspinlock_paravirt.h` — the predicate and the head

**(i) The predicate.** Place immediately after `is_wait_preempted()` (i.e. after
`:478`, the closing brace after `return kvm;` at `:477`), so the two sit together and the contrast is visible.

```c
/*
 * Is the CURRENT HOLDER of @lock -- not our predecessor-as-a-waiter, which is
 * what pv_wait_early()'s tier 1 and tier 2 answer -- host-preempted?
 *
 * The test is "did the holder miss a tick it owed us", NOT "is the holder's
 * heartbeat stale". The distinction is the whole point and the earlier
 * specification got it wrong:
 *
 *   ivh_tsc_full_redesign_build_plan_2026-07-29.md sec 1.2 proposed
 *   `cs_stamp != 0 && ivh_beat_stale(holder_cpu)` ("form 1"), arguing that the
 *   tick is a hardirq and fires through preempt_disable(), so a running holder
 *   stays fresh. True -- but fresh at TICK cadence, 1 ms, while
 *   ivh_pv_beat_threshold is 220000 cycles = 100 us in the tuned configuration
 *   this box actually runs (spin_mode 2). A perfectly healthy holder therefore
 *   reads stale for ~90% of every tick period. Form 1 is a false-positive
 *   generator here. It is NOT one at the compiled default of 3300000 cycles
 *   (1.5 ms), which is why the earlier plan's reasoning looked sound.
 *
 * So: deliberately DO NOT read ivh_pv_beat_threshold. A holder that acquired
 * at time T and is running must publish by T + one tick period. If it has not
 * published by T + ivh_cs_owed_ticks periods, it is not running. A holder in a
 * five-millisecond critical section still ticks, and is exonerated. That is
 * the property this predicate exists to have.
 *
 * Reaction time is floored at ivh_cs_owed_ticks ticks, ~2-3 ms at the default.
 * That is SLOWER than ivh_pv_spin_threshold's ~45 us at 32768 iterations, and
 * that is fine: the two are not competitors, see the head loop below.
 *
 * Returns true and fills *acq_out / *held_out only on a fire.
 */
static inline bool is_cs_preempted(struct qspinlock *lock, struct pv_node *prev,
				   u64 *acq_out, u64 *held_out)
{
	struct ivh_cs_owner *o;
	u64 acq, beat, now;
	s64 held;

	/* ivh_cs_check_calls is counted by the caller, ivh_cs_head_probe_one(),
	 * so the tenure-gate abstains fall inside the same partition. */
	if (!prev) {
		/* Role B: first thread queued, xchg_tail() returned no prior
		 * tail, so there is no predecessor and no identity. Structural,
		 * not a bug -- counted so its size is known rather than
		 * assumed. See ivh_head_waiter_adaptive_spinning_design
		 * _2026-09-14.md sec 1 role B. */
		this_cpu_inc(ivh_cs_abstain_noprev);
		return false;
	}

	/*
	 * Hard interlock with handoff rotation. pv_handoff_rotate() REWRITES
	 * ->next pointers in the queue, so under ivh_pv_rot_enable the node
	 * that released our MCS baton need not be the node we linked behind,
	 * and `prev` is then not the holder. The sysctl handlers refuse the
	 * combination in both directions; this is the belt to that braces,
	 * because the two knobs can in principle be raced against each other.
	 */
	if (unlikely(READ_ONCE(ivh_pv_rot_enable))) {
		this_cpu_inc(ivh_cs_abstain_rot);
		return false;
	}

	o = &per_cpu(ivh_cs_owner, prev->cpu);

	/*
	 * prev->cpu is safe to read at ANY time, and this is worth stating
	 * because it is the one place a stale pointer could have bitten:
	 * qnodes[] is DEFINE_PER_CPU_ALIGNED (qspinlock.c:138) and
	 * pv_init_node() stores pn->cpu = smp_processor_id(), so the ->cpu
	 * field of the node at (cpu, idx) is that cpu, permanently, across
	 * every reuse of the slot. It cannot go stale in a harmful direction.
	 * The only real staleness question -- "is that CPU still the holder" --
	 * is answered by the tag compare on the next line.
	 */
	if (READ_ONCE(o->lock) != (void *)lock) {
		this_cpu_inc(ivh_cs_abstain_tag);
		return false;
	}
	smp_rmb();		/* pairs with __ivh_cs_owner_stamp()'s smp_wmb() */
	acq = READ_ONCE(o->tsc);

	now  = rdtsc();
	held = (s64)(now - acq);

	/*
	 * SIGNED, for the same reason ivh_beat_age() is: a small negative
	 * cross-vCPU TSC skew must read as "too young", not wrap to an enormous
	 * positive and fire instantly.
	 */
	if (held <= 0) {
		this_cpu_inc(ivh_cs_abstain_skew);
		return false;
	}
	if ((u64)held <= (u64)READ_ONCE(ivh_cs_tick_period) *
			 READ_ONCE(ivh_cs_owed_ticks)) {
		this_cpu_inc(ivh_cs_abstain_young);
		return false;
	}

	/* FORM-0 population: long hold, liveness not yet consulted. */
	this_cpu_inc(ivh_cs_long_hold);

	/*
	 * NO_HZ_FULL guard, unconditional and not a command-line assumption.
	 *
	 * On an adaptive-ticks CPU the absence of a beat proves nothing. The
	 * tick-stop decision is taken at tick_nohz_irq_exit()
	 * (kernel/time/tick-sched.c:1295), reached from tick_irq_exit()
	 * (kernel/softirq.c:639-650) whose only context gate is !in_hardirq() --
	 * it tests HARDIRQ_MASK and says nothing about PREEMPT_MASK. And
	 * can_stop_full_tick() (tick-sched.c:358-375) checks six tick_dep bits
	 * and has no preempt_count() or lockdep check at all. So a nohz_full
	 * CPU CAN hold a contended spinlock with the tick stopped:
	 * Documentation/timers/no_hz.rst:139-142, "transitioning to kernel mode
	 * does not automatically change the mode".
	 *
	 * On THIS boot it cannot: /proc/cmdline carries neither nohz_full= nor
	 * dynticks (it carries nohz=off), tick_nohz_full_running is false, and
	 * tick_nohz_full_cpu() is a NOP-patched read-only static branch
	 * (context_tracking_key, DEFINE_STATIC_KEY_FALSE_RO) -- free. It is
	 * here so the predicate does not depend on that staying true, and
	 * ivh_cs_abstain_nohz must read exactly 0 in every run on this host.
	 */
	if (unlikely(tick_nohz_full_cpu(prev->cpu))) {
		this_cpu_inc(ivh_cs_abstain_nohz);
		return false;
	}

	beat = READ_ONCE(per_cpu(ivh_tsc_beat, prev->cpu).stamp);

	/*
	 * Second tag read. Under ivh_cs_owner_clear == 1 the clear commits
	 * ->lock = NULL before the releasing store, so a tag that still reads
	 * @lock HERE means the whole {lock, tsc, beat} observation was taken
	 * inside the hold (build plan sec 1.2 d). Under clear == 0 it is
	 * harmless and catches a nested re-stamp that raced the reads.
	 */
	if (READ_ONCE(o->lock) != (void *)lock) {
		this_cpu_inc(ivh_cs_abstain_retag);
		return false;
	}
	if ((s64)(beat - acq) >= 0) {
		/*
		 * It HAS ticked since acquiring. Long, but alive. This is the
		 * population form 0 would have fired on and this predicate
		 * correctly exonerates; ivh_cs_healthy_long / ivh_cs_long_hold
		 * is the false-positive audit ratio. Note the holder also
		 * publishes from ivh_beat_publish_in_spin() and pv_init_node()
		 * when it is itself contending on some inner lock, which can
		 * only move samples INTO this branch -- the safe direction.
		 */
		this_cpu_inc(ivh_cs_healthy_long);
		return false;
	}

	*acq_out  = acq;
	*held_out = (u64)held;
	this_cpu_inc(ivh_cs_fired);
	return true;
}

/* Shared log2 bucketing, same convention as ivh_beat_age_hist_raw. */
static __always_inline int ivh_cs_bucket(u64 v)
{
	int b = v ? ilog2(v) : 0;

	return b >= IVH_BEAT_AGE_HIST_BUCKETS ? IVH_BEAT_AGE_HIST_BUCKETS - 1 : b;
}

/*
 * Close the open episode, if any. Keyed on the holder's acquisition TSC in
 * *ep_acq: that key is what makes re-sampling harmless. Zeroes the key.
 */
static __always_inline void ivh_cs_ep_close(u64 *ep_acq, u64 ep_start, u64 now,
					    int why)
{
	u64 d;

	if (!*ep_acq)
		return;
	d = now - ep_start;
	this_cpu_add(ivh_cs_ep_cycles[why], d);
	this_cpu_inc(ivh_cs_ep_events_by_end[why]);
	this_cpu_inc(ivh_cs_ep_hist[why][ivh_cs_bucket(d)]);
	*ep_acq = 0;
}

/*
 * Per-tenure soundness gate (build plan sec 1.2). Called ONCE per head tenure,
 * immediately after set_pending(), only when ivh_cs_head_probe is armed.
 * Returns the verdict ivh_cs_head_probe_one() applies to every sampled check
 * in the tenure.
 */
#define IVH_CS_GATE_OK		0
#define IVH_CS_GATE_TENURE	1	/* waitcnt >= 1, no clear */
#define IVH_CS_GATE_HASHED	2	/* halted at handoff, prev already released */
#define IVH_CS_GATE_LATE	3	/* running at handoff, promptness gate failed */

static noinline u8 ivh_cs_tenure_gate(struct qspinlock *lock,
				      struct pv_node *prev, int waitcnt,
				      bool entered_hashed)
{
	bool clr = READ_ONCE(ivh_cs_owner_clear);
	s64 prompt = (s64)READ_ONCE(ivh_cs_prompt_cycles);
	struct ivh_cs_owner *o;
	void *tag;
	s64 age;

	/*
	 * W: commit our pending store. set_pending() is a plain WRITE_ONCE
	 * (_Q_PENDING_BITS == 8); until it drains from our store buffer, a
	 * stealer's atomic_read() in pv_hybrid_queued_unfair_trylock() on
	 * another CPU can still see pending == 0. On x86-64 smp_mb() is
	 * `lock addl $0,-4(%rsp)` (arch/x86/include/asm/barrier.h:53), a
	 * serialising instruction. Paid once per tenure, only with the probe
	 * armed; acceptance check A7 includes it.
	 */
	smp_mb();

	/* No identity, or rotation: is_cs_preempted() abstains and counts. */
	if (!prev || READ_ONCE(ivh_pv_rot_enable))
		return IVH_CS_GATE_OK;

	if (waitcnt)
		return clr ? IVH_CS_GATE_OK : IVH_CS_GATE_TENURE;

	this_cpu_inc(ivh_cs_tenure0_enter);

	if (entered_hashed) {
		/*
		 * Halted at handoff. pv_kick_node() wrote _Q_SLOW_VAL and did
		 * NOT wake us; we were woken either by prev's unlock-slowpath
		 * pv_kick() (AFTER its release -- a steal may have happened) or
		 * by an unrelated interrupt (IF=1 HLT; prev may still hold).
		 * _Q_SLOW_VAL is written only by prev's pv_kick_node() for THIS
		 * handoff and cleared only by prev's release, so reading it
		 * after W proves prev still holds and, pending now being
		 * committed, will until we acquire. Airtight. Sec 1.2 c-HASHED.
		 */
		this_cpu_inc(ivh_cs_tenure0_hashed);
		if (READ_ONCE(lock->locked) != _Q_SLOW_VAL) {
			this_cpu_inc(ivh_cs_tenure0_hashed_released);
			return clr ? IVH_CS_GATE_OK : IVH_CS_GATE_HASHED;
		}
		return IVH_CS_GATE_OK;
	}

	/*
	 * Running at handoff. No lock-byte witness exists (_Q_LOCKED_VAL both
	 * before and after a steal), so bound the window instead. NOT airtight:
	 * a hold shorter than the bound can complete and be stolen inside it.
	 * Tag first, then tsc: the stamp writes tsc then tag.
	 */
	o   = &per_cpu(ivh_cs_owner, prev->cpu);
	tag = READ_ONCE(o->lock);
	age = (s64)(rdtsc() - READ_ONCE(o->tsc));

	if (tag == (void *)lock) {
		this_cpu_inc(ivh_cs_prompt_hist[ivh_cs_bucket(age > 0 ? (u64)age : 0)]);
		if (age > prompt) {
			this_cpu_inc(ivh_cs_tenure0_late);
			return clr ? IVH_CS_GATE_OK : IVH_CS_GATE_LATE;
		}
		return IVH_CS_GATE_OK;
	}

	/*
	 * Tag already cleared at W under clr == 1: prev released before our
	 * pending committed. If the promptness gate would have PASSED this,
	 * it is exactly a tenure the clr == 0 configuration would have
	 * admitted with a stale stamp. Shadow-count it (sec 1.4).
	 */
	if (clr && !tag && age <= prompt)
		this_cpu_inc(ivh_cs_shadow_gate_pass_released);

	return IVH_CS_GATE_OK;	/* is_cs_preempted() abstains on the tag */
}

/*
 * One sampled probe. noinline on purpose: the head's spin loop is the hottest
 * loop in the kernel under contention and must stay small in icache. Returns
 * true if the predicate fired (Stage B acts on that; Stage A ignores it).
 */
static noinline bool ivh_cs_head_probe_one(struct qspinlock *lock,
					   struct pv_node *prev, u8 gate,
					   u64 *ep_acq, u64 *ep_start,
					   bool *ep_any)
{
	u64 acq = 0, held = 0, now;

	this_cpu_inc(ivh_cs_check_calls);

	/*
	 * Apply the per-tenure soundness verdict (ivh_cs_tenure_gate()). Any
	 * non-OK verdict means `prev` may no longer be the holder while its
	 * stamp tag still names @lock -- a potential FALSE POSITIVE, the bad
	 * direction -- so the whole tenure abstains.
	 */
	switch (gate) {
	case IVH_CS_GATE_TENURE:
		this_cpu_inc(ivh_cs_abstain_tenure);
		return false;
	case IVH_CS_GATE_HASHED:
		this_cpu_inc(ivh_cs_abstain_hashed);
		return false;
	case IVH_CS_GATE_LATE:
		this_cpu_inc(ivh_cs_abstain_late);
		return false;
	}

	if (!is_cs_preempted(lock, prev, &acq, &held)) {
		/*
		 * Not firing does NOT close the episode -- a single sampled
		 * miss inside a genuine stall (say the holder briefly ticked
		 * from an inner lock's spin loop) would otherwise fragment one
		 * episode into several and re-inflate exactly the event count
		 * this design exists to deflate. Only an ACQUIRE, a holder
		 * change, or tenure exit closes an episode.
		 */
		return false;
	}

	now = rdtsc();

	if (*ep_acq && *ep_acq != acq) {
		/* The holder changed under us: close the old episode as an
		 * upper bound and open a new one. */
		ivh_cs_ep_close(ep_acq, *ep_start, now, IVH_CS_EP_HOLDER_CHANGED);
	}
	if (!*ep_acq) {
		*ep_acq   = acq;
		*ep_start = now;
		*ep_any   = true;
		this_cpu_inc(ivh_cs_ep_events);
	}
	return true;
}
```

**(ii) The head.** Signature, `qspinlock_paravirt.h:1497-1499`:

```c
 static u32
-pv_wait_head_or_lock(struct qspinlock *lock, struct mcs_spinlock *node)
+pv_wait_head_or_lock(struct qspinlock *lock, struct mcs_spinlock *node,
+		     struct mcs_spinlock *prev)
 {
 	struct pv_node *pn = (struct pv_node *)node;
+	struct pv_node *pp = (struct pv_node *)prev;	/* may be NULL */
 	struct qspinlock **lp = NULL;
 	int waitcnt = 0;
 	unsigned long loop;
 	unsigned long threshold;
+	/*
+	 * ALL episode state is in locals. struct pv_node stays exactly 32
+	 * bytes with head_ctl at offset 24 and rot_flags in the 3-byte hole at
+	 * 21; nothing here touches it. That is not incidental -- the head is a
+	 * single thread running a single loop, so its detection state has no
+	 * reason to be visible to anyone else.
+	 */
+	u64 ep_acq = 0, ep_start = 0, tenure_start = 0;
+	bool ep_any = false, probe, entered_hashed = false;
+	u8 cs_gate = IVH_CS_GATE_OK;
```

Entry check, `:1510-1511`. Capture the HASHED entry. This is the exact
halted-at-handoff case of §1.2, and `pn->state` is overwritten with
`VCPU_RUNNING` at the top of the loop, so it must be saved here:

```c
 	if (READ_ONCE(pn->state) == VCPU_HASHED)
-		lp = (struct qspinlock **)1;
+	{
+		lp = (struct qspinlock **)1;
+		/* sec 1.2: was halted at handoff; prev may already have
+		 * released before we could set pending. */
+		entered_hashed = true;
+	}
```

Top of the outer `for (;; waitcnt++)`, after the existing
`this_cpu_inc(ivh_head_spin_enter);` at `:1542`:

```c
+		/*
+		 * Read the gate ONCE per tenure, not per iteration -- the same
+		 * rule the G-LOCK-21-spin comments impose on
+		 * ivh_pv_spin_threshold two lines below, and for the same
+		 * reason: a live sysctl flip must not take effect mid-spin, or
+		 * the per-tenure accounting stops being internally consistent.
+		 */
+		probe = READ_ONCE(ivh_cs_head_probe);
+		ep_acq = 0;
+		ep_any = false;
+		if (probe)
+			tenure_start = rdtsc();
```

Immediately after `set_pending(lock);` at `:1548`, the per-tenure soundness
gate:

```c
 		set_pending(lock);
+		/*
+		 * Soundness gate, once per tenure, BEFORE the first sampled
+		 * check: commits the pending store and decides whether `prev`
+		 * is provably still the holder (build plan sec 1.2). Behaviour-
+		 * neutral: no control flow depends on cs_gate outside the probe.
+		 */
+		if (unlikely(probe))
+			cs_gate = ivh_cs_tenure_gate(lock, pp, waitcnt,
+						     entered_hashed);
```

Inside the inner spin loop, `:1551-1563`, immediately after `ivh_beat_publish_in_spin(loop);` at `:1561`
and `cpu_relax()`:

```c
 			ivh_beat_publish_in_spin(loop);
+			/*
+			 * IVH head adaptive check. STAGE A IS DETECT-ONLY:
+			 * there is no break, no goto, and no store outside
+			 * this_cpu counters below this point, so the loop's
+			 * trip count is bit-identical to before. That is the
+			 * behaviour-neutrality proof, and it is checkable:
+			 * ivh_head_spin_iters_sum / ivh_head_spin_attempts must
+			 * still equal ivh_pv_spin_threshold exactly.
+			 *
+			 * Sampled on PV_PREV_CHECK_MASK, the same cadence as
+			 * pv_wait_early()'s tier 2, for the same cacheline
+			 * reason -- every evaluation pulls a remote line
+			 * (prev's ivh_cs_owner, then prev's ivh_tsc_beat).
+			 *
+			 * At the default ivh_cs_head_probe == 0 this is one
+			 * already-loaded register test and one predicted
+			 * not-taken branch.
+			 */
+			if (unlikely(probe) &&
+			    (loop & PV_PREV_CHECK_MASK) == 0)
+				ivh_cs_head_probe_one(lock, pp, cs_gate,
+						      &ep_acq, &ep_start,
+						      &ep_any);
 			cpu_relax();
```

After the loop, before `if (!lp)`, at the existing accounting block `:1574-1575` (comment opens at `:1567`)
— the EXHAUST close:

```c
 		this_cpu_add(ivh_head_spin_iters_sum, threshold - loop);
 		this_cpu_inc(ivh_head_spin_attempts);
+		if (unlikely(probe)) {
+			u64 now = rdtsc();
+
+			ivh_cs_ep_close(&ep_acq, ep_start, now, IVH_CS_EP_EXHAUST);
+			ivh_cs_tenure_record(tenure_start, now, ep_any);
+		}
```

At `gotlock:` (label `:1646`), immediately before the `head_ctl` reset at `:1676` — the ACQUIRED
close, plus the hold-duration sample that gives the false-positive audit its
denominator:

```c
+	if (unlikely(probe)) {
+		u64 now = rdtsc();
+
+		/*
+		 * The predecessor's hold has just ended -- we are taking the
+		 * lock it released. If its stamp is still readable, this is a
+		 * free, population-correct sample of how long a CONTENDED hold
+		 * actually lasts on this workload, which is exactly the
+		 * distribution the false-positive audit needs and the only one
+		 * this predicate ever judges. Costs nothing on the release path.
+		 */
+		/*
+		 * Only when the tenure gate PASSED and the clear is off. A
+		 * failed gate means prev may have released long ago, so
+		 * now - a would span a stealer's hold too. With the clear on,
+		 * the tag is already NULL here and the sample is taken
+		 * holder-side in __ivh_cs_owner_clear() instead.
+		 */
+		if (pp && cs_gate == IVH_CS_GATE_OK &&
+		    !READ_ONCE(ivh_cs_owner_clear) &&
+		    !READ_ONCE(ivh_pv_rot_enable)) {
+			struct ivh_cs_owner *o = &per_cpu(ivh_cs_owner, pp->cpu);
+			u64 a = READ_ONCE(o->tsc);
+			s64 h = (s64)(now - a);
+
+			if (READ_ONCE(o->lock) == (void *)lock && h > 0)
+				this_cpu_inc(ivh_cs_prev_hold_hist[ivh_cs_bucket(h)]);
+		}
+		ivh_cs_ep_close(&ep_acq, ep_start, now, IVH_CS_EP_ACQUIRED);
+		ivh_cs_tenure_record(tenure_start, now, ep_any);
+	}
 	WRITE_ONCE(pn->head_ctl, HC(0, 0, HEAD_IDLE));
```

with

```c
static __always_inline void ivh_cs_tenure_record(u64 start, u64 now, bool det)
{
	u64 d = now - start;
	int i = det ? 1 : 0;

	this_cpu_add(ivh_cs_tenure_cycles[i], d);
	this_cpu_inc(ivh_cs_tenure_hist[i][ivh_cs_bucket(d)]);
}
```

**Deliberately NOT done: setting `HEAD_YIELDED`.** The brief notes it exists and
is never set. It is reserved by Idea 2 Stage 1 (head-role takeover), and
`qspinlock_paravirt.h:61-66` states that `ivh_head_woke_yielded` **must read 0 in
every Stage-0 run** — that is that design's acceptance check. Overloading the
value here would destroy it. This feature gets its own counters instead.

### 3.7 `arch/x86/kernel/kvm.c` — definitions, calibration, sysctls

**(i) Knobs**, next to the existing block at `:1362-1397`:

```c
/*
 * is_cs_preempted() knobs. All default 0 / inert: at these values the feature
 * is one predicted branch at the stamp site and one at the probe site, and
 * nothing else in the kernel changes.
 */
unsigned long ivh_cs_owner_enable = 0UL;	/* arm the stamp at qspinlock.c:462 */
unsigned long ivh_cs_owner_clear  = 0UL;	/* arm the unlock-path clear; compiled in Stage A (sec 1.4) */
unsigned long ivh_cs_head_probe   = 0UL;	/* arm the head-side detect + count */
unsigned long ivh_cs_head_bail    = 0UL;	/* Stage B: THE only behaviour knob */
unsigned long ivh_cs_owed_ticks   = 2UL;
/*
 * Compiled default assumes 2.2 GHz at HZ=1000; overwritten at late_initcall
 * from the live tsc_khz, exactly as ivh_pv_beat_threshold is (:1556).
 */
unsigned long ivh_cs_tick_period  = 2200000UL;
#define IVH_CS_PROMPT_US	9ULL
unsigned long ivh_cs_prompt_cycles = 19800UL;	/* 9 us at 2.2 GHz; recalibrated below */
EXPORT_SYMBOL_GPL(ivh_cs_owner_enable);
EXPORT_SYMBOL_GPL(ivh_cs_owner_clear);
```

**(ii) Storage and counters**, in the block at `:1483-1503`:

```c
/* is_cs_preempted() -- see <asm/ivh_tsc_beat.h> for what each measures. */
DEFINE_PER_CPU_ALIGNED(struct ivh_cs_owner, ivh_cs_owner);
EXPORT_PER_CPU_SYMBOL_GPL(ivh_cs_owner);
DEFINE_PER_CPU(u64, ivh_cs_stamps);
DEFINE_PER_CPU(u64, ivh_cs_clears);
DEFINE_PER_CPU(u64, ivh_cs_stamp_overwrote);
DEFINE_PER_CPU(u64, ivh_cs_check_calls);
DEFINE_PER_CPU(u64, ivh_cs_abstain_noprev);
DEFINE_PER_CPU(u64, ivh_cs_abstain_rot);
DEFINE_PER_CPU(u64, ivh_cs_abstain_tag);
DEFINE_PER_CPU(u64, ivh_cs_abstain_skew);
DEFINE_PER_CPU(u64, ivh_cs_abstain_young);
DEFINE_PER_CPU(u64, ivh_cs_abstain_nohz);
DEFINE_PER_CPU(u64, ivh_cs_long_hold);
DEFINE_PER_CPU(u64, ivh_cs_healthy_long);
DEFINE_PER_CPU(u64, ivh_cs_fired);
DEFINE_PER_CPU(u64, ivh_cs_ep_events);
DEFINE_PER_CPU(u64, ivh_cs_ep_events_by_end[IVH_CS_EP_NR]);
DEFINE_PER_CPU(u64, ivh_cs_ep_cycles[IVH_CS_EP_NR]);
DEFINE_PER_CPU(u64, ivh_cs_ep_hist[IVH_CS_EP_NR][IVH_BEAT_AGE_HIST_BUCKETS]);
DEFINE_PER_CPU(u64, ivh_cs_tenure_cycles[2]);
DEFINE_PER_CPU(u64, ivh_cs_tenure_hist[2][IVH_BEAT_AGE_HIST_BUCKETS]);
DEFINE_PER_CPU(u64, ivh_cs_prev_hold_hist[IVH_BEAT_AGE_HIST_BUCKETS]);
DEFINE_PER_CPU(u64, ivh_cs_abstain_tenure);
DEFINE_PER_CPU(u64, ivh_cs_abstain_hashed);
DEFINE_PER_CPU(u64, ivh_cs_abstain_late);
DEFINE_PER_CPU(u64, ivh_cs_abstain_retag);
DEFINE_PER_CPU(u64, ivh_cs_tenure0_enter);
DEFINE_PER_CPU(u64, ivh_cs_tenure0_hashed);
DEFINE_PER_CPU(u64, ivh_cs_tenure0_hashed_released);
DEFINE_PER_CPU(u64, ivh_cs_tenure0_late);
DEFINE_PER_CPU(u64, ivh_cs_shadow_gate_pass_released);
DEFINE_PER_CPU(u64, ivh_cs_prompt_hist[IVH_BEAT_AGE_HIST_BUCKETS]);
```

**(iii) Calibration**, next to `ivh_pv_beat_calibrate()` at `:1556`:

```c
/*
 * One tick in raw TSC cycles. tsc_khz * 1000 / HZ = cycles-per-second / HZ.
 * At tsc_khz = 2200000, HZ = 1000: 2 200 000 cycles.
 *
 * Derived rather than hardcoded for the same reason ivh_pv_beat_threshold is:
 * the knob has to survive a different host. If tsc_khz is 0 here the compiled
 * 2.2 GHz default stands and the pr_info says so -- a wrong tick period makes
 * the predicate more eager or more conservative, never unsafe, because the
 * liveness term is what decides and this only decides when to consult it.
 */
static int __init ivh_cs_tick_calibrate(void)
{
	if (tsc_khz)
	{
		ivh_cs_tick_period = (unsigned long)((u64)tsc_khz * 1000ULL / HZ);
		ivh_cs_prompt_cycles = (unsigned long)((u64)tsc_khz *
					IVH_CS_PROMPT_US / 1000ULL);
	}

	pr_info("IVH: CS tick period = %lu cycles (HZ=%d, tsc_khz=%u), owed-tick margin = %lu\n",
		ivh_cs_tick_period, HZ, tsc_khz, ivh_cs_owed_ticks);
	return 0;
}
late_initcall(ivh_cs_tick_calibrate);
```

**(iv) Bounds and validating handlers**, near `:1848-1860`:

```c
static unsigned long ivh_cs_owed_min = 1UL;
static unsigned long ivh_cs_owed_max = 64UL;
/*
 * A tick period is a physical constant of the boot, not a free parameter; the
 * sysctl is writable only so a sweep can deliberately detune it. Floor at
 * 1000 cycles so a typo cannot turn the predicate into "fire always".
 */
static unsigned long ivh_cs_tick_min = 1000UL;
static unsigned long ivh_cs_tick_max = 1UL << 32;
/* Promptness bound: floor so a typo cannot abstain on everything, ceiling at
 * one tick (beyond that the gate is meaningless next to ivh_cs_owed_ticks). */
static unsigned long ivh_cs_prompt_min = 100UL;
static unsigned long ivh_cs_prompt_max = 2200000UL;
```

Three handlers on the `ivh_pv_proc_*` pattern (`struct ctl_table tmp = *table;`
→ `proc_doulongvec_minmax(&tmp, …)` → validate → `pr_err` → `WRITE_ONCE`):

- `ivh_cs_proc_head_probe`: refuse `1` unless `ivh_cs_owner_enable == 1`
  (otherwise every check abstains on the tag and the run measures nothing);
  refuse `1` while `ivh_pv_rot_enable != 0`.
- `ivh_cs_proc_head_bail`: refuse `1` unless `ivh_cs_head_probe == 1` **and**
  `ivh_adaptive_mode == IVH_MODE_ADAPTIVE (2)`; refuse while
  `ivh_pv_rot_enable != 0`.
- `ivh_pv_proc_rot_enable` (**new wrapper on an existing knob**): refuse `1`
  while `ivh_cs_head_probe` or `ivh_cs_head_bail` is set. This is the other half
  of the interlock; without it the two can be raced.

**(v) Table entries** in `ivh_pv_sysctls[]` (`:1862-2008`), `.maxlen =
sizeof(unsigned long)`, `.mode = 0644`:

| procname | handler | extra1/extra2 |
|---|---|---|
| `ivh_cs_owner_enable` | `proc_doulongvec_minmax` | — |
| `ivh_cs_owner_clear` | `proc_doulongvec_minmax` | — |
| `ivh_cs_head_probe` | `ivh_cs_proc_head_probe` | — |
| `ivh_cs_head_bail` | `ivh_cs_proc_head_bail` | — |
| `ivh_cs_owed_ticks` | `proc_doulongvec_minmax` | `&ivh_cs_owed_min` / `_max` |
| `ivh_cs_tick_period` | `proc_doulongvec_minmax` | `&ivh_cs_tick_min` / `_max` |
| `ivh_cs_prompt_cycles` | `proc_doulongvec_minmax` | `&ivh_cs_prompt_min` / `_max` |

and change the existing `ivh_pv_rot_enable` entry's `.proc_handler` to the new
`ivh_pv_proc_rot_enable`.

### 3.8 `/root/ivh_tools/read_ivh_counters.py`

Counters are not exported by the kernel at all — no debugfs, no
`{name, ptr}` table. The Python resolves symbols through `/proc/kallsyms` and
reads `/proc/kcore`, so **every new name must be added by hand**, and array
shapes must be mirrored.

```python
IVH_CS_EP_NR = 3            # must match IVH_CS_EP_NR in <asm/ivh_tsc_beat.h>
IVH_CS_EP_NAMES = ["ACQUIRED", "HOLDER_CHANGED", "EXHAUST"]
```
Append to `DEFAULT_COUNTERS`: `ivh_cs_stamps`, `ivh_cs_clears`,
`ivh_cs_stamp_overwrote`, `ivh_cs_check_calls`, `ivh_cs_abstain_noprev`,
`ivh_cs_abstain_rot`, `ivh_cs_abstain_tag`, `ivh_cs_abstain_skew`,
`ivh_cs_abstain_young`, `ivh_cs_abstain_nohz`, `ivh_cs_long_hold`,
`ivh_cs_healthy_long`, `ivh_cs_fired`, `ivh_cs_ep_events`,
`ivh_cs_abstain_tenure`, `ivh_cs_abstain_hashed`, `ivh_cs_abstain_late`,
`ivh_cs_abstain_retag`, `ivh_cs_tenure0_enter`, `ivh_cs_tenure0_hashed`,
`ivh_cs_tenure0_hashed_released`, `ivh_cs_tenure0_late`,
`ivh_cs_shadow_gate_pass_released`.
Append to `ARRAY_COUNTERS`:
```python
    "ivh_cs_ep_events_by_end": (IVH_CS_EP_NR,),
    "ivh_cs_ep_cycles":        (IVH_CS_EP_NR,),
    "ivh_cs_ep_hist":          (IVH_CS_EP_NR, IVH_BEAT_AGE_HIST_BUCKETS),
    "ivh_cs_tenure_cycles":    (2,),
    "ivh_cs_tenure_hist":      (2, IVH_BEAT_AGE_HIST_BUCKETS),
    "ivh_cs_prev_hold_hist":   (IVH_BEAT_AGE_HIST_BUCKETS,),
    "ivh_cs_prompt_hist":      (IVH_BEAT_AGE_HIST_BUCKETS,),
```
Mirror the same names into `/root/ivh_tools/phase0b_dump.py`'s `SCALARS` /
`ARRAYS`, which is what the harness uses for before/after deltas.

---

## 4. How this coexists with `ivh_pv_spin_threshold`

**The threshold is not removed, not reduced, and not touched.** They operate on
different timescales and answer different questions, and that is the point:

| | `ivh_pv_spin_threshold` | `is_cs_preempted()` |
|---|---|---|
| reacts in | ~45 µs (32768 iterations) | ~2–3 ms (`ivh_cs_owed_ticks` × 1 ms) |
| asks | "have I spun long enough that spinning is probably a mistake?" | "is the holder demonstrably not running?" |
| evidence | none — a timeout | positive: a tick that was owed and did not arrive |
| failure mode | halts on a *healthy* long hold and eats an IPI | none for healthy long holds, by construction |

At 32768 the threshold fires **first, essentially always** — 45 µs is 50× faster
than this predicate can possibly answer. So `is_cs_preempted()` only ever gets to
speak in one configuration: when the threshold is raised. That is not a
limitation, it is the intended use, and it is exactly the arm the brief says is
currently 23.1 % slower than IVH+AS:

> `three_arm_exhaust.sh` arm `noexh` sets `ivh_pv_spin_threshold = 16777216`
> (= `1<<24`, the `ivh_spin_thresh_max` clamp at `kvm.c:1860`), so
> `PV_BAIL_EXHAUST` essentially never fires and the head spins until it wins.

**The hypothesis this whole feature tests** is therefore precise and falsifiable:
*the `noexh` arm is slow because the head, having no adaptive signal, spins
through holder preemptions it could have slept through; give it that signal and
`noexh` + `is_cs_preempted` beats both `as` and `noexh`.* If Stage A's numbers
say the recoverable time is negligible, that hypothesis is dead and the 23.1 %
has some other cause — which is itself a useful finding.

Stage B's A/B must therefore be run at **both** threshold settings (§6.2), and a
win at 32768 alone would be surprising and should be disbelieved until explained.

---

## 5. STAGE A — detect only, zero behaviour change

### 5.1 What ships

Everything in §3 **except** `ivh_cs_head_bail`, the Stage-B counters, and the
early `break`. **Amended:** `ivh_cs_owner_clear`'s call site in
`<asm/qspinlock.h>` (§3.5) now ships in Stage A, default off. Stage A is measured
in two configurations in one boot (§1.4):
- **A-clr** (`ivh_cs_owner_clear=1`) is airtight and authoritative for the kill
  criterion.
- **A-gate** (`=0`) is airtight only on HASHED+witness tenures, bounded on
  RUNNING+promptness tenures, and abstains on tenure ≥ 1.

### 5.2 The behaviour-neutrality argument, and how it is checked

Three claims, each mechanically verifiable rather than asserted:

1. **No new control-flow edge exists.** `ivh_cs_head_probe_one()` contains no
   `break`, no `goto`, no `return` that changes the caller's path, and writes
   nothing outside `this_cpu` counters and the caller's own stack locals. Grep
   the diff for `break`/`goto`/`continue` inside the added hunks: there must be
   none.
2. **The trip count is unchanged.** `ivh_head_spin_iters_sum /
   ivh_head_spin_attempts` must equal `ivh_pv_spin_threshold` **exactly** — the
   invariant `qspinlock_paravirt.h:1567-1575` already documents ("only reached
   via natural exhaustion, so this should average almost exactly
   SPIN_THRESHOLD"). Stage A must not perturb it in either arm.
3. **At defaults the added instructions are two predicted branches.** One
   `READ_ONCE(ivh_cs_owner_enable)` at `qspinlock.c:462`, one register test of
   `probe` in the spin loop (read once per tenure, not per iteration). This is
   the identical posture `ivh_beat_publish_in_spin()`
   (`qspinlock_paravirt.h:499-508`) and `ivh_lock_set_holder()`
   (`ivh_lock_holder.h:70-79`) already carry, both of which this project has
   already certified as safe to leave compiled in permanently. The clear gate in
   `queued_spin_unlock()` adds a third `READ_ONCE` + predicted branch on the
   uncontended unlock path (A6 covers it). With the probe **armed**, each head
   tenure also pays one `smp_mb()` (a LOCK-prefixed no-op) plus one `rdtsc`, in
   `ivh_cs_tenure_gate()`. That is a timing change, not a control-flow change,
   and A7 covers it.

### 5.3 Counters and what each proves

| counter | proves |
|---|---|
| `ivh_cs_stamps` | the stamp site at `:462` is live and on the real path |
| `ivh_cs_stamp_overwrote` | how often a nested hold clobbers an outer one — sizes the blind spot |
| `ivh_cs_check_calls` | the head reaches the probe; denominator for everything |
| `ivh_cs_abstain_noprev` | the size of the role-B gap (head with no predecessor) |
| `ivh_cs_abstain_tag` | coverage loss from nesting / unstamped acquisitions |
| `ivh_cs_abstain_skew` | **TSC synchrony across vCPUs.** Must be ~0 or the design is unsound |
| `ivh_cs_abstain_young` | the healthy bulk; should dominate by orders of magnitude |
| `ivh_cs_abstain_nohz` | **must be exactly 0**; nonzero ⇒ the cmdline changed, run is void |
| `ivh_cs_abstain_rot` | the rotation interlock fired; must be 0 with `rot_enable=0` |
| `ivh_cs_long_hold` | the form-0 population — the FP-audit denominator |
| `ivh_cs_healthy_long` | **the tick term doing its job** — long holds correctly exonerated |
| `ivh_cs_fired` | raw sampled fires (the over-countable number) |
| `ivh_cs_ep_events` | **distinct stalls** (the honest number) |
| `ivh_cs_ep_cycles[ACQUIRED]` | **the recoverable time** — the only number the decision turns on |
| `ivh_cs_ep_hist[*]` | the duration distribution, which rotation never measured |
| `ivh_cs_tenure_hist[0..1]` | does detection predict a long head wait at all? |
| `ivh_cs_prev_hold_hist` | the true contended-hold-duration population (observer-side under A-gate, holder-side under A-clr) |
| `ivh_cs_abstain_hashed` | per-check abstains: halted-at-handoff tenure whose `_Q_SLOW_VAL` witness failed (the first-version hole) |
| `ivh_cs_abstain_late` | per-check abstains: running-at-handoff tenure that failed the promptness gate |
| `ivh_cs_abstain_tenure` | per-check abstains: tenure ≥ 1 without the clear |
| `ivh_cs_abstain_retag` | tag changed between the two reads; expected ≈ 0 |
| `ivh_cs_tenure0_enter` / `_hashed` / `_hashed_released` / `_late` | **per-tenure** sizes of each soundness case: how many first tenures start HASHED, how many of those had `prev` already gone, and how many RUNNING tenures were late |
| `ivh_cs_shadow_gate_pass_released` | (A-clr only) residual-race tenures that A-gate's promptness gate would have wrongly admitted |
| `ivh_cs_prompt_hist` | handoff-to-pending latency distribution; tunes `ivh_cs_prompt_cycles` |
| `ivh_cs_clears` | the clear is live under A-clr; `stamps ≈ clears + overwrote` |

### 5.4 Duration measurement — why it cannot over-count

The brief is right that this is where the project was burned. `ivh_head_yield_ok
_tier2_spinning` is incremented inside `pv_wait_early()`'s
`(loop & PV_PREV_CHECK_MASK) != 0` gate (`qspinlock_paravirt.h:545-546`), so a
single 1 ms stall against a 32768-iteration budget is sampled up to 128 times by
the same waiter. It reported 3 104/s, the duration was never measured, and the
idea was worth ≈ 0.

Three structural defences, not conventions:

1. **Episodes are keyed on the holder's acquisition TSC.** Repeat fires against
   the same `acq` extend the open episode; they never open a new one. Sampling
   rate cannot inflate `ivh_cs_ep_events`.
2. **A non-firing sample does not close the episode.** Otherwise a momentary
   miss (say the holder published from an inner lock's spin loop) would split one
   stall into several and re-create the very inflation being avoided.
3. **`ivh_cs_fired / ivh_cs_ep_events` is reported as the over-count factor.**
   That number is the direct, quantitative post-mortem on the rotation mistake.
   Expect it in the range 10–128; if it is ~1, the sampling is not hitting the
   same stall twice and something is wrong with the keying.

Three close reasons keep truncated measurements from being mistaken for real
ones: only `ACQUIRED` is a measurement, `HOLDER_CHANGED` is an upper bound, and
`EXHAUST` is a lower bound. **Do not sum them.**

### 5.5 The false-positive audit

The claim being audited is: *"`is_cs_preempted()` has no false positives from
long critical sections, because a holder running a 5 ms CS still ticks."*

**Named comparison, primary: `ivh_cs_healthy_long` against `ivh_cs_long_hold`.**

- `ivh_cs_long_hold` is every hold older than the margin — i.e. exactly what
  form 0 (`ivh_tsc_full_redesign_build_plan_2026-07-29.md` §1.2) would fire on.
- `ivh_cs_healthy_long` is the subset that had ticked since acquiring — the ones
  form 2 exonerates and form 0 would have got wrong.
- **`ivh_cs_healthy_long / ivh_cs_long_hold` is the false-positive rate this
  predicate removes.** It must be materially above zero. A value near 0 means
  either (a) long healthy holds do not exist on this workload — implausible under
  hackbench, and refutable from `ivh_cs_prev_hold_hist` — or (b) the liveness
  term is inert and form 2 has silently degenerated into form 0, in which case
  the central claim is unproven and Stage B must not proceed.

**Secondary: `ivh_cs_ep_hist[ACQUIRED]` against `ivh_cs_prev_hold_hist`.**
`prev_hold_hist` is the real distribution of contended hold durations, sampled on
the observer side at zero release-path cost. If the detected episodes sit inside
its bulk, we are firing on ordinary long holds. If they sit in a separate mode
above its p99.9, we are firing on anomalies. This is the population-correctness
point `ivh_tsc_full_redesign_build_plan_2026-07-29.md` §0.2.3 already insisted on
for the CS threshold.

**Explicitly NOT used as ground truth: `vcpu_is_preempted()`.** Steal-time is
untrustworthy in this CVM and `vcpu_is_preempted()` is hardwired false here —
`is_wait_preempted()`'s own comment (`qspinlock_paravirt.h:404-409`) records that
`src==1`'s "ground truth" is meaningless on this host and that
`ivh_beat_age_hist_running/_preempted` "has never once been populated". Any
2×2 agreement matrix against it would be noise. Host-side ground truth must come
from the host, from the user.

**Known false-positive class that these audits will NOT catch**, stated rather
than hidden: a holder inside a multi-millisecond IRQ-off region publishes no
beat and is indistinguishable from a preempted one. The realistic sources are
`stop_machine()` (`kernel/stop_machine.c:232-234` — all 16 CPUs at once, 1–20 ms,
triggered by text patching, static-key updates, module load, hotplug) and printk
to the serial console (`console=ttyS0`; ~7 ms per line at 115200). Economically
these are arguably *true* positives — the holder really is not progressing — but
they must be excluded from measurement (§6.4) because they are not the population
the feature targets.

### 5.6 Acceptance checks

| # | check | pass |
|---|---|---|
| A1 | plumbing live | `ivh_cs_stamps > 0` and `ivh_cs_check_calls > 0` |
| A2 | partition 1 | `check_calls == tenure + hashed + late + noprev + rot + tag + skew + young + long_hold`, **deviation exactly 0**, in both A-clr and A-gate |
| A3 | partition 2 | `long_hold == nohz + retag + healthy_long + fired`, **deviation exactly 0** |
| A4 | episode identity | `ep_events == sum(ep_events_by_end[])`, deviation 0; `fired >= ep_events` |
| A5 | trip count intact | `ivh_head_spin_iters_sum / ivh_head_spin_attempts == 32768` exactly, in **both** arms |
| A6 | instrumentation is free when off | ABBA hackbench, `(owner_enable=0, head_probe=0)` vs the previous kernel: within ±1 % |
| A7 | instrumentation is cheap when on | ABBA hackbench, `(1,1)` vs `(0,0)` **same kernel**: within ±1 %. If not, Stage B's numbers are confounded — fix before proceeding |
| A8 | nohz sanity | `ivh_cs_abstain_nohz == 0` **and** `cat /sys/devices/system/cpu/nohz_full` is `(null)` **and** `/proc/cmdline` still has `nohz=off` |
| A9 | TSC sanity | `abstain_skew / check_calls < 1e-4` |
| A10 | rotation interlock | `ivh_cs_abstain_rot == 0` with `ivh_pv_rot_enable=0`; writing `ivh_pv_rot_enable=1` while `head_probe=1` returns `EINVAL` and logs |
| A11 | Idea-2 invariant preserved | `ivh_head_woke_yielded == 0` still |
| A12 | coverage reported (not pass/fail) | `abstain_tag / (check_calls − young)`, `abstain_noprev / check_calls`, and per tenure `tenure0_hashed / tenure0_enter`, `tenure0_hashed_released / tenure0_hashed`, `tenure0_late / (tenure0_enter − tenure0_hashed)` |
| A13 | witness sanity | under A-clr, a HASHED tenure whose witness **passed** must never see a NULL tag at its first check. Log `ivh_cs_abstain_tag` separately for witness-passed HASHED tenures (debug build only) or, cheaper, confirm `tenure0_hashed_released` under A-clr ≈ under A-gate within noise |
| A14 | residual race sized | `ivh_cs_shadow_gate_pass_released / ivh_cs_ep_events` (A-clr). Below 1 %: A-gate is admissible for Stage B. Above: Stage B must ship with the clear |
| A7b | clear cost | ABBA hackbench, `owner_clear=1` vs `0` with `owner_enable=1`, `head_probe=0`: within ±1 %. This isolates the unlock-fastpath cost |
| A15 | A-clr vs A-gate agreement | `ep_events`, `recoverable_fraction` and `healthy_long/long_hold` agree between the two configurations after restricting A-clr to tenure-0 checks. A-gate materially **above** A-clr means the gates are admitting stale stamps |

A6 and A7 are the ones people skip. Do not skip them: Stage B's entire claim is a
few percent of hackbench, and an instrumentation cost of the same magnitude
would make the result meaningless in either direction.

### 5.7 Kill criterion

Let `T` = measured wall seconds, `N` = 16, `F` = `tsc_khz × 1000` = 2.2e9.

> **recoverable_fraction = Σ_cpus ivh_cs_ep_cycles[ACQUIRED] / (N × F × T)**

the share of total vCPU time spent by a queue head spinning *after* a holder
preemption was detectable and *before* it acquired. It is the **ceiling** on what
Stage B can win — a perfect oracle recovers all of it and nothing more.

**STOP — do not build Stage B — if any of:**

- **K1.** `ivh_cs_ep_events` rate < 100/s aggregated across 16 CPUs. Too rare to
  move a benchmark.
- **K2. `recoverable_fraction < 0.01`.** *This is the check handoff rotation
  skipped, and it is the headline.* 3 104 events/s of something with an
  unmeasured duration was worth ≈ 0. A 1 % ceiling is inside hackbench's
  run-to-run noise and cannot produce the several-percent effect that motivated
  this work. Note the brief's own arithmetic: 3 104/s × 100 µs would be 27 % of
  wall time, which is implausible — so expect the durations to be *short* and
  take K2 seriously.
- **K3.** p50 of `ivh_cs_ep_hist[ACQUIRED]` is below the **measured** pv_wait
  round trip on this host. Compute that round trip in the same run from the
  existing counters: `Σ ivh_node_halt_cycles[c] / Σ ivh_node_halt_events[c]`
  over `c ∈ {TIER1, TIER1_AGREED, TIER1_DISAGREED, TIER2, EXHAUST}`
  (`qspinlock_paravirt.h:526-528`). Do not assume a number. If the median
  episode is shorter than a halt-and-wake, halting strictly loses.
- **K4.** `p50(ivh_cs_tenure_hist[1]) < 2 × p50(ivh_cs_tenure_hist[0])` —
  detection carries no information about how long the head will actually wait, so
  acting on it is acting on noise.
- **K5 (not an automatic kill, but a hard gate).**
  `ivh_cs_healthy_long / ivh_cs_long_hold < 0.05` — the liveness term exonerates
  almost nothing, the predicate is form 0 in disguise, and its no-false-positives
  claim is unproven. Explain or fix before proceeding.
- **K6 (fix, do not conclude).**
  `(abstain_tag + abstain_noprev) > 0.9 × (check_calls − abstain_young)` —
  coverage is too low to judge anything. Add sites A4/A6 and/or arm the clear,
  then re-measure. A kill decision taken here would be a measurement artefact.
- **K7 (coverage of the no-clear design).** If
  `tenure0_hashed / tenure0_enter` is large (most first tenures start
  halted-at-handoff) **and** `tenure0_hashed_released / tenure0_hashed` is large
  (the halted head was usually woken by `prev`'s release kick, i.e. `prev` was
  already gone), then A-gate's first-tenure coverage is too small to matter.
  The coordinator reports ~1.4 M halts per 55 s hackbench run; I have not
  re-measured it. That is *not* a kill of the feature. It kills the zero-cost
  A-gate variant, and Stage B must use A-clr and pay A7b. The feature itself is
  then judged on A-clr numbers alone. Note the physical implication: a head that
  was halted until `prev` released has, by construction, nothing to detect on
  that tenure. The interesting population is heads spinning **while** `prev`
  holds, which is RUNNING-at-handoff or HASHED-woken-by-tick.

A `recoverable_fraction` of 1–3 % is **not** a green light. It is "measure a
second workload before spending a boot on Stage B" — qlockbench and AFL are the
project's other two, per `ivh_benchmark_reranking_2026-09-14.md`.

---

## 6. STAGE B — act on it

**Only if Stage A passes every acceptance check and no kill criterion fires.**

### 6.1 What the head does

**Halt early, via the existing path.** Break out of the spin loop and let the
already-audited `clear_pending()` → `pv_hash()` → `xchg(_Q_SLOW_VAL)` →
`pv_wait()` sequence run exactly as exhaustion does today. No new mechanism, no
new state, no new failure mode.

```c
 			if (unlikely(probe) &&
 			    (loop & PV_PREV_CHECK_MASK) == 0) {
-				ivh_cs_head_probe_one(lock, pp, cs_gate,
-						      &ep_acq, &ep_start, &ep_any);
+				bool hit = ivh_cs_head_probe_one(lock, pp,
+						cs_gate, &ep_acq, &ep_start,
+						&ep_any);
+
+				if (hit && bail) {
+					this_cpu_inc(ivh_cs_head_bailed);
+					cause = IVH_CS_HALT_CS;
+					break;
+				}
 			}
```
with `bail = probe && READ_ONCE(ivh_cs_head_bail) &&
READ_ONCE(ivh_adaptive_mode) == IVH_MODE_ADAPTIVE;` read once per tenure
alongside `probe`, and `cause` initialised to `IVH_CS_HALT_EXHAUST` at the top of
each tenure.

**The accounting invariant must be repaired in the same patch.** Today
`qspinlock_paravirt.h:1567-1575` is documented as "only reached via natural
exhaustion (`loop == 0` here) … this path has no early-bail logic in any
mechanism". An early `break` falsifies that. Split it:

```c
-		this_cpu_add(ivh_head_spin_iters_sum, threshold - loop);
-		this_cpu_inc(ivh_head_spin_attempts);
+		/*
+		 * Stage B introduces the first early exit this loop has ever
+		 * had, which falsifies the "loop == 0 here" invariant this
+		 * block was built on. Split rather than blended: the
+		 * exhaustion accumulators keep meaning exactly what they meant
+		 * (and keep averaging exactly SPIN_THRESHOLD, which is still
+		 * the accounting sanity check), and the bail population gets
+		 * its own pair. Behaviour-identical when ivh_cs_head_bail == 0,
+		 * because loop is then always 0 here.
+		 */
+		if (loop) {
+			this_cpu_add(ivh_head_spin_iters_bail_sum, threshold - loop);
+			this_cpu_inc(ivh_head_spin_bail_attempts);
+		} else {
+			this_cpu_add(ivh_head_spin_iters_sum, threshold - loop);
+			this_cpu_inc(ivh_head_spin_attempts);
+		}
```

**Measure the halt, do not assume it helped.** Bracket the existing
`pv_wait(&lock->locked, _Q_SLOW_VAL)` at `:1627` exactly as `pv_wait_node()`
brackets its own (`:879-881`), and record by cause:

```c
+		halt_tsc = ivh_raw_tsc();
 		pv_wait(&lock->locked, _Q_SLOW_VAL);
+		{
+			u64 d = ivh_raw_tsc() - halt_tsc;
+
+			this_cpu_add(ivh_head_halt_cycles[cause], d);
+			this_cpu_inc(ivh_head_halt_events[cause]);
+			this_cpu_inc(ivh_head_halt_hist[cause][ivh_cs_bucket(d)]);
+		}
```

This is what tells you *why* a win or a loss happened: a CS-caused halt that is
systematically **longer** than an exhaustion halt means the head slept past the
release and the wake path is the problem, not the detector.

**Rejected alternatives, with reasons:**

- **Directed yield to the holder.** `KVM_HC_SCHED_YIELD` is reachable
  (`arch/x86/kernel/kvm.c:650`) and donating our slice to a preempted holder is
  the textbook LHP mitigation, strictly better than halting *if* the host honours
  it. But it is a new hypercall on a new path, its behaviour on a TD is not
  established in this tree, and it would confound the first A/B of the detector
  itself. Defer to a Stage C, and only after Stage B shows the detector is sound.
- **Migration.** Out of scope: migration is deliberately off while adaptive
  spinning is isolation-tested, and nothing here changes that.
- **Setting `HEAD_YIELDED`.** See §3.6 — it is reserved and its zero-ness is
  another design's acceptance check.
- **Removing or lowering `ivh_pv_spin_threshold`.** See §4 — it stays.

### 6.2 Sysctl surface

| knob | default | effect |
|---|---|---|
| `ivh_cs_owner_enable` | 0 | arms the stamp at `qspinlock.c:462` |
| `ivh_cs_head_probe` | 0 | arms detection + all counters (requires `owner_enable=1`) |
| `ivh_cs_owed_ticks` | 2 | margin in ticks, 1–64, sweepable |
| `ivh_cs_tick_period` | derived | cycles per tick, override for sweeps |
| **`ivh_cs_head_bail`** | **0** | **the only knob that changes behaviour** (requires `head_probe=1` and `adaptive_mode=2`) |
| `ivh_cs_owner_clear` | 0 | arms the unlock-path clear (compiled since Stage A); makes the tag authoritative on every tenure and bypasses the HASHED/late/tenure gates |
| `ivh_cs_prompt_cycles` | derived (9 µs) | promptness bound for A-gate RUNNING tenures; re-set from `ivh_cs_prompt_hist` |

Every flip is an `echo`, inside one boot. That is deliberate and it is this
project's standing posture: bundle the compilation, serialise the *authority*.

### 6.3 The A/B — ABBA-counterbalanced, not fixed-order

A fixed arm order previously fabricated a 14 % effect in this project against a
true order effect of ~3.5 %. `three_arm_exhaust.sh` interleaves
(`for r; do for arm in pv as noexh`) which is better than blocking by arm, but it
is still **fixed order within every round**, so any systematic drift within a
round maps straight onto the arm contrast. That must be replaced.

**Arms** (all at `spin_mode 2`, `ivh_cs_owner_enable=1`, `ivh_cs_head_probe=1`,
`ivh_pv_rot_enable=0`, migration unchanged from the existing IVH+AS arm):

- **A** — `ivh_cs_head_bail=0` (detect only)
- **B** — `ivh_cs_head_bail=1`

**run at both threshold settings**, because §4 predicts the effect lives at the
raised one:
- **T32k** — `ivh_pv_spin_threshold=32768` (expect ≈ no difference; a win here
  would be surprising and should be disbelieved until explained)
- **Tmax** — `ivh_pv_spin_threshold=16777216` (the `noexh` arm, the one measured
  23.1 % slower than IVH+AS — **this is where the hypothesis lives**)

**Design.** Blocks of 4 reps, alternating `ABBA` / `BAAB`, ≥ 6 blocks per
threshold setting (48 hackbench runs per setting):

```bash
ORDERS=(ABBA BAAB)
for blk in $(seq 1 "$BLOCKS"); do
  ord=${ORDERS[$(( blk % 2 ))]}
  for (( i=0; i<4; i++ )); do
      arm=${ord:$i:1}
      set_arm "$arm"
      run_rep "$blk" "$i" "$arm"      # emit blk,pos,arm,time,counters...
  done
done
```

Every rep emits its position `i` in the block. Analyse **paired within block**,
and additionally fit `time ~ arm + position` and report the residual position
coefficient. **Require `|arm effect| > 2 × |position effect|` before claiming
anything.** Also carry the existing `pv` arm once per block as a drift anchor —
if `pv` moves between blocks, the host is not stable and the run is void.

Start from `/root/ivh_tools/three_arm_exhaust.sh`: its `set_arm()`/`spin_mode`
setup, its per-row re-read of the live sysctls into the CSV (so the row
self-documents that the arm actually took), and its `phase0b_dump.py` delta
harness are all correct and should be kept verbatim. Replace only the
`for r; do for arm in …` loop with the block structure above, and add
`ivh_cs_head_bail` / `ivh_cs_owner_enable` / `ivh_cs_head_probe` to the
readback columns.

Copy `phase1_rotate_run.sh`'s two safety habits: a `trap cleanup EXIT` that
restores every knob to 0 however the run ends, and a mandatory 10 s `smoke` mode
before any long run.

### 6.4 Measurement hygiene (mandatory, and specific to this box)

- **`dmesg -n 1` for the duration, restored after.** `console=ttyS0` at 115200 is
  on the command line; one printk'd line is ~7 ms of IRQ-off time on the emitting
  CPU, which both perturbs the timing and injects genuine false detections.
  Assert `dmesg` gained no lines across the run and void it if it did.
- **Re-assert the environment before every block**, the house idiom from
  `phase0b_idle_run.sh`: `[[ -e /proc/sys/kernel/ivh_cs_head_probe ]] ||
  fail "wrong kernel? ($(uname -r))"`, `ivh_pv_preempt_src == 2`,
  `ivh_pv_beat_threshold == 220000`, `vcap_probe` and `MY_ivh_atc` running. A
  fallback boot silently gives an older kernel where the feature is inert and
  every arm is identical.
- **Never cite in-guest steal readings.** Host-side ground truth must come from
  the host.

---

## 7. Open questions — flagged, not guessed

1. **Prior-art reconciliation is a decision someone has to make, not me.**
   `include/linux/ivh_lock_holder.h` declares this feature by name and
   `ivh_tsc_full_redesign_build_plan_2026-07-29.md` §3.3 specifies a different
   implementation of it, one third of which is merged. Does this plan supersede
   that, and does the 4 MB inert holder table stay? My recommendation is in §0.1
   (keep it allocated, do not use it, cross-reference it) but it is a call about
   project direction.
2. **`ivh_head_waiter_adaptive_spinning_design_2026-09-14.md` §1–§2 is now partly
   wrong** (§0.2). It should be amended rather than left to mislead, since it is
   dated today and reads as current.
3. **Unmeasured: the stamp's cost.** I estimate ≤ 60 cycles gate-on at
   `qspinlock.c:462` (out-of-line call + `rdtsc` + two same-line per-CPU stores +
   two counters), against a path that has already spun through at least one MCS
   handoff. That is an estimate. Acceptance check A7 measures it; do not ship a
   Stage-B number without A7.
4. **Resolved (guest side): `RDTSC` does not trap into this kernel under TDX.**
   `virt_exception_kernel()` (`arch/x86/coco/tdx/tdx.c:840-860`) dispatches
   `#VE` only for HLT, MSR read/write, CPUID, EPT violation and I/O. Any other
   exit reason hits `pr_warn("Unexpected #VE: %lld\n")` and fails the fault.
   This kernel calls `rdtsc()` on every tick (`account_process_tick()`) and from
   both qspinlock spin loops, and `dmesg | grep -c "Unexpected #VE"` reads **0**
   on the running boot. `tdx_early_init()` also forces
   `X86_FEATURE_TSC_RELIABLE` (`tdx.c:1126`, "TSC is the only reliable clock in
   TDX guest"). So RDTSC runs without a `#VE`. **Not verifiable from the guest
   tree:** whether the TDX module itself exits to the host VMM on RDTSC. Per the
   TDX module specification it virtualises TSC in hardware (offset/scaling)
   without an exit, but that is the spec, not this tree. The measured
   per-`rdtsc` cost still belongs in A7.
5. **Unresolved: whether nesting matters.** `ivh_cs_stamp_overwrote` and
   `ivh_cs_abstain_tag` will say. If the tag-abstain rate is high, the fix is a
   2-deep per-CPU stack indexed by a depth counter — but building that before
   check A12 says it is needed would be speculative.
6. **Largely superseded by §1.4.** The clear is no longer just about tenure ≥ 1
   coverage. It is the only airtight close of the RUNNING-at-handoff residual
   race (§1.2 c-RUNNING). Whether Stage B can run without it is decided by A14
   (residual race size), A15 (A-clr vs A-gate agreement), K7 (HASHED share) and
   A7b (its cost), all measured in the Stage A boot.
9. **Unmeasured: the promptness bound.** The 9 µs default is an estimate of the
   handoff → `pv_wait_node()` exit → `set_pending()` path with an IRQ-length
   margin. `ivh_cs_prompt_hist` replaces the guess with data. Whatever the bound,
   the gate narrows the race and does not close it.
7. **`ivh_pv_beat_threshold` is 220000 live but 3300000 after any reboot**
   (`kvm.c:1395`, reset by `ivh_pv_beat_calibrate()` at `:1556`; moved to 220000
   only by `spin_mode 2`/`4`). This predicate does not read it — deliberately,
   §3.6 — but anything comparing against tier 2 in the same run must re-assert it.
8. **Not treated as a gap:** migration is off on purpose while adaptive spinning
   is isolation-tested.

---

## 8. Ordered task list

**Stage A — build**

1. `arch/x86/include/asm/ivh_tsc_beat.h`: add `#include <linux/ivh_lock_holder.h>`
   (§3.1 i); add `struct ivh_cs_owner` + `DECLARE_PER_CPU_ALIGNED` + the
   `ivh_cs_tick_period` / `ivh_cs_owed_ticks` / `ivh_cs_prompt_cycles` externs
   (§3.1 ii); add the Stage-A counter declarations, including the tenure-gate
   abstains, `ivh_cs_tenure0_*`, `ivh_cs_shadow_gate_pass_released`,
   `ivh_cs_prompt_hist` and `ivh_cs_clears` (§3.1 iii).
2. `include/linux/ivh_lock_holder.h`: add `ivh_cs_owner_stamp()` /
   `ivh_cs_owner_release()` inline gates, the two `extern`s and the two `__`
   prototypes, plus the `#else` stubs (§3.2).
3. `arch/x86/kernel/ivh_lock_holder.c`: add `#include <asm/ivh_tsc_beat.h>` and
   the two worker bodies, including the holder-side `prev_hold_hist` sample in
   the clear (§3.3).
4. `arch/x86/kernel/kvm.c`: knobs (§3.7 i), storage + counter definitions
   (§3.7 ii), `ivh_cs_tick_calibrate()` `late_initcall` (§3.7 iii), bounds +
   `ivh_cs_proc_head_probe` + the `ivh_pv_proc_rot_enable` interlock wrapper
   (§3.7 iv), sysctl table entries including `ivh_cs_owner_clear` and
   `ivh_cs_prompt_cycles` (§3.7 v). The prompt bound is calibrated in
   `ivh_cs_tick_calibrate()`.
4b. `arch/x86/include/asm/qspinlock.h`: `ivh_cs_owner_release(lock)` in
   `queued_spin_unlock()` before `pv_queued_spin_unlock()` (§3.5). **Moved from
   Stage B.**
5. `kernel/locking/qspinlock.c`: `prev = NULL` at `:202`; pass `prev` at `:417`;
   widen `__pv_wait_head_or_lock` at `:155`; `ivh_cs_owner_stamp(lock)` after
   `set_locked()` at `:462` and (optional) at the `:453` A4 branch; add
   `#include <linux/tick.h>` (§3.4).
6. `kernel/locking/qspinlock_paravirt.h`: `is_cs_preempted()`, `ivh_cs_bucket()`,
   `ivh_cs_ep_close()`, `ivh_cs_tenure_record()`, `ivh_cs_tenure_gate()` and
   `ivh_cs_head_probe_one(…, u8 gate, …)` after `:478`; widen
   `pv_wait_head_or_lock()`'s signature and add `pp`; per-tenure locals +
   `entered_hashed` capture at `:1510-1511` + `probe` read; the
   `ivh_cs_tenure_gate()` call right after `set_pending()` at `:1548`; the sampled probe call in the spin loop; the EXHAUST
   close after the loop; the ACQUIRED close + `prev_hold_hist` sample at
   `gotlock:` (§3.6).
7. `/root/ivh_tools/read_ivh_counters.py` and `phase0b_dump.py`: new names and
   array shapes (§3.8).

**Stage A — verify (no reboot; the human reboots)**

8. `./scripts/config --set-str LOCALVERSION "-G-LOCK-29-cspreempt"`;
   `make olddefconfig` → expect "No change to .config" apart from LOCALVERSION.
9. Targeted first: `make -j16 arch/x86/kernel/kvm.o
   arch/x86/kernel/ivh_lock_holder.o kernel/locking/qspinlock.o`. Then
   `make -j16 2>&1 | tee /root/build.log`; `grep -i "error:" /root/build.log`
   must be empty. Re-run to confirm idempotence.
10. Grep the diff for `break`/`goto`/`continue` inside added hunks — must be none
    (§5.2 claim 1).
11. `make modules_install && make install && update-grub`; verify
    `/boot/vmlinuz-6.17.0-G-LOCK-29-cspreempt+` and the grub entry exist.
12. `grub-reboot "Advanced options for Ubuntu>Ubuntu, with Linux
    6.17.0-G-LOCK-29-cspreempt+"`. **Then stop and hand back to the user for the
    reboot.** Do not reboot.

**Stage A — measure (after the user reboots)**

13. Post-boot preflight: `uname -r`; `ls /proc/sys/kernel/ | grep ivh_cs`;
    `dmesg | grep "IVH: CS tick period"` must report 2200000 cycles;
    `cat /sys/devices/system/cpu/nohz_full` must be `(null)`.
14. `/root/spin_mode 2`. Verify `ivh_pv_beat_threshold == 220000`.
15. Acceptance A6: ABBA hackbench at `(owner_enable=0, head_probe=0)` against the
    G-LOCK-28 kernel.
16. Arm: `echo 1 > …/ivh_cs_owner_enable; echo 1 > …/ivh_cs_head_probe`. 10 s
    smoke run in **each** of A-gate (`ivh_cs_owner_clear=0`) and A-clr (`=1`);
    check A1–A4, A8–A11 in both.
17. Acceptance A7: ABBA hackbench `(1,1)` vs `(0,0)` on this kernel. Acceptance
    A7b: ABBA `owner_clear=1` vs `0` (with `owner_enable=1`, `head_probe=0`).
18. Full detect-only runs at `ivh_pv_spin_threshold=16777216` (the arm where the
    signal should live), ≥ 60 s each, `dmesg -n 1`. Run **A-clr first as the
    authoritative run**, then A-gate. Dump all counters for each.
19. Compute for each configuration: `recoverable_fraction`,
    `ivh_cs_fired/ivh_cs_ep_events`, `ivh_cs_healthy_long/ivh_cs_long_hold`, the
    measured pv_wait round trip, and p50 of `ep_hist[ACQUIRED]` vs
    `tenure_hist[0/1]` vs `prev_hold_hist`. Also compute the §5.6 A12–A15
    coverage/residual numbers and the K7 HASHED share. Set
    `ivh_cs_prompt_cycles` from `ivh_cs_prompt_hist` (e.g. its p99) and repeat
    A-gate once if it moved materially.
20. Apply §5.7 **to the A-clr numbers**. **If any kill criterion fires, write the negative result up and
    stop** — the same discipline that closed handoff rotation cheaply.
21. Sweep `ivh_cs_owed_ticks` 1..4 in the same boot to check the margin is not
    doing the work on its own.

**Stage B — only if Stage A passes**

22. Add the Stage-B counters, the early `break` + `bail` (keep the clear
    configuration A14/A15/K7 selected: A-gate only if all three allow it), the split exhaustion/bail accounting, and the head-halt
    duration recording (§6.1).
23. Add the `ivh_cs_head_bail` sysctl and the
    `ivh_cs_proc_head_bail` interlock (§6.2).
24. Build, stage with `grub-reboot`, **hand back for the reboot.**
25. Write the counterbalanced harness (§6.3) starting from
    `three_arm_exhaust.sh`, with `trap cleanup EXIT` and a smoke mode.
26. Run A/B at `Tmax` first (where the hypothesis lives), then `T32k` as a
    negative control. Report the arm effect, the position effect, and their
    ratio.
