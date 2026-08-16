# Follow-up: does TDX itself produce false-positive "steal"?

This is a follow-up to `cvm_setup/CVM_HACKBENCH_VARIANCE_INVESTIGATION.md`.
Read that first if you haven't — this picks up directly from its
conclusion ("no host corunner ever ran tonight, IVH correctly did
nothing"). That conclusion has a real hole in it, found by re-reading the
source and by a control experiment on the reference machine. Both are
below. Your job now: chase down *why* the synthetic busy-loop test showed
a capacity split, since the leading explanation is no longer "there must
be a real corunner" — it might be that TDX itself is being misread as
steal.

## The hole in the original conclusion

Your report used a synthetic test (8 busy-loops pinned to cpu0-7,
guest-local, no host corunner reachable) to confirm the *observation*
pipeline works: `cap_cont=457` vs `cap_clean=869`, a real split. You read
this as "the signal correctly detects asymmetric load." But re-read
`ivh_tick_steal_accumulate()` in `kernel/sched/core.c` (starts line 2385)
— its own design comment says the opposite of what a guest-only busy loop
should produce:

> "with a periodic tick, consecutive deliveries are exactly TICK_NSEC
> apart absent preemption... avail_c = TICK - idle <= TICK and excess_c
> <= 0 for any vCPU that is not being preempted"

And with `ivh_tks_idle_sub=0` (shipped, confirmed on this CVM):

> "the raw inter-delivery gap is already the delay, on an idle vCPU as
> much as a busy one"

**Busy and idle are supposed to look identical to this estimator, as long
as the host isn't actually preempting the vCPU.** A guest-local busy loop
with zero host interference should produce `excess_c ≈ 0` — no steal, no
capacity drop. It's specifically a *host preemption* detector, not a
guest-business detector. So your test producing a real split is evidence
*against* "there's no host contention anywhere," not for "the pipeline
works" — something at the host boundary reacted to those 8 vCPUs going
CPU-bound, even with no dedicated corunner tenant in the picture.

## Control experiment, just run on bench-18c-2 (not this machine)

To find out whether that reaction is TDX-specific or just "any real host
scheduling pressure, TDX or not," I ran the identical experiment on the
non-TDX reference machine: 8 busy-loops (`taskset -c N bash -c 'while
true; do :; done'`) pinned to its then-"clean" cpu group (8-15), 15s
settle, before/after capacity read.

```
before: cap_clean(8-15) = 787
after:  cap_clean(8-15) = 925   (busy loops pinned there)
```

**It went up, not down.** A guest-CPU-bound-but-not-host-preempted vCPU
read *cleaner*, matching the mechanism's own design exactly — busy ≠
steal on this normal KVM box. `cap_cont(0-7)`, driven by that machine's
real, independently-running corunner, was unaffected by the experiment
(343 before, 305 after — noise).

## What this means

The CVM's version of this exact experiment produced the opposite sign
from the control. That's the actual anomaly to explain, and "no real
contention exists" doesn't explain it — something made 8 CPU-bound guest
vCPUs look preempted on the CVM when the equivalent load did not look
preempted on a normal VM. Live hypothesis, not yet confirmed: **TDX's own
guest/host transition overhead — `#VE` exits, `TDCALL`s, memory
encryption/decryption latency — might scale with vCPU business in a way
that delays tick delivery just enough to trip this estimator**, entirely
independent of any other tenant's presence. If true, this CVM may
genuinely read more "steal" than a legacy VM under the same real-world
conditions, not because more host contention exists, but because TDX
itself costs more at the guest/host boundary and this estimator can't
currently tell that apart from preemption.

## What to actually do

1. **Check the hardfloor angle first — it's cheap and might independently
   explain the zero-migrations result from the busy-loop test**, no TDX
   theory required: `cap_clean=869` in your original test is *below*
   `IVH_CAP_HARDFLOOR=880`. If literally no CPU cleared the hardfloor
   during that run, that alone explains zero migrations — check the raw
   per-CPU `ivh_uc_cpu:` lines in `/proc/ivh_debug` at the moment of a
   repeat of that test, not just the two-group average.

2. **Re-run the busy-loop test with finer instrumentation**: watch
   `/proc/interrupts` (or anything else guest-visible that proxies
   guest/host transition rate — you don't have host access, but you may
   have guest-side counters) before/during/after pinning the loops. If
   transition-related activity spikes in lockstep with the capacity drop,
   that's real support for the TDX-overhead theory rather than a
   coincidence.

3. **Try a lighter, more gradual version of the synthetic load** (e.g. 2
   busy loops instead of 8, or partial CPU load via `stress-ng --cpu N
   --cpu-load 50` instead of a tight `while true` loop) and see if the
   capacity drop scales smoothly with guest load intensity. A smooth
   dose-response relationship between "how CPU-bound the guest workload
   is" and "how much steal gets reported" would be strong evidence for a
   guest-load-coupled false positive rather than genuine, load-independent
   host contention.

4. **Re-check whether a real corunner is running on `mars`** is still
   worth doing independently of all this — that question is orthogonal
   to whether the estimator has a TDX-specific bias, and both could be
   true at once (no corunner *and* the estimator over-reads TDX's own
   overhead as steal).

Report back with whichever of these actually moves — don't stop at the
first plausible-sounding one.
