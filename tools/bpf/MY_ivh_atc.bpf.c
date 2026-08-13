// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
// NOTE: set this suit your system
#include "vmlinux.h"
#include "bpf_helpers.h"
unsigned long tgidpid = 0;
unsigned long cgid = 0;
unsigned long allret = 0;
unsigned long max_exec_slice = 0;
/*
 * EXPERIMENT 2026-07-15: destination search always starts from the
 * *source* CPU's own index (ctx->start = rq->cpu) and stops at the first
 * accepted Tier-1 candidate. Since that start point is fixed per source
 * CPU, every migration attempt from a given stolen CPU scans candidates
 * in the exact same order and piles onto the same first-found busy
 * target -- a likely thundering-herd contributor to the ~10x guest-level
 * holder-preemption regression. This cursor rotates the scan start so
 * consecutive searches (even from the same source) spread across
 * different targets instead.
 *
 * MEASURED WORSE: host-preempted improvement 1.49x->1.22x, iterations
 * -7.5%->-15.4%, holder preemption unchanged (~10.5%, no improvement).
 * Thundering-herd was not the driver of the holder-preemption cost.
 * Reverted to fixed per-source start; cursor kept unused for reference.
 */
unsigned long ivh_rr_cursor = 0;
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
//EXPERIMENT 2026-07-15: tried widening to 6ms (1.5x the HZ=250 tick period)
//on the theory that 1.5ms < tick period causes false "preempted" reads on
//healthy busy CPUs by sampling-phase alone. Measured WORSE: host-preempted
//improvement dropped ~1.49x->1.44x, iterations -12.8% (vs -7.5% at 1.5ms),
//and one migration ballooned to 203.9ms (vs 7-12ms max at 1.5ms) -- 6ms lets
//genuinely-preempted targets through. Reverted to 1.5ms, the best measured
//so far.
static int is_cpu_preempted(struct rq *rq, u64 now_time)
{
    u64 ref = rq->last_idle_tp > rq->clock_preempt
              ? rq->last_idle_tp : rq->clock_preempt;

    if (ref > now_time)
        return 0;

    /*
     * EXPERIMENT 2026-07-15 (second pass): 6ms = 1.5x the HZ=250 tick
     * period.  The 1.5ms threshold is BELOW the 4ms tick period, so on a
     * busy-but-healthy CPU (heartbeat only refreshed at tick) this check
     * false-fires ~60% of the time by sampling phase alone — measured
     * directly: 9182 PREEMPTED rejections of clean candidates vs 6565
     * accepts in one run, driving a 53% full-scan selection failure rate
     * (1423 of 2674 ivh_selected traces returned dst=-1), which left
     * threads spinning out their ~20ms waits on stolen vCPUs.  The gate
     * order makes the tight threshold redundant as steal protection:
     * GATE_CAPACITY_LOW (floor 850) has already excluded every actually
     * stolen candidate before this gate runs.  The first 6ms experiment
     * earlier tonight measured worse, but that was in the old regime
     * (no userspace-holder gate, extend grace 50us, eval cooldown drops,
     * active-balance escalation) where extra migrations amplified the
     * stacking damage; those are all fixed now.
     */
    return now_time - ref > 6000000ULL;
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
#define ACC_TIER1_ACTIVE  9  /* accepted: active (non-idle) worker */
#define ACC_TIER2_IDLE    10 /* accepted (fallback record): idle vCPU */
#define REJ_USER_LOCKHOLDER 11 /* target's curr holds a USERSPACE lock (rseq cr_counter) */
#define REJ_MAX           12

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, REJ_MAX);
    __type(key, u32);
    __type(value, u64);
} reject_reasons SEC(".maps");

/*
 * EXPERIMENT: capacity-floor sanity check, 2026-07-15. Is IVH_CAP_FLOOR
 * (850) actually calibrated against the live cpu_capacity signal, or is
 * it another stale constant like the 300us/1.5ms preemption threshold
 * was? Keyed by *candidate* cpu index (0-15), summed across all real
 * CPUs that evaluate it -- best-effort, not atomic-safe against every
 * race, good enough for a sanity average. Dump with:
 *   bpftool map dump name cap_sum ; bpftool map dump name cap_cnt
 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 16);
    __type(key, u32);
    __type(value, u64);
} cap_sum SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 16);
    __type(key, u32);
    __type(value, u64);
} cap_cnt SEC(".maps");

static __always_inline void bump_reason(u32 reason)
{
    u64 *val = bpf_map_lookup_elem(&reject_reasons, &reason);
    if (val)
        (*val)++;
}

/*
 * BUILD B (vcap retirement plan 2026-08-03, sec 6.2): runtime capacity-source
 * kill switch.
 *
 * Every capacity read in this program used to be an unconditional
 * select_rq->cpu_capacity, i.e. vcap's number, decided at compile time.  That
 * made the riskiest flip in the system a source edit + recompile + reload, and
 * the 2026-08-02 report sec 6.4 records an hour lost to a "revert" that did not
 * revert because the kernel sysctl was reverted while the BPF side was not.
 *
 * ivh_cfg[0] is the capacity source, using the SAME numbering as the kernel's
 * /proc/sys/kernel/ivh_cap_source:
 *     0 = vcap             -> rq->cpu_capacity        (default, today's behaviour)
 *     3 = ivh_uc_capacity  -> rq->ivh_uc_capacity     (the new in-kernel signal)
 * Values 1 and 2 are kernel-side-only concepts (shadow / Part C) and map to
 * vcap here, deliberately: this program has no Part C consumer.
 *
 * Keys 1-3 are reserved (max_entries 4) so a later knob costs a map update and
 * not a reload.
 *
 * Flip:     bpftool map update name ivh_cfg key 0 0 0 0 value 3 0 0 0
 * Rollback: bpftool map update name ivh_cfg key 0 0 0 0 value 0 0 0 0
 */
#define IVH_CFG_CAP_SOURCE   0
#define IVH_CAP_SRC_VCAP     0
#define IVH_CAP_SRC_UC       3

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 4);          /* key 0 = cap_source, 1-3 reserved */
    __type(key, u32);
    __type(value, u32);
} ivh_cfg SEC(".maps");

/*
 * The ONLY way this program is allowed to read a CPU's capacity.
 *
 * `src` must be resolved ONCE per scan and stashed in the per-scan context --
 * never re-read per candidate.  A mid-scan map write would otherwise produce a
 * half-vcap, half-uc candidate set, i.e. an unreproducible decision.  See
 * task_ctx.cap_source and its initialiser in test3().
 */
static __always_inline unsigned long ivh_cap_of(struct rq *rq, u32 src)
{
    return src == IVH_CAP_SRC_UC ? rq->ivh_uc_capacity : rq->cpu_capacity;
}

/* Read ivh_cfg[0] once, at the top of a scan. */
static __always_inline u32 ivh_cap_source_now(void)
{
    u32 key = IVH_CFG_CAP_SOURCE;
    u32 *v = bpf_map_lookup_elem(&ivh_cfg, &key);

    return v ? *v : IVH_CAP_SRC_VCAP;
}

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
 * RETIRED 2026-08-03: destination capacity floor, 2026-07-02 gate-combo
 * experiment.  Kept defined for reference only -- NOTHING READS IT.
 *
 * It required a migration target to be well above ivh_capacity_threshold (the
 * *source* trigger ceiling), on the theory that a single absolute constant on
 * the capacity scale is meaningful.  That is true of vcap's number and false
 * of rq->ivh_uc_capacity: ivh_uc_gate_recalibration_2026-08-03.md sec 2.4
 * measured the SAME eight host-contended vCPUs, under an UNCHANGED host
 * corunner, reading 506-520 (guest saturated), 742-823 (guest idle) and
 * 890-936 (sustained hackbench).  850 is on the correct side of the population
 * in two of those regimes and on the wrong side in the third -- and the third
 * is the regime IVH actually operates in.  See sec 4: "IVH_CAP_FLOOR as a
 * concept -- a fixed constant on the capacity scale -- is a vcap-shaped
 * artefact and does not survive vcap's retirement."
 */
#define IVH_CAP_FLOOR    850   /* RETIRED -- unused, see above */

/*
 * Gate reshape, ivh_uc_gate_recalibration_2026-08-03.md sec 5.1 / 5.2.
 *
 * The signal carries perfectly reliable ORDINAL information (100.00% correct
 * group ordering over 3,217 samples, worst-case inter-group gap 77) but has no
 * stable scale ORIGIN.  So both capacity gates are reshaped to read the
 * ordering rather than the absolute value:
 *
 *   GATE_CAPACITY_LOW  -> population-normalised top-band test against scan_max
 *   GATE_NOT_BETTER    -> relative test with a fixed absolute margin
 *
 * Values re-derived offline (V2) from this boot's own capture: a genuine
 * 0-lateral / 0-empty-destination-set plateau exists across
 * D in {0,25,50,75} x K in {25,50,75}, with (50,50) inside it and all four
 * orthogonal neighbours also clean -- 70,784 useful accepts, 0 lateral, 0 empty
 * over 8,848 source-samples.  (50,50) is therefore the centre of a plateau, not
 * a knife edge, and matches the values the document modelled.
 *
 * Both margins (50) sit BELOW the worst observed true inter-group gap (77) and
 * ABOVE the per-sample dispersion, so no single-sample perturbation can flip a
 * verdict.  This is what distinguishes the reshape from the Part C failure,
 * where the ranking itself was noise (see recalibration doc sec 4).
 */
#define IVH_CAP_TOPBAND     50   /* K: dest must be within K of the best CPU in this scan */
#define IVH_CAP_MARGIN      20   /* D: dest must beat source by at least D            */

/*
 * 2026-08-08 (WALL-path validation doc sec 4): SCALE-FREE margin.
 *
 * The absolute D above is only correct for a signal whose dynamic range is
 * stable.  ivh_uc_capacity's WALL variant (ivh_uc_used_source=0) is not: its
 * contended/clean spread wanders over a ~150-point range with recent load
 * history, because x_wall = 1 - steal/avail inherits the tick-gap estimator's
 * 6-24% metric recovery (final_tsc_only doc sec 6.4) and its denominator
 * `avail` is itself load-dependent.  Measured live, back-to-back protocol:
 *
 *   regime (cap_cont/cap_clean)   D=50            D=20
 *   compressed  ~980 / 1023       14.1-14.7 s     11.5 s
 *   wide        ~810 /  940       12.3-12.6 s     13.9-14.2 s
 *
 * A crossed interaction: no single D is right for both.  The fix is the same
 * one sec 5.1 already applied to the top-band test -- normalise to the
 * population actually observed in this scan instead of to a constant.
 *
 * Form: the destination must close at least half the gap between the source
 * and the best CPU seen this scan.  Written as `2*dcap >= src + scan_max` so
 * there is no subtraction to underflow when scan_max < src (which happens
 * whenever the source is itself the healthiest CPU in the scan).
 *
 * IVH_CAP_MARGIN_MIN remains as a pure noise rail: under uniform contention
 * scan_max collapses onto src, the midpoint collapses onto src too, and a
 * single LSB of jitter would otherwise flip the verdict -- exactly the Part C
 * migration-storm mechanism the absolute D was introduced to stop.
 *
 * Set IVH_CAP_MARGIN_REL to 0 to restore the plain absolute D.
 */
#define IVH_CAP_MARGIN_REL   0
#define IVH_CAP_MARGIN_MIN  20   /* noise rail under the relative form */
#define IVH_CAP_MARGIN_NUM   1   /* dest must close NUM/DEN of the src..scan_max gap */
#define IVH_CAP_MARGIN_DEN   3

/*
 * Vestigial absolute rail -- deliberately far from the operating point, and it
 * is a RAIL, NOT A CALIBRATION.  It exists so a scan in which every CPU is
 * deeply stolen cannot promote one of them to "best tier" and migrate onto it
 * (the saturated regime's 506-520 cluster).  It is never the binding
 * constraint in the idle or hackbench regimes.
 *
 * If reject_reasons[REJ_CAPACITY_LOW] ever goes to ~100%, this has become the
 * binding gate, the reshape has failed, and the answer is recalibration doc
 * sec 8 (publish steal/elapsed instead) -- NOT lowering this number.
 */
#define IVH_CAP_HARDFLOOR  880

struct task_ctx {
    struct task_struct *curr;          /* task that is to be moved */
    int *target_cpu_ptr;               /* result - where should the task be moved? */
    u64 now;
    int start;                         /* scan start offset (rotated, NOT necessarily the source CPU) */
    int source_cpu;                    /* actual source CPU -- never migrate here */
    u64 rq_last_preempt;               /* last_preemption of source rq (age reference) */
    int source_capacity;               /* capacity of the source vCPU, on cap_source's scale */
    int total_cpus;                    /* total number of CPUs in system */
    int average_capacity;              /* system average capacity, for EDWARDS-style gate */
    int *found_active_worker_ptr;      /* 1 once a Tier 1 (active worker) target is found */
    u32 cap_source;                    /* ivh_cfg[0], resolved ONCE per scan (see ivh_cap_of) */
    u32 scan_max;                      /* max capacity over all CPUs, resolved ONCE per scan */
};

/*
 * scan_max resolution pass (recalibration doc sec 5.1, and sec 7 risk 2).
 *
 * MUST be computed once, before the candidate bpf_loop(), and stashed in
 * task_ctx -- exactly the discipline the retirement plan sec 6.2 imposes on
 * cap_source, and for the same reason plus one more:
 *
 *   - reproducibility: scan_max is a live, unsynchronised read of 16 rqs.  Two
 *     evaluations microseconds apart would otherwise compute different
 *     scan_max values and reach different verdicts for the SAME candidate.
 *   - self-consistency: recomputing per candidate would let the top-band test
 *     compare candidate i against a population snapshot that candidate j never
 *     saw, i.e. a candidate set that is half-old and half-new.
 *
 * The max is taken over ALL CPUs including the source, matching the offline
 * model that produced the (D,K) plateau -- that model's per-sample max was
 * likewise over all 16 CPUs.
 */
struct scan_max_ctx {
    u32 max;
    u32 cap_source;
};

static int scan_max_cpu(u32 iter, void *data)
{
    struct scan_max_ctx *c = data;
    struct rq *rq_i = bpf_per_cpu_ptr(&runqueues, iter);
    unsigned long cap;

    if (!rq_i)
        return 0;

    cap = ivh_cap_of(rq_i, c->cap_source);
    if (cap > c->max)
        c->max = (u32)cap;

    return 0;
}

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

    /* EXPERIMENT: capacity-floor sanity check, 2026-07-15 (see cap_sum/cap_cnt above). */
    {
        u32 key = (u32)cpu;
        u64 *sum = bpf_map_lookup_elem(&cap_sum, &key);
        u64 *cnt = bpf_map_lookup_elem(&cap_cnt, &key);
        if (sum && cnt) {
            __sync_fetch_and_add(sum, ivh_cap_of(select_rq, ctx->cap_source));
            __sync_fetch_and_add(cnt, 1);
        }
    }

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

    /*
     * EXPERIMENT 2026-07-15: USERSPACE lockholder gate.
     *
     * task->lock_depth is only maintained by the KERNEL spinlock wrappers
     * (kernel/locking/spinlock.c).  A userspace CS holder (NHextend2 /
     * pthread_spin_lock via the nptl patch) marks its critical section in
     * rseq->cr_counter (bits [31:2] = nesting depth, set by inc_extend()
     * in userspace) — its lock_depth stays 0 the whole time.  So the gate
     * above NEVER protects the userspace holder: migrants land on the
     * holder's CPU, the enqueue path's wakeup preemption sets a HARD
     * need-resched (which the rseq extension cannot defer), and the holder
     * loses the CPU mid-CS for a full mate-slice.  This was the dominant
     * driver of the ~10x guest-level holder-preemption elevation.
     *
     * cr_counter lives in USER memory.  bpf_probe_read_user() reads the
     * CALLING task's address space, so the check is only valid when the
     * target's curr shares the caller's mm (same process — exactly the
     * case that matters: one NHextend2 thread evaluating a CPU running a
     * sibling thread).  For foreign-mm targets, skip the check (behaves
     * as before).  copy_from_user_nofault semantics: safe in atomic
     * context, returns -EFAULT instead of faulting.
     */
    {
        struct task_struct *tcurr = select_rq->curr;

        if (tcurr->mm && ctx->curr->mm && tcurr->mm == ctx->curr->mm) {
            struct rseq *urseq = tcurr->rseq;

            if (urseq) {
                u32 cr = 0;

                if (!bpf_probe_read_user(&cr, sizeof(cr), &urseq->cr_counter)
                    && (cr & ~3u)) {
                    bump_reason(REJ_USER_LOCKHOLDER);
                    return 0;
                }
            }
        }
    }
#endif

#if GATE_SPINNER
    /*
     * Skip spinners: already queued in an MCS waiter list.
     * - lock free:  spinner already judged this vCPU safe for itself —
     *               migrating here risks thrashing.
     * - lock held:  migrated task joins queue behind existing waiter,
     *               the waiter preemption problem.
     *
     * EXPERIMENT 2026-07-15: tried enabling this to reduce the busy-target
     * stacking that was driving ~10x guest-level holder preemption. Measured
     * WORSE on every axis: host-preempted improvement 1.49x->1.27x,
     * iterations -7.5%->-17.4%, holder preemption unchanged (~10.5%, no
     * improvement). Reverted to 0.
     */
    if (select_rq->curr->wait_depth > 0) {
        bump_reason(REJ_SPINNER);
        return 0;
    }
#endif

#if GATE_CAPACITY_LOW
    /* Reshaped 2026-08-03 (recalibration doc sec 5.1): population-normalised
     * top-band test, plus a vestigial absolute rail.  Replaces the retired
     * absolute IVH_CAP_FLOOR, which is a vcap-shaped artefact (see its comment
     * above).  Asking "is this destination in the healthiest tier currently
     * observable" is scale-free by construction, so the 126-point regime drift
     * of sec 2.4 moves the threshold WITH the population instead of leaving it
     * behind. */
    {
        unsigned long dcap = ivh_cap_of(select_rq, ctx->cap_source);

        /* Rail first: never migrate onto a catastrophically stolen vCPU, even
         * if it happens to be the best one in this scan. */
        if (dcap <= IVH_CAP_HARDFLOOR) {
            bump_reason(REJ_CAPACITY_LOW);
            return 0;
        }

        /* Top band: within IVH_CAP_TOPBAND of the best CPU seen this scan.
         * Written as an addition on the left rather than a subtraction on the
         * right so it cannot underflow when scan_max < IVH_CAP_TOPBAND. */
        if (dcap + IVH_CAP_TOPBAND < ctx->scan_max) {
            bump_reason(REJ_CAPACITY_LOW);
            return 0;
        }
    }
#endif

#if GATE_NOT_BETTER
    /* Reshaped 2026-08-03 (recalibration doc sec 5.2): target must beat the
     * source by a fixed absolute MARGIN, not merely be strictly greater.
     *
     * A bare `>` is flipped by a single LSB of noise, which is exactly the
     * mechanism behind the Part C migration storm.  A margin wider than the
     * signal's per-sample dispersion is not.  Modelled at D=50 this removes
     * 100% of lateral (contended -> contended) acceptances while keeping every
     * useful accept and emptying no destination set.
     *
     * It also handles the uniform-contention case BETTER than the old absolute
     * floor did: if every CPU is equally stolen, nothing clears src + D, the
     * destination set empties, and IVH correctly does nothing -- by
     * construction rather than by accident of where a constant happened to
     * sit. */
#if IVH_CAP_MARGIN_REL
    {
        unsigned long dcap = ivh_cap_of(select_rq, ctx->cap_source);
        unsigned long src  = (unsigned long)ctx->source_capacity;

        /* Noise rail first -- cheap, and it is the binding one under uniform
         * contention where the midpoint test degenerates. */
        if (dcap < src + IVH_CAP_MARGIN_MIN) {
            bump_reason(REJ_NOT_BETTER);
            return 0;
        }
        /* Close at least IVH_CAP_MARGIN_NUM/IVH_CAP_MARGIN_DEN of the
         * src..scan_max gap.  Multiplied out so there is no subtraction to
         * underflow when scan_max < src. */
        if (dcap * IVH_CAP_MARGIN_DEN <
            src * (IVH_CAP_MARGIN_DEN - IVH_CAP_MARGIN_NUM)
            + (unsigned long)ctx->scan_max * IVH_CAP_MARGIN_NUM) {
            bump_reason(REJ_NOT_BETTER);
            return 0;
        }
    }
#else
    if (ivh_cap_of(select_rq, ctx->cap_source)
        < (unsigned long)ctx->source_capacity + IVH_CAP_MARGIN) {
        bump_reason(REJ_NOT_BETTER);
        return 0;
    }
#endif
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
     * less remaining burst time and is closer to being preempted. */
    if (select_rq->last_preemption <= ctx->rq_last_preempt) {
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

    if (idle_cpu(select_rq)) {
        /*
         * Tier 1 (preferred): idle vCPU. Migrating a spinner onto a busy
         * clean CPU just stacks it behind whoever's already running there
         * — including, sometimes, the very holder we're trying to protect,
         * which is what was driving the ~10x holder-preemption regression
         * measured 2026-07-15. An idle target costs a hypervisor wake-up,
         * but the thread runs immediately instead of queuing. Stop search.
         */
        bump_reason(ACC_TIER2_IDLE);
        *target_cpu_ptr = (int)(cpu);
        *(ctx->found_active_worker_ptr) = 1;
        return 1;
    }

    /* Tier 2 (fallback): active/busy vCPU. No wake-up cost, but the thread
     * queues behind existing work. Keep searching for a Tier 1 idle target
     * first; only settle for this if none exists. */
    if (!*(ctx->found_active_worker_ptr)) {
        bump_reason(ACC_TIER1_ACTIVE);
        *target_cpu_ptr = (int)(cpu);
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

    int start = 0;
    u32 nr_loops = total_cpus - 1;
    int target_cpu = -1;

    int found_active_worker = 0;

    /* BUILD B: resolve the capacity source ONCE, here, before the bpf_loop().
     * Both the source's own capacity and every candidate's capacity are then
     * read on the same scale for the whole scan. */
    u32 cap_src = ivh_cap_source_now();

    /* Gate reshape (recalibration doc sec 5.1): resolve the population maximum
     * ONCE, here, before the candidate bpf_loop() -- see scan_max_cpu(). */
    struct scan_max_ctx smc = { .max = 0, .cap_source = cap_src };

    bpf_loop(total_cpus, &scan_max_cpu, &smc, 0);

    struct task_ctx task_context = {
        .curr = curr,
        .target_cpu_ptr = &target_cpu,
        .now = now_time,
        .start = rq->cpu,
        .source_cpu = rq->cpu,
        .rq_last_preempt = rq->last_preemption,
        .source_capacity = ivh_cap_of(rq, cap_src),
        .total_cpus = total_cpus,
        .average_capacity = average_capacity,
        .found_active_worker_ptr = &found_active_worker,
        .cap_source = cap_src,
        .scan_max = smc.max
    };

    unsigned long mm_bits = curr->mm ? curr->mm->cpu_bitmap[0] : 0UL;

    /* EXPERIMENT 2026-08-06 (ivh_four_questions_report, Q4 sec 1.3): both
     * capacity gates are monotone in dcap, and dcap <= scan_max for every
     * candidate by construction, so if scan_max itself cannot clear
     * GATE_CAPACITY_LOW's hard rail or GATE_NOT_BETTER's margin, NO candidate
     * can -- the whole per-candidate loop is provably dead work. Measured
     * 80% regression recovery in isolation; being tested here stacked on top
     * of ivh_selection_trylock. */
    /* The early-out must DOMINATE the per-candidate gate: it may only reject
     * scans in which no candidate could possibly pass.  Under the relative
     * margin the per-candidate test at dcap == scan_max reduces to
     * scan_max >= src (implied), so the binding term is the noise rail. */
#if IVH_CAP_MARGIN_REL
    if (smc.max <= IVH_CAP_HARDFLOOR ||
        smc.max < (u32)ivh_cap_of(rq, cap_src) + IVH_CAP_MARGIN_MIN)
        return -1;
#else
    if (smc.max <= IVH_CAP_HARDFLOOR ||
        smc.max < (u32)ivh_cap_of(rq, cap_src) + IVH_CAP_MARGIN)
        return -1;
#endif

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
    u32 cap_source;                    /* ivh_cfg[0], resolved once per scan */
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

    /* NOTE: sched/cfs_latency_select has NO kernel call site (verified
     * 2026-08-03: declared in include/linux/sched_hook_defs.h:11, never
     * invoked -- see the comment at kernel/sched/core.c:1052).  These reads
     * are dead today, but they are converted to ivh_cap_of() anyway so that
     * no vcap-scale read survives anywhere in this program if the hook ever
     * gains a call site.  Costs nothing. */

    //if the target is uncontested - no reason to hesitate
    if (ivh_cap_of(select_rq, ctx->cap_source) > 1000) {
        *target_cpu_ptr = cpu;
        return 1;
    }

    //path if there are only SCHED-IDLE tasks running
    if (select_rq->nr_running > 0) {
        //if it's a better than average core - that's good enough!
        if (ivh_cap_of(select_rq, ctx->cap_source) > (ctx->average_capacity)) {
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
        if (ivh_cap_of(select_rq, ctx->cap_source) > (ctx->average_capacity) &&
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
        .average_capacity = average_capacity,
        .cap_source = ivh_cap_source_now()
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
