// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
// NOTE: set this suit your system
#include "vmlinux.h"
#include "bpf_helpers.h"
unsigned long tgidpid = 0;
unsigned long cgid = 0;
unsigned long allret = 0;
unsigned long max_exec_slice = 0;

/*
 * Round-robin starting point for test3()'s destination search, 2026-07-10.
 * process_cpu() used to always scan starting at (source_cpu + 1), so two
 * lockholders on the same source vCPU always probed destinations in the
 * same order and converged on the same low-numbered "first free" CPU.
 * Bumped once per test3() call and shared across all CPUs/threads; a plain
 * non-atomic increment is fine here since this is just a load-spreading
 * hint, not a correctness-critical value (same reasoning as the benign
 * last-write-wins race documented on dest_verdict_cache below).
 */
unsigned int ivh_rr_start = 0;
#define fits_capacity(cap, max)   ((cap) * 1280 < (max) * 1024)
#define SCHED_FIXEDPOINT_SHIFT    10
#define SCHED_FIXEDPOINT_SCALE    (1L << SCHED_FIXEDPOINT_SHIFT)

/* Increase resolution of cpu_capacity calculations */
#define SCHED_CAPACITY_SHIFT      SCHED_FIXEDPOINT_SHIFT
#define SCHED_CAPACITY_SCALE      (1L << SCHED_CAPACITY_SHIFT)

#define debug(args...)

extern const struct rq runqueues __ksym; /* struct type global var. */
char LICENSE[] SEC("license") = "Dual BSD/GPL";

/* Shadow-mode candidate counters — PERCPU_ARRAY, each CPU accumulates
 * independently.  Sum per-CPU values in userspace to get totals.
 *
 * Index layout:
 *   0  USER_MOVABLE ticks (lockholder seen at tick, user CS, movable)
 *   1  USER_NONMOVABLE ticks
 *   2  KERNEL_MOVABLE ticks
 *   3  KERNEL_NONMOVABLE ticks
 *   4  CANDIDATE_TOTAL  (one-shot per task per CPU)
 *   5  CANDIDATE_USER_MOVABLE
 *   6  CANDIDATE_USER_NONMOVABLE
 *   7  CANDIDATE_KERNEL_MOVABLE
 *   8  CANDIDATE_KERNEL_NONMOVABLE
 */
#define CTR_USER_MOVABLE        0
#define CTR_USER_NONMOVABLE     1
#define CTR_KERNEL_MOVABLE      2
#define CTR_KERNEL_NONMOVABLE   3
#define CTR_CANDIDATE_TOTAL     4
#define CTR_CAND_USER_MOV       5
#define CTR_CAND_USER_NMON      6
#define CTR_CAND_KERN_MOV       7
#define CTR_CAND_KERN_NMON      8
#define CTR_MAX                 9


struct sched_in_entry {
    u32 pid;
    u64 stamp;
};

/* Per-CPU: tracks which task is current and when it was first seen. */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, struct sched_in_entry);
} sched_in_map SEC(".maps");

/* Per-CPU: per-class tick counts and one-shot candidate counts. */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, CTR_MAX);
    __type(key, u32);
    __type(value, u64);
} lhp_counters SEC(".maps");

/* Per-CPU: pid of the last task recorded as a new candidate.
 * Used for one-shot deduplication: only count a task as a candidate
 * once per scheduling epoch on this CPU. */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, u32);
} last_candidate_pid SEC(".maps");

/*
 * Dynamic JIT-process banlist, populated by detect_jit kprobe and by
 * the comm banlist in test().  Key: TGID (process group ID).
 *
 * Keying by TGID instead of TID is critical: JIT runtimes have many
 * threads sharing the same mm (e.g. Bun has HeapHelper, JITWorker, AND
 * a main thread named "claude" + an "HTTP Client" thread).  Migrating
 * ANY thread in the process spreads mm->cpu_bitmap to new CPUs — so we
 * must block ALL threads in a JIT process, not just the named JIT ones.
 */
#define PROT_EXEC  0x4
#define JIT_MAP_MAX 4096
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, JIT_MAP_MAX);
    __type(key, u32);   /* TGID */
    __type(value, u8);
} jit_tgids SEC(".maps");

/*
 * Per-CPU record of the last accepted migration.  Userspace polls this
 * every few seconds and writes to /var/log/ivh_migrations.log with fsync.
 * Using a map instead of bpf_trace_printk avoids routing output through
 * trace_pipe — a reader process for trace_pipe would itself become an IVH
 * migration candidate on throttled CPUs, creating a feedback loop that
 * floods the spinlock path and triggers TLB IPI soft lockups.
 */
struct ivh_migration_event {
    char  comm[16];
    int   src_cpu;
    int   dst_cpu;
    unsigned long mm_bits;
    u64   timestamp;
    u64   count;       /* total migrations accepted on this CPU */
};

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, u32);
    __type(value, struct ivh_migration_event);
} last_migration SEC(".maps");


//Function used to determine if CPU is idle
//If the only processes running on a CPU are sched-IDLE, core is considered idle
static int idle_cpu(struct rq *rq)
{
    if (rq->nr_running && rq->curr->policy == 5)
        return 1;
    if (rq->curr != rq->idle)
        return 0;
    if (rq->nr_running)
        return 0;
    if (rq->ttwu_pending)
        return 0;
    return 1;
}

//Function to get how long the current task has been running without interruption
//Interruptions include going idle, preemption, etc
static u64 get_task_runtime(u64 now_time, struct rq *rq)
{
	u64 ref;

if (rq->last_idle_tp > rq->last_preemption)
    ref = rq->last_idle_tp;
else
    ref = rq->last_preemption;

if (ref > now_time)
    return 0;

return now_time - ref;
}

//Function to check if a cpu is preempted
//Returns 1 if the vCPU heartbeat is stale >1.5ms (hypervisor is stealing it),
//0 if the heartbeat is fresh (CPU is running normally).
//Matches the threshold used by the kernel's is_cpu_preempted() in cputime.c.
//
// 2026-07-02: clock_preempt is only refreshed on an active scheduler tick
// (account_process_tick, kernel-side). A tickless-idle CPU (NO_HZ_IDLE, the
// default) stops refreshing it and looks "stolen" once idle exceeds 1.5ms —
// confirmed this session: 270/270 commit-time vetoes hit targets with
// healthy capacity. last_idle_tp is written on idle transitions
// (account_idle_time, tick-independent) and is fresh for a just-idled CPU,
// so take the more recent of the two as the true liveness timestamp. Same
// idiom already used in the GATE_BURST_BUDGET block below.
static int is_cpu_preempted(struct rq *rq, u64 now_time)
{
    u64 last_seen = rq->clock_preempt > rq->last_idle_tp
                     ? rq->clock_preempt : rq->last_idle_tp;

    if (last_seen > now_time)
        return 0;

    return now_time - last_seen > 1500000ULL;
}

/*
 * Per-CPU counters: why did process_cpu() reject a candidate, or how did it
 * accept one?  EXPERIMENT: hackbench gate-rejection profiling, 2026-07-01.
 * Read after a run with: bpftool map dump name reject_reasons
 */
#define REJ_CPUMASK       0  /* not in task's own cpus_ptr */
#define REJ_CLAIMED       1  /* already claimed by another IVH thread this tick */
#define REJ_LOCKHOLDER    2  /* target's curr is inside a critical section */
#define REJ_SPINNER       3  /* target's curr is already an MCS waiter */
#define REJ_CAPACITY_LOW  4  /* fails EDWARDS-style capacity gate */
#define REJ_NOT_BETTER    5  /* target capacity <= source capacity */
#define REJ_PREEMPTED     6  /* target heartbeat stale (hypervisor stole it) */
#define REJ_BURST_ORDER   7  /* target's active burst started earlier than ours */
#define REJ_BURST_BUDGET  8  /* target has used up its typical active window */
#define ACC_TIER1_IDLE    9  /* accepted: nr_running == 0, genuinely empty runqueue */
#define ACC_TIER2_NR1     10 /* accepted (fallback record): nr_running == 1 */
#define REJ_NR_RUNNING    11 /* target runqueue depth >= 2 — would queue behind existing work */
#define REJ_MAX           12

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, REJ_MAX);
    __type(key, u32);
    __type(value, u64);
} reject_reasons SEC(".maps");

static __always_inline void bump_reason(u32 reason)
{
    u64 *val = bpf_map_lookup_elem(&reject_reasons, &reason);
    if (val)
        (*val)++;
}

/*
 * Destination-verdict cache, 2026-07-02.  process_cpu() is called at very
 * high frequency (millions/sec under load) and re-reads/re-derives the same
 * per-CPU facts on every call, even when two calls from different source
 * vCPUs scan the same candidate moments apart.  cpu_capacity is written by
 * userspace (vcap) on its own poll cadence -- much slower than the
 * process_cpu() call rate -- so caching it for a short TTL costs
 * negligible extra staleness.
 *
 * Deliberately NOT cached: GATE_LOCKHOLDER (lock_depth flickers at ~200ns
 * per this session's measured last_cs_ns histogram -- caching it risks a
 * stale "not a lock holder" verdict feeding a real migration decision,
 * exactly the LHP scenario IVH exists to prevent) and GATE_PREEMPTED
 * (heartbeat staleness -- treated as fast-changing until proven otherwise).
 *
 * GATE_BURST_ORDER's *raw* last_preemption value is cached (a per-CPU,
 * caller-independent fact), but its pass/fail verdict is NOT -- that
 * comparison is against the calling source rq's own last_preemption
 * (ctx->rq_last_preempt), which differs per caller, so the boolean result
 * cannot be shared across source vCPUs the way GATE_CAPACITY_LOW's can.
 *
 * Plain (non-percpu) array: the whole point is sharing one verdict across
 * different source vCPUs' concurrent scans, which a percpu map would defeat.
 * Concurrent writers racing to update the same target's slot is a benign
 * last-write-wins race on soft state, not a correctness issue.
 */
#define DEST_CACHE_TTL_NS 10000ULL

struct dest_cache_entry {
    u8  cap_healthy;      /* cached GATE_CAPACITY_LOW verdict */
    u64 last_preemption;  /* cached raw value, for GATE_BURST_ORDER */
    u64 cached_at_ns;
};

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 256);
    __type(key, u32);
    __type(value, struct dest_cache_entry);
} dest_verdict_cache SEC(".maps");

/*
 * Compile-time gate toggles for process_cpu(), EXPERIMENT: hackbench
 * leave-one-out gate sweep, 2026-07-01.  Set to 0 to disable a gate.
 * (CPUMASK and CLAIMED are not toggleable: CPUMASK is a hard correctness
 * requirement, CLAIMED prevents two IVH threads racing onto the same
 * target — neither is a tunable policy decision.)
 */
#define GATE_LOCKHOLDER    1
#define GATE_SPINNER    0
#define GATE_CAPACITY_LOW    1
#define GATE_NOT_BETTER    1
#define GATE_PREEMPTED    1
#define GATE_BURST_ORDER    0
#define GATE_BURST_BUDGET    0

/*
 * Stage A5 (ivh_build_and_evaluation_plan_2026-07-11.md): runtime toggle for
 * the Tier-2 (nr_running==1) reluctant-fallback destination below. Default 1
 * preserves exactly today's behavior (record the lateral candidate, keep
 * scanning for Tier-1). Set to 0 at runtime (bpftool map update on the .bss
 * map, or a loader flag once one exists) to make process_cpu() abstain
 * (leave target_cpu at -1) instead of ever accepting a Tier-2 landing --
 * lets "IVH found nothing good" be observed/tested cleanly, and composes
 * with a future adaptive-spinning fallback (the thread just spins normally
 * instead of taking a lateral, not-actually-better migration).
 */
int ivh_tier2_fallback_enabled = 1;

/*
 * Destination capacity floor, 2026-07-02 gate-combo experiment: require
 * a migration target to be well above ivh_capacity_threshold (currently
 * tuned to 512 as the *source* trigger ceiling), not just "average or
 * >500". With a strict floor here, dest > 850 > source (<=512) always
 * holds, so GATE_NOT_BETTER is provably redundant and left off.
 */
#define IVH_CAP_FLOOR    400

struct task_ctx {
    struct task_struct *curr;          /* task that is to be moved */
    int *target_cpu_ptr;               /* result - where should the task be moved? */
    u64 now;
    int start;                         /* CPU the search scan starts from (round-robin, not necessarily source_cpu) */
    int source_cpu;                    /* actual source CPU — excluded from candidacy */
    u64 rq_last_preempt;               /* last_preemption of source rq (age reference) */
    int source_capacity;               /* cpu_capacity of the source vCPU */
    int total_cpus;                    /* total number of CPUs in system */
    int average_capacity;              /* system average capacity, for EDWARDS-style gate */
    int *found_active_worker_ptr;      /* 1 once a Tier 1 (active worker) target is found */
};

static int process_cpu(u32 iter, void *data)
{
    struct task_ctx *ctx = data;

    struct task_struct *curr = ctx->curr;

    //Current CPU
    int cpu = (iter + ctx->start) % ctx->total_cpus;

    /* Never migrate a task to the CPU it's already on. */
    if (cpu == ctx->source_cpu)
        return 0;

    //cpumask of curr cpu
    const cpumask_t *cpumask = curr->cpus_ptr;
    unsigned long cpumask_bits = *(cpumask->bits);

    //is valid cpu for task?
    if (!(cpumask_bits & (1UL << cpu))) {
        bump_reason(REJ_CPUMASK);
        return 0;
    }

    int *target_cpu_ptr = ctx->target_cpu_ptr;

    //RQ for current cpu
    struct rq *select_rq = bpf_per_cpu_ptr(&runqueues, cpu);

    //if RQ invalid, return
    if (!select_rq)
        return 0;

    //has this cpu been selected by other IVH threads?
    if (select_rq->prmpt_flags.counter & (1 << 2)) {
        bump_reason(REJ_CLAIMED);
        return 0;
    }

#if GATE_LOCKHOLDER
    /* Skip lockholders: vCPU is inside a CS; adding more work hurts. */
    if (select_rq->curr->lock_depth > 0) {
        bump_reason(REJ_LOCKHOLDER);
        return 0;
    }
#endif

#if GATE_SPINNER
    /*
     * Skip spinners: already queued in an MCS waiter list.
     * - lock free:  spinner already judged this vCPU safe for itself —
     *               migrating here risks thrashing.
     * - lock held:  migrated task joins queue behind existing waiter,
     *               the waiter preemption problem.
     */
    if (select_rq->curr->wait_depth > 0) {
        bump_reason(REJ_SPINNER);
        return 0;
    }
#endif

#if GATE_CAPACITY_LOW || GATE_BURST_ORDER
    /*
     * Shared cache lookup for both gates below — see dest_verdict_cache
     * comment above for why cap_healthy is a cacheable final verdict but
     * last_preemption is cached as a raw value, not a verdict.
     */
    u8  cap_healthy;
    u64 dest_last_preemption;
    {
        u32 cache_key = (u32)cpu;
        struct dest_cache_entry *ce = bpf_map_lookup_elem(&dest_verdict_cache, &cache_key);

        if (ce && ctx->now >= ce->cached_at_ns &&
            ctx->now - ce->cached_at_ns < DEST_CACHE_TTL_NS) {
            cap_healthy = ce->cap_healthy;
            dest_last_preemption = ce->last_preemption;
        } else {
            struct dest_cache_entry fresh;

            cap_healthy = select_rq->cpu_capacity > IVH_CAP_FLOOR;
            dest_last_preemption = select_rq->last_preemption;
            fresh.cap_healthy = cap_healthy;
            fresh.last_preemption = dest_last_preemption;
            fresh.cached_at_ns = ctx->now;
            bpf_map_update_elem(&dest_verdict_cache, &cache_key, &fresh, BPF_ANY);
        }
    }
#endif

#if GATE_CAPACITY_LOW
    /* Strict floor: only migrate to a vCPU well above the source trigger
     * ceiling — see IVH_CAP_FLOOR comment above. */
    if (!cap_healthy) {
        bump_reason(REJ_CAPACITY_LOW);
        return 0;
    }
#endif

#if GATE_NOT_BETTER
    /* Target must be strictly better than the source — lateral migrations
     * to equally-throttled vCPUs waste a schedule() call and stall under
     * uniform host contention (e.g. all CPUs stolen). */
    if (select_rq->cpu_capacity <= ctx->source_capacity) {
        bump_reason(REJ_NOT_BETTER);
        return 0;
    }
#endif

#if GATE_PREEMPTED
    /* Skip if the vCPU heartbeat is stale — hypervisor has already preempted it. */
    if (is_cpu_preempted(select_rq, ctx->now)) {
        bump_reason(REJ_PREEMPTED);
        return 0;
    }
#endif

#if GATE_BURST_ORDER
    /* Skip if target vCPU started its active burst earlier than us — it has
     * less remaining burst time and is closer to being preempted.
     * dest_last_preemption is the cached raw value (see cache block above);
     * the comparison itself is still done fresh per-caller since it depends
     * on ctx->rq_last_preempt (the calling source rq's own value). */
    if (dest_last_preemption <= ctx->rq_last_preempt) {
        bump_reason(REJ_BURST_ORDER);
        return 0;
    }
#endif

#if GATE_BURST_BUDGET
    /*
     * Skip if this vCPU has already used up its typical active window.
     *
     * ref = the later of last_preemption and last_idle_tp, whichever
     * happened more recently marks the true start of this vCPU's current
     * active run.  target_active = now - ref = how long the vCPU has been
     * continuously active since then (the "? :" is: if now > ref, subtract;
     * otherwise clamp to 0 to avoid wrapping on an uninitialized ref).
     *
     * ewma_act_ns is the exponentially weighted average of how long this
     * vCPU runs before the hypervisor preempts it.  If target_active has
     * already reached that budget, preemption is overdue — migrating there
     * would put the task on a vCPU that is about to be yanked away.
     */
    {
        u64 ref = select_rq->last_idle_tp > select_rq->last_preemption
                  ? select_rq->last_idle_tp : select_rq->last_preemption;
        u64 target_active = ctx->now > ref ? ctx->now - ref : 0;
        if (select_rq->ewma_act_ns > 0 && target_active >= select_rq->ewma_act_ns) {
            bump_reason(REJ_BURST_BUDGET);
            return 0;
        }
    }
#endif

    /*
     * 2026-07-02: runqueue-depth gate + inverted tier preference. The old
     * logic preferred a busy ("active worker") destination to avoid a
     * hypervisor vCPU wake-up cost. On a saturated guest that means the
     * migrated thread queues behind dest->curr — pure added latency for a
     * thread that hasn't even acquired its target lock yet. Prefer a
     * genuinely empty runqueue instead.
     *
     * nr_running is read live, never cached in dest_verdict_cache: it
     * changes faster than cpu_capacity and on the same order as
     * lock_depth — the same reasoning that keeps GATE_LOCKHOLDER and
     * GATE_PREEMPTED out of the cache applies here.
     */
    {
        u32 dest_nr = select_rq->nr_running;

        if (dest_nr >= 2) {
            /* Deep queue: reject outright. */
            bump_reason(REJ_NR_RUNNING);
            return 0;
        }

        if (dest_nr == 0) {
            /* Tier 1 (inverted): genuinely empty runqueue — nothing to
             * queue behind. Best possible landing, stop search immediately. */
            bump_reason(ACC_TIER1_IDLE);
            *target_cpu_ptr = (int)(cpu);
            *(ctx->found_active_worker_ptr) = 1;
            return 1;
        }

        /* dest_nr == 1: reluctant fallback. Remember it but keep scanning
         * for a truly-empty runqueue -- unless Stage A5's toggle says to
         * abstain instead of ever accepting a lateral Tier-2 landing. */
        if (!ivh_tier2_fallback_enabled) {
            bump_reason(REJ_NOT_BETTER); /* reuse: "didn't accept this candidate" */
            return 0;
        }
        if (!*(ctx->found_active_worker_ptr)) {
            bump_reason(ACC_TIER2_NR1);
            *target_cpu_ptr = (int)(cpu);
        }
    }

    return 0;
}

/*
 * JIT-worker comm banlist — do not migrate these thread classes.
 *
 * These runtimes interleave spinlock-protected kernel work with heavy
 * mmap/mprotect/madvise calls (JIT code emission, GC, shader compilation).
 * IVH migration spreads their mm->cpu_bitmap; future TLB shootdowns then
 * target CPUs that may be in IRQ-disabled spinlock slowpaths, producing
 * the TLB-IPI deadlock seen in the 2026-06-23 soft lockup.
 *
 * Checked via task->comm (char[16], always present, no CO-RE needed):
 *   "Bun Pool"   Bun thread pool workers (pool 0–N)
 *   "JITWorker"  JavaScriptCore JIT compiler threads
 *   "HeapHelper" JSC GC / heap helper threads
 *   "JS Helper"  gjs / gnome-shell SpiderMonkey helpers
 *   "gjs"        GNOME JavaScript runtime (SpiderMonkey)
 *   "llvmpipe-"  Mesa llvmpipe shader JIT (LLVM)
 *
 * comm comparisons use the minimum prefix that is unique across all thread
 * names on this system (verified via ps -eLo comm survey 2026-06-23).
 */
static __always_inline int is_jit_worker(struct task_struct *t)
{
    char c0 = t->comm[0];
    char c1 = t->comm[1];
    char c2 = t->comm[2];
    char c3 = t->comm[3];

    /* "Bun " — Bun Pool 0..N */
    if (c0 == 'B' && c1 == 'u' && c2 == 'n' && c3 == ' ')
        return 1;
    /* "JITW" — JITWorker */
    if (c0 == 'J' && c1 == 'I' && c2 == 'T' && c3 == 'W')
        return 1;
    /* "Heap" — HeapHelper */
    if (c0 == 'H' && c1 == 'e' && c2 == 'a' && c3 == 'p')
        return 1;
    /* "JS H" — JS Helper */
    if (c0 == 'J' && c1 == 'S' && c2 == ' ' && c3 == 'H')
        return 1;
    /* "gjs\0" — GNOME JS runtime */
    if (c0 == 'g' && c1 == 'j' && c2 == 's' && c3 == '\0')
        return 1;
    /* "llvm" — llvmpipe shader JIT */
    if (c0 == 'l' && c1 == 'l' && c2 == 'v' && c3 == 'm')
        return 1;
    /* "snap" — snapd / snap-update-ns; heavy mount-namespace operations
     * (clone(CLONE_NEWNS), mount, umount) while being migrated at high
     * rate caused hypervisor hard-resets in the 2026-06-24 crash. */
    if (c0 == 's' && c1 == 'n' && c2 == 'a' && c3 == 'p')
        return 1;

    return 0;
}

/*
 * EXPERIMENT_NO_MOVE: set to 1 to make test3() return -1 (running_migration
 * fires and BPF decides yes, but no task ever actually moves).
 * Experiment A = 1, Experiment B/C = 0.
 *
 * EXPERIMENT_FIXED_CPU: set to 1 to make test3() return a trivial fixed
 * target ((rq->cpu + 1) % total_cpus) instead of running the full CPU
 * search algorithm.  If crashes stop with this set, test3()'s search logic
 * is producing bad targets.  If crashes continue, migrate_task_to_async_fair
 * itself is the problem.
 */
#define EXPERIMENT_NO_MOVE    0
#define EXPERIMENT_FIXED_CPU  0

SEC("sched/cfs_sched_tick_end")
int BPF_PROG(test, struct rq *rq, u64 now_time, unsigned int num_of_idle,
             int curr_lock_depth, int curr_kernel_lockholder,
             int curr_user_lockholder, int curr_lockholder, int curr_waiter)
{
    if (!curr_lockholder)
        return 0;

    struct task_struct *curr_task = rq->curr;
    u32 tgid = curr_task->tgid;

    /*
     * Comm banlist: fast check for known JIT thread names.  On first hit,
     * seed jit_tgids with the process TGID so all sibling threads (main
     * thread, HTTP Client, fs.watch, etc.) sharing the same mm are also
     * blocked.  BPF_NOEXIST avoids redundant writes on every subsequent
     * tick — after the first insertion this becomes a no-op.
     */
    if (is_jit_worker(curr_task)) {
        if (!bpf_map_lookup_elem(&jit_tgids, &tgid)) {
            u8 one = 1;
            bpf_map_update_elem(&jit_tgids, &tgid, &one, BPF_NOEXIST);
        }
        return 0;
    }

    /* Dynamic map: block any sibling thread in a known JIT process. */
    if (bpf_map_lookup_elem(&jit_tgids, &tgid))
        return 0;

    return 1;
}

//Hook to decide on which core to land on
SEC("sched/cfs_select_run_cpu_spin")
int BPF_PROG(test3, struct rq *rq, struct task_struct *curr, u64 now_time, int average_capacity, int total_cpus)
{
#if EXPERIMENT_NO_MOVE
    return -1; /* Exp A: decision made but no task moves */
#endif
#if EXPERIMENT_FIXED_CPU
    /* Exp B2: bypass test3 search logic entirely — migrate to next CPU.
     * If this is stable but real test3 crashes, the bug is in CPU selection.
     * If this also crashes, the bug is in migrate_task_to_async_fair itself. */
    int next = (rq->cpu + 1) % total_cpus;
    return next;
#endif
    /*
     * JIT banlist: block migration of JIT worker threads and all siblings
     * sharing the same mm.  Mirrors the identical check in test() (tick path).
     *
     * Without this, the pre-lock path bypasses the tick-time banlist — the
     * 2026-06-25 overnight crash was caused by HeapHelper being migrated
     * across all 16 CPUs via this path, expanding mm->cpu_bitmap to 0xffff.
     * Subsequent JIT TLB flushes sent IPIs to all 16 CPUs; several were in
     * IRQ-disabled spinlock sections and couldn't respond → soft lockup.
     */
    u32 tgid = curr->tgid;
    if (is_jit_worker(curr)) {
        if (!bpf_map_lookup_elem(&jit_tgids, &tgid)) {
            u8 one = 1;
            bpf_map_update_elem(&jit_tgids, &tgid, &one, BPF_NOEXIST);
        }
        return -1;
    }
    if (bpf_map_lookup_elem(&jit_tgids, &tgid))
        return -1;

    /* Round-robin the scan's starting CPU across calls instead of always
     * pivoting off source_cpu — see ivh_rr_start comment at top of file. */
    int start = ivh_rr_start % total_cpus;
    ivh_rr_start++;

    u32 nr_loops = total_cpus;
    int target_cpu = -1;

    int found_active_worker = 0;

    struct task_ctx task_context = {
        .curr = curr,
        .target_cpu_ptr = &target_cpu,
        .now = now_time,
        .start = start,
        .source_cpu = rq->cpu,
        .rq_last_preempt = rq->last_preemption,
        .source_capacity = rq->cpu_capacity,
        .total_cpus = total_cpus,
        .average_capacity = average_capacity,
        .found_active_worker_ptr = &found_active_worker
    };

    unsigned long mm_bits = curr->mm ? curr->mm->cpu_bitmap[0] : 0UL;

    bpf_loop(nr_loops, &process_cpu, &task_context, 0);

    if (target_cpu >= 0) {
        /* Record to map — userspace polls and fsync's; no trace_pipe. */
        u32 key = 0;
        struct ivh_migration_event *ev =
            bpf_map_lookup_elem(&last_migration, &key);
        if (ev) {
            __builtin_memcpy(ev->comm, curr->comm, 16);
            ev->src_cpu   = rq->cpu;
            ev->dst_cpu   = target_cpu;
            ev->mm_bits   = mm_bits;
            ev->timestamp = now_time;
            ev->count    += 1;
        }
    }

    return target_cpu;
}

SEC("sched/cfs_should_spinlock")
int test4(int test)
{
    return 1;
}

SEC("sched/cfs_should_bias")
int test6(int test)
{
    return 1;
}

struct latency_ctx {
    struct task_struct *curr;          /* task to be placed */
    struct cpumask *idle_cpus;         /* mask of idle CPUs */
    int *target_cpu_ptr;               /* pointer to selected CPU */
    int start;                         /* CPU to start search from */
    u64 *min_latency_ptr;              /* pointer to track minimum latency */
    u64 *preemption_time_ptr;          /* pointer to track preemption time */
    int *found_good_cpu_ptr;           /* pointer to flag for good CPU found */
    u64 *max_bad_latency_ptr;          /* pointer to track max latency for bad CPUs */
    u64 now;                           /* current time */
    u64 *longest_runtime_ptr;          /* pointer to track longest runtime */
    int total_cpus;                    /* total CPUs in system */
    int average_capacity;              /* average CPU capacity in system */
};

static int search_latency(u32 iter, void *data)
{
    struct latency_ctx *ctx = data;
    struct task_struct *curr = ctx->curr;
    int cpu = (iter + ctx->start) % ctx->total_cpus;

    //leave if we've checked each cpu
    if (iter >= ctx->total_cpus) {
        return 1;
    }

    const cpumask_t *cpumask_const = curr->cpus_ptr;
    unsigned long cpumask_bits = *(cpumask_const->bits);

    //check if task is allowed to run on said cpu
    if (!(cpumask_bits & (1UL << cpu))) {
        return 0;
    }

    struct rq *select_rq = bpf_per_cpu_ptr(&runqueues, cpu);
    if (!select_rq) {
        return 0;
    }

    int *target_cpu_ptr = ctx->target_cpu_ptr;
    u64 *preemption_time_ptr = ctx->preemption_time_ptr;

    //if the target is uncontested - no reason to hesitate
    if (select_rq->cpu_capacity > 1000) {
        *target_cpu_ptr = cpu;
        return 1;
    }

    //path if there are only SCHED-IDLE tasks running
    if (select_rq->nr_running > 0) {
        //if it's a better than average core - that's good enough!
        if (select_rq->cpu_capacity > (ctx->average_capacity)) {
            *target_cpu_ptr = cpu;
            return 1;
        }

        //if it has been running recently - good enough
        if (get_task_runtime(ctx->now, select_rq) < 2000000) {
            *target_cpu_ptr = cpu;
            return 1;
        }

        return 0;
    }

    if (idle_cpu(select_rq)) {
        //normal loop, if a cpu is less than the median - ignore it. Otherwise pick lowest latency
        if (select_rq->cpu_capacity > (ctx->average_capacity) &&
            select_rq->avg_latency <= *(ctx->min_latency_ptr)) {

            *(ctx->min_latency_ptr) = select_rq->avg_latency;
            *target_cpu_ptr = cpu;
            return 0;
        }
    }

    return 0;
}

SEC("sched/cfs_latency_select")
int BPF_PROG(test32, int prev, struct task_struct *curr, struct cpumask *idle_cpus,
             int average_capacity, int total_cpus)
{
    int start = 0;
    int nr_loops = total_cpus;
    int target_cpu = -1;
    u64 preemption_time = 0;
    int util_percent = (curr->se.avg.util_avg * 100) / (1L << 10);

    if (util_percent > 10 || curr->policy == 5) {
        return -1;
    }

    u64 min_latency = 0xFFFFFFFFFFFFFFFFULL; // Using max u64 value
    u64 max_bad_latency = 0xFFFFFFFFFFFFFFFFULL; // Using max u64 value
    int found_good_cpu = 0;
    u64 longest_runtime = 0xFFFFFFFFFFFFFFFFULL; // Using max u64 value
    u64 now = 0;

    struct rq *current_rq = bpf_this_cpu_ptr(&runqueues);
    if (current_rq) {
        now = current_rq->clock_preempt;
    }

    struct latency_ctx latency_context = {
        .curr = curr,
        .target_cpu_ptr = &target_cpu,
        .idle_cpus = idle_cpus,
        .start = prev,
        .preemption_time_ptr = &preemption_time,
        .min_latency_ptr = &min_latency,
        .found_good_cpu_ptr = &found_good_cpu,
        .max_bad_latency_ptr = &max_bad_latency,
        .now = now,
        .longest_runtime_ptr = &longest_runtime,
        .total_cpus = total_cpus,
        .average_capacity = average_capacity
    };

    bpf_loop(256, &search_latency, &latency_context, 0);
    return target_cpu;
}

/*
 * Kprobe on do_mprotect_pkey — the internal mprotect implementation.
 * Signature: do_mprotect_pkey(unsigned long start, size_t len,
 *                              unsigned long prot, int pkey)
 * On x86-64: rdi=start, rsi=len, rdx=prot, rcx=pkey.
 *
 * Any thread requesting PROT_EXEC is a JIT compiler.  Record its TGID
 * in jit_tgids so all threads sharing the same mm are blocked from IVH
 * migration.  This catches every runtime without hardcoded thread names.
 */
SEC("kprobe/do_mprotect_pkey")
int detect_jit(struct pt_regs *ctx)
{
    unsigned long prot = ctx->dx; /* rdx = 3rd arg on x86-64 */
    if (!(prot & PROT_EXEC))
        return 0;

    u32 tgid = (u32)(bpf_get_current_pid_tgid() >> 32);
    u8 one = 1;
    bpf_map_update_elem(&jit_tgids, &tgid, &one, BPF_NOEXIST);
    return 0;
}
