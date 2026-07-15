# Hotlock + time_left reference

Written because the working tree state was worth capturing in one place.
**As of this writing the code below is NOT lost** — `git status` on
`rseq-port` still shows every file below as modified-but-uncommitted, and
`grep` confirms every function/struct/sysctl named here is present at the
line numbers given. Nothing here has been rebuilt or booted into during
this session; this doc is read-only w.r.t. the tree. If you're reading this
after a `git checkout`/`git reset` that actually did discard the working
tree, these line numbers won't match anymore and you're looking at real
loss — check `git reflog` first (see "If the code really is gone" at the
bottom).

Two independent mechanisms, easy to conflate because they share the word
"gate":

- **Hotlock** — decides *which locks* are worth intervening on (per-lock
  contention history).
- **`ivh_steal_imminent()` / time_left** — decides *when the current vCPU*
  is in enough danger of an imminent host steal to justify migrating away
  from it at all. Independent of any specific lock.

Both loci (pre-lock in `spinlock.c`, post-lock in `fair.c`) call into both
mechanisms, but use them differently — pre-lock's Hotlock check is an
optional off-by-default selectivity filter; post-lock's is unconditional
and load-bearing.

---

## 1. Hotlock

### Data structure — `kernel/locking/spinlock.c:189-254`

```c
struct ivh_hotlock_entry {
	const void	*lock;       /* direct-mapped key; NULL == empty bucket */
	atomic_t	history;     /* EWMA, IVH_HOTLOCK_SCALE fixed point */
	atomic_t	waiters;     /* live count, NOT reset on collision */
	atomic64_t	last_eval_ns;/* per-lock cooldown for the expensive tail */
	u8		__pad[8];    /* pads 24B -> 32B, 2 entries per cache line */
};

static struct ivh_hotlock_entry ivh_hotlock_table[IVH_HOTLOCK_SIZE] __cacheline_aligned;
```

Constants — `include/linux/bpf_sched.h:132-135`:

```c
#define IVH_HOTLOCK_BITS   12   /* 4096 buckets, ~64KB table */
#define IVH_HOTLOCK_SIZE   (1 << IVH_HOTLOCK_BITS)
#define IVH_HOTLOCK_SCALE  10   /* fixed point: 1<<10 == "1.0" */
#define IVH_HOTLOCK_HALF   (1 << (IVH_HOTLOCK_SCALE - 1))
```

Direct-mapped hash table, deliberately lockless. `history` is a soft
heuristic (decaying "was this lock usually contended" estimate) — a lost
update or bucket collision degrades precision, not correctness, same
tolerance already accepted for the BPF destination-verdict cache.

### Two independent signals per lock

1. **`history`** (`ivh_hotlock_update()` / `ivh_hotlock_read()`,
   `spinlock.c:263-320`) — a decaying EWMA, sampled once per acquisition:
   ```c
   new = old + ((sample - old) >> ivh_hotlock_ewma_k);
   ```
   `sample` is `1<<IVH_HOTLOCK_SCALE` if contended, `0` if not.
   `ivh_hotlock_ewma_k` (sysctl, default `3`) controls decay rate — smaller
   k reacts faster, larger k smooths more.

   Two optimizations worth knowing about when reading traces:
   - The `!contended` path is sampled only 1-in-8
     (`ivh_hotlock_sample_counter`, `spinlock.c:281-282`) — real contention
     is rare (~0.8% of acquisitions, measured earlier in the project) and
     is the signal Hotlock needs fast, so only the common "still cold" path
     is throttled.
   - The CAS is skipped entirely when `new == old` (`spinlock.c:307-312`)
     — confirmed via audit to be the dominant cost: ~88% of calls are cold
     locks that decay to and stay at exactly 0.

2. **`waiters`** (`ivh_hotlock_note_waiter_enter/exit()`,
   `spinlock.c:396-417`) — a *live* count, incremented/decremented from
   `kernel/locking/qspinlock.c` at the real contention entry/exit points
   (pending-bit spin, full MCS queueing). Not an average — this is "how
   many other threads are queued on this exact lock right now." By the
   time a new holder's own acquisition completes, its own enter/exit pair
   has already balanced out, so whatever remains is genuinely other
   waiters.

   `contended = waiters > 0` is the immediate/live signal;
   `history > IVH_HOTLOCK_HALF` is the smoothed/historical one. Post-lock
   uses `is_hot = contended || history > IVH_HOTLOCK_HALF` — i.e. either
   signal alone can mark a lock hot.

### Where each locus uses it

**Post-lock (`ivh_post_lock()`, `spinlock.c:434-541`)** — unconditional,
load-bearing. Every eligible acquisition updates `history` and reads
`waiters`, gated only by `ivh_post_lock_enabled` (checked first,
`spinlock.c:460-461`) and the `lock_depth == 1` recursion guard
(`spinlock.c:483-484`). This is also where the **`ivh_hotlock:`**
trace_printk lives (`spinlock.c:529-534`) — see §3 for why this line is
dangerous to leave on.

**Pre-lock (`ivh_pre_lock()`, `spinlock.c:58-127`)** — optional, off by
default (`ivh_pre_lock_hotlock_enabled`, default `0`). When on
(`spinlock.c:105-116`):
```c
if (ivh_pre_lock_hotlock_enabled) {
	int waiters = ivh_hotlock_waiters(lock);
	bool contended = waiters > 0;
	if (!ivh_post_lock_enabled)
		ivh_hotlock_update(lock, contended);   /* only update if nothing else is */
	if (!contended && ivh_hotlock_read(lock) <= IVH_HOTLOCK_HALF) {
		this_cpu_inc(ivh_prelock_hotlock_cold_skipped);
		return;
	}
	this_cpu_inc(ivh_prelock_hotlock_hot_passed);
}
```
Rationale (full comment at `spinlock.c:92-104`): pre-lock never had an
economic reason for a duration-based floor (unlike post-lock, which
migrates *while holding* the lock, so waiters pay for it) — this asks "is
this lock worth caring about" instead of "is this CS long enough,"
reusing post-lock's existing table rather than building a second one.
Only updates `history` itself when post-lock is disabled, so the two loci
don't double-sample the same signal.

### Tier check (post-lock only) — `fair.c:13591-13603`

```c
if (!contended) {
	u16 tier1_bar = ((1 << IVH_HOTLOCK_SCALE) * ivh_hotlock_tier1_pct) / 100;
	if (history <= tier1_bar) {
		this_cpu_inc(ivh_post_lock_tier_skipped);
		return;
	}
}
```
A live waiter (`contended == true`) is trusted at the normal steal-imminence
bar. No live waiter but hot history requires a *stricter* bar
(`ivh_hotlock_tier1_pct`, sysctl, default `75` → bar = 75% of full scale)
since nothing confirms urgency right now, only history.

### Sysctls (current defaults, all live at `kernel/sched/bpf_sched.c`)

| sysctl | default | line |
|---|---|---|
| `ivh_pre_lock_hotlock_enabled` | `0` | bpf_sched.c:87 |
| `ivh_hotlock_ewma_k` | `3` | bpf_sched.c:107 |
| `ivh_hotlock_tier1_pct` | `75` | bpf_sched.c:115 |
| `ivh_hotlock_eval_cooldown_ns` | `50000` | bpf_sched.c:132 |

---

## 2. time_left / `ivh_steal_imminent()`

`fair.c:13227-13264`, shared by both loci. **Answers a different question
than Hotlock**: not "is this lock worth caring about" but "is the vCPU I'm
*currently* running on (`rq = this_rq()`, captured while preemption is
still disabled — see the correctness note at `fair.c:13574-13580`) in
enough danger of an imminent host steal that migrating away is worth it
right now."

```c
static __always_inline bool ivh_steal_imminent(struct rq *rq)
{
	u64 now, elapsed_since_active;
	s64 time_left;

	if (rq->cpu_capacity > ivh_capacity_threshold) {      /* Gate 1 */
		this_cpu_inc(ivh_steal_imminent_capacity_reject);
		return false;
	}

	now = sched_clock();
	elapsed_since_active = now - max(rq->last_preemption, (u64)rq->last_idle_tp);
	time_left = (s64)rq->last_active_time - (s64)elapsed_since_active
		    - (s64)current->last_cs_ns;

	if (unlikely(ivh_trace_enabled))
		trace_printk("ivh_time_left: cpu=%d ewma_act_ns=%llu last_active_time=%llu "
			     "elapsed_since_active=%llu last_cs_ns=%llu time_left=%lld "
			     "threshold=%lu\n",
			     smp_processor_id(), rq->ewma_act_ns, rq->last_active_time,
			     elapsed_since_active, current->last_cs_ns, time_left,
			     ivh_time_left_threshold_ns);

	if (rq->last_active_time != 0 && time_left > (s64)ivh_time_left_threshold_ns) {  /* Gate 2 */
		this_cpu_inc(ivh_steal_imminent_time_left_reject);
		return false;
	}

	return true;
}
```

**Reading the sign correctly** (this is the part worth being precise
about): `time_left` is *budget remaining in the current active burst*, not
a danger score. `return true` means "danger is real, proceed with
migration"; `return false` means "still fine, skip." So:

- Gate 1: `cpu_capacity > threshold` (default `512`, healthy CPUs read
  ~1022 in `/proc/vcap_info`) → **capacity is high, vCPU looks healthy →
  reject** (nothing to fix).
- Gate 2: `time_left > threshold` (default `500000` ns) → **plenty of
  runway left before the typical active-burst would end → reject** (no
  urgency yet).

Only proceeds when `time_left` is *small or negative* — i.e. we're already
deep into (or past) the vCPU's typical active-burst length, plus our own
CS would push further past it, meaning a steal looks imminent by this
model.

If `last_active_time == 0` (no measurement yet), Gate 2 never vetoes —
same fallback the old `ewma_act_ns` check had for `ewma == 0`.

### Where `last_active_time` actually comes from — `kernel/sched/cputime.c:256-286`

This is the part that isn't in `fair.c` at all, easy to miss:

```c
static __always_inline u64 steal_account_process_time(u64 maxtime)
{
#ifdef CONFIG_PARAVIRT
	if (static_key_false(&paravirt_steal_enabled)) {
		u64 steal;
		struct rq *rq = this_rq();

		steal = paravirt_steal_clock(smp_processor_id());
		steal -= rq->prev_steal_time;
		steal = min(steal, maxtime);
		account_steal_time(steal);
		rq->prev_steal_time += steal;
		if (steal > 0) {
			u64 now = sched_clock();
			if (steal > 1000000) {   /* >1ms of newly-detected steal */
				if (rq->last_preemption > rq->last_idle_tp)
					rq->last_active_time = now - rq->last_preemption - steal;
				else
					rq->last_active_time = now - rq->last_idle_tp - steal;
				rq->last_preemption = now;
			}
			rq->preemptions += 1;
			if (rq->max_latency < steal)
				rq->max_latency = steal;
		}
		return steal;
	}
#endif
	return 0;
}
```

So `last_active_time` is **a single most-recent historical sample**, not a
smoothed average — refreshed only when a new steal event exceeding 1ms is
actually detected via `paravirt_steal_clock()`. It records "how long was
my previous active stretch, right before that steal happened." `rq->preemptions`
and `rq->max_latency` are updated in the same block — useful as ground truth
when comparing predictors (see §4).

### `ewma_act_ns` — the retired signal, still logged

`kernel/sched/sched.h:1363`, written externally via
`set_ewma_act_ns()` (`kernel/sched/core.c:237-244`) by `vsched_module`
(userspace `vcap`, not this kernel tree). This **used to be** Gate 2's
signal until a 2026-07-03 correction (see the full rationale comment at
`fair.c:13202-13225`) found it "stale/unreliable," and replaced it with
the `last_active_time` formula above, built entirely from fields the
kernel already computes natively (no external module dependency). It is
still computed by `vcap` and still logged in every `ivh_time_left:` trace
line for side-by-side comparison — it was never removed from the trace,
only from the gating decision — which is exactly the hook for the
comparison you want to run (§4).

### Sysctls

| sysctl | default | line |
|---|---|---|
| `ivh_capacity_threshold` | `512` | bpf_sched.c:20 |
| `ivh_time_left_threshold_ns` | `500000` | bpf_sched.c:29 |

---

## 3. Traces — and the ring-buffer trap

Two `trace_printk` sites relevant here, both gated behind the single
sysctl `ivh_trace_enabled` (default `0`):

- `"ivh_hotlock: lock=%px waiters=%d history=%d path=%s decision=%s\n"`
  — `spinlock.c:530-534`. Fires on **every eligible lock acquisition**,
  unconditionally, whenever post-lock is enabled. This is the expensive
  one.
- `"ivh_time_left: cpu=%d ewma_act_ns=%llu last_active_time=%llu ...\n"`
  — `fair.c:13251-13256`. Fires on every call that clears Gate 1
  (capacity) — still a large fraction of all Hotlock-hot classifications
  under real contention (millions/run).

**Both share the same on/off switch.** There is no way to enable
`ivh_time_left:` without also enabling `ivh_hotlock:`, short of a rebuild
that splits the gate. This matters in practice: during today's freeze
repro under real contention, `ivh_hotlock:` produced ~6.8M lines while
ftrace itself reported **~210M dropped events** (`grep -oE "LOST [0-9]+
EVENTS"` on the drained `trace_pipe` output) — i.e. ~97% of everything
written to the ring buffer, including the rarer `ivh_time_left:` and
`ivh_selected:`/`ivh_post_lock_migrate:` lines, was overwritten before a
single-reader `cat trace_pipe` drain could get to it. Out of 145 real
migrations in that run, only 2 `ivh_post_lock_migrate:` lines survived.

**Practical consequence for debugging ewma vs last_active_time:** don't
try to capture `ivh_time_left:` under full hackbench-scale contention with
`ivh_hotlock:` also live — you'll lose most of your samples to overwrite.
Options, cheapest first:
1. Filter at the source with `ftrace`'s function filter if you only care
   about the `ivh_steal_imminent` call site (won't help — both
   trace_printks are inside functions called from the same hot path, and
   `trace_printk` isn't a separate ftrace event you can enable/disable
   independently).
2. Use a **higher-bandwidth per-CPU ring buffer**
   (`/sys/kernel/debug/tracing/buffer_size_kb`, per-CPU, default is
   usually small) before starting the drain — this raises the volume
   ftrace can hold before overwriting, buying more headroom before loss
   starts.
3. Read from `/sys/kernel/debug/tracing/per_cpu/cpuN/trace_pipe_raw` with
   one drain process per CPU instead of the single global `trace_pipe` —
   the global one serializes all 16 CPUs through one reader, which is
   itself a bottleneck independent of ring-buffer size.
4. The clean fix is a rebuild: give `ivh_time_left:` its own sysctl
   (e.g. `ivh_time_left_trace_enabled`) so it can run without
   `ivh_hotlock:` alongside it. Not done yet — flagged here since you
   said no rebuilds this session.

---

## 4. How to actually compare `ewma_act_ns` vs `last_active_time`

Both values are in every `ivh_time_left:` line already — you don't need
new instrumentation, only a capture strategy that survives the volume
problem above (§3) and a way to score "which one predicted better."

1. Pick one of the mitigations in §3 (buffer_size_kb bump is the
   cheapest, zero code change).
2. Run a workload with real contention (`sysbench` on the neighbor +
   hackbench or NHextend here) with `ivh_trace_enabled=1`, capture
   `ivh_time_left:` lines to a file on persistent disk (not `/tmp` — see
   the freeze-repro lesson from earlier today: `/tmp` doesn't survive a
   hard VM reset, `/home/nick/...` does).
3. Ground truth for "was a steal actually imminent" is `rq->preemptions`
   incrementing and `rq->max_latency` in `cputime.c` — those aren't
   currently exposed via `/proc/ivh_debug` or trace, so for now the
   closest proxy is: did a real migration follow
   (`ivh_post_lock_migrate:`/`ivh_selected:`), and separately, did
   `is_cpu_preempted()` (the layer-1 destination-health check,
   `fair.c`, referenced at `spinlock.c:161`) fire shortly after. If you
   want a clean ground-truth signal instead of a proxy, that's a small,
   real addition: export `rq->preemptions`/`rq->max_latency` deltas
   alongside the `ivh_time_left:` line (or via `/proc/ivh_debug`) so each
   sample can be labeled "steal happened within Nms" after the fact.
4. For each sample, compute what each predictor *would have* rejected:
   - `last_active_time`-based `time_left` (already computed, already in
     the line).
   - Same formula substituting `ewma_act_ns` for `last_active_time` — do
     this in post-processing (Python), not in the kernel; the trace line
     already has both fields, so it's a matter of recomputing on read.
5. Compare each predictor's reject/proceed decision against the ground
   truth label from step 3. The `post_acquisition_reactive_migration_2026-07-02.md`
   doc in this same directory has the earlier framing for how conditional
   precision was scored — worth reading before rederiving the metric from
   scratch.

---

## 5. "Fix everything back up" — restoration checklist

Nothing in the source tree needs restoring — it's all still there. What's
*not* currently true, because every reboot resets it, is: the module isn't
loaded, `vcap`/`MY_ivh_atc` aren't running, and every `ivh_*` sysctl is
back at its compiled-in default. In order:

1. **If you've edited any `.c`/`.h` file since the currently-booted
   kernel was built**, you need a kernel rebuild + reboot into it before
   any of the code above takes effect at all. (Not doing this now per
   your instruction — this is the one step that's actually on you to run
   when ready.) Otherwise skip to 2.
2. Load the module for whatever kernel you're currently booted into:
   ```
   sudo insmod /lib/modules/$(uname -r)/extra/vsched_module.ko
   ```
   (`install_module.sh` in the tree root rebuilds+caches this per kernel
   version if it's missing — pass the exact `uname -r` string explicitly,
   its no-arg default reads `include/generated/utsrelease.h`, which can
   silently point at a different kernel version than the one actually
   booted.)
3. Start the daemons (order doesn't matter, but both need the module
   loaded first):
   ```
   cd /home/nick/vsched_main/vcapacity && sudo ./vcap &
   sudo /home/nick/kernels/linux-6.17-rseqport/tools/bpf/MY_ivh_atc &
   ```
   Check `pgrep -fa "vcap|MY_ivh_atc"` for exactly one of each — a
   duplicate `vcap` causes D-state hangs and double BPF hook firing.
4. Confirm `/proc/vcap_info` and `/proc/ivh_debug` are both readable and
   updating.
5. Set sysctls for whichever locus you're testing. For Hotlock-gated
   pre-lock + last_active_time-based post-lock (today's intended
   configuration):
   ```
   sudo sysctl -w kernel.ivh_pre_lock_hotlock_enabled=1
   sudo sysctl -w kernel.ivh_post_lock_enabled=1
   sudo sysctl -w kernel.ivh_post_lock_dispatch=1
   sudo sysctl -w kernel.ivh_spin_yield_enabled=1
   ```
   Leave `ivh_capacity_threshold` (512) and `ivh_time_left_threshold_ns`
   (500000) at default unless you're specifically re-running the
   comparison in §4. Only set `ivh_trace_enabled=1` when you actually want
   traces — it's not free (§3).

---

## If the code really is gone

It isn't, as of this writing — but if you land here after a real
`git reset --hard` / `git checkout <old-commit>` that discarded working-tree
changes: check `git reflog` first (`git log --all --source` if the commit
was ever committed anywhere, even on a branch you've since deleted). The
reflog on this repo currently goes back to `84f1e5fcc` (2026-07-02,
"6.17.0-rseqport34+") as the most recent commit, with everything described
in this doc present only as *uncommitted* working-tree changes on top of
that — meaning a hard reset to any earlier commit **would** actually
discard it, since none of this was ever committed. If that's what
happened, the reflog is the only path back (assuming a recent-enough
editor swap file / IDE local history isn't a faster option), and this doc
is what you'd be reconstructing from.
