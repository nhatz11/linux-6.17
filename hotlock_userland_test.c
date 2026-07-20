/*
 * Pure userspace validation of the Hotlock concept: can a per-lock decaying
 * contention-history EWMA (the exact formula kernel/locking/spinlock.c's
 * ivh_hotlock_update()/ivh_hotlock_read() use) actually differentiate a
 * heavily-contended lock (like NHextend's benchmark lock) from a rarely-used
 * one (simulating sshd's incidental, low-contention kernel spinlock use) --
 * with ZERO kernel involvement, no syscalls, no rebuild needed.
 *
 * Three locks, three thread populations:
 *   HOT  lock: N threads hammer it in a tight loop, long CS (NHextend's
 *              pattern) -- real, sustained queueing.
 *   COLD lock: 1 thread takes it occasionally with real work in between
 *              (sshd doesn't spin -- it takes a lock, does I/O, moves on).
 *   FREQ lock: M threads hammer it in a tight loop, but with a CS so short
 *              that acquisitions almost always resolve before anyone else
 *              arrives (hackbench's actual pattern: 100% of measured kernel
 *              lock acquisitions in that workload resolve via the qspinlock
 *              pending-bit fast path, never showing a live waiter). High
 *              attempt RATE, near-zero instantaneous queueing.
 *
 * Two independent classifiers are run side by side over all three locks:
 *
 *   Model A (existing, matches the kernel's current ivh_hotlock_update()):
 *     EWMA of the boolean "was there a live waiter when I attempted this
 *     acquisition". Correctly flags HOT (real queueing) and COLD (true
 *     idle), but reads FREQ as cold too -- by design, since instantaneous
 *     queue depth for a short-CS lock really is ~0 most of the time. This
 *     is the open question: is "never queues" the same as "doesn't matter
 *     if its holder gets preempted"? For a lock touched constantly by many
 *     threads, a stall on *any* single holder has much higher odds of
 *     overlapping with someone else's attempt than the same stall would on
 *     a lock touched once an hour -- even though steady-state queue depth
 *     reads identically ~0 for both.
 *
 *   Model B (new, proposed): a leaky-bucket attempt-rate estimator, decoupled
 *     from contention entirely. Every attempt (contended or not) adds a
 *     fixed increment; the value decays continuously by *elapsed wall-clock
 *     time* since the last touch (a real half-life), not by call count. This
 *     is deliberately NOT "EWMA with a bigger k" -- the existing EWMA only
 *     moves when fed a sample, and samples are piggybacked on acquisition
 *     events, so an unconditional sample=1 would just latch permanently hot
 *     the moment any attempt happens, never decaying between bursts. A real
 *     time-based half-life is required for "used often" and "used rarely"
 *     to actually separate. This prototype uses floating point + a per-bucket
 *     mutex for clarity; a real kernel version would need an integer
 *     fixed-point approximation (e.g. right-shift by elapsed/HALF_LIFE) and
 *     lockless atomics, same as Model A's table.
 *
 * A small direct-mapped table (same fixed-point EWMA design as the kernel
 * version, just sized for a handful of locks instead of 4096 buckets) tracks
 * all three locks by address and reports a live verdict every second from
 * both models, plus a final summary comparing each against ground-truth
 * measured contention rate (Model A) and raw attempt rate (Model B).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <stdatomic.h>

#define rmb() asm volatile ("lfence" ::: "memory")

/* Same fixed-point scale as the kernel design (include/linux/bpf_sched.h). */
#define IVH_HOTLOCK_SCALE  10
#define IVH_HOTLOCK_HALF   (1 << (IVH_HOTLOCK_SCALE - 1))
#define HOTLOCK_TABLE_SIZE 256 /* small direct-mapped table, plenty for a demo */

struct hotlock_entry {
        const void        *lock;
        atomic_int         history; /* EWMA, IVH_HOTLOCK_SCALE fixed point */
        atomic_ullong      samples;
        atomic_ullong      contended_samples;
};

static struct hotlock_entry hotlock_table[HOTLOCK_TABLE_SIZE];
static int ewma_k = 3; /* smoothing factor, matches kernel default order of magnitude */

static unsigned hash_ptr(const void *p)
{
        /* Multiplicative (Fibonacci) hash, same family as the kernel's
         * hash_ptr()/hash_64() -- a plain shift-and-modulo doesn't spread
         * closely-spaced static addresses (two `unsigned long` globals
         * declared next to each other can land only 8 bytes apart, which
         * collided in bucket 1 with a naive (addr>>4)%N hash). Multiplying
         * by a large odd constant mixes the bits thoroughly before taking
         * the top bits, so even adjacent addresses land in different
         * buckets. */
        unsigned long v = (unsigned long)p;
        v *= 0x9E3779B97F4A7C15ULL; /* 2^64 / golden ratio */
        return (unsigned)(v >> (64 - 8)); /* top 8 bits -> 0..255 for HOTLOCK_TABLE_SIZE=256 */
}

static struct hotlock_entry *hotlock_slot(const void *lock)
{
        struct hotlock_entry *e = &hotlock_table[hash_ptr(lock)];

        if (atomic_load(&e->lock) != lock) {
                /* Cold-start on a bucket collision, same as the kernel design. */
                atomic_store((_Atomic(const void *) *)&e->lock, lock);
                atomic_store(&e->history, 0);
                atomic_store(&e->samples, 0);
                atomic_store(&e->contended_samples, 0);
        }
        return e;
}

static void hotlock_update(const void *lock, bool contended)
{
        struct hotlock_entry *e = hotlock_slot(lock);
        int sample = contended ? (1 << IVH_HOTLOCK_SCALE) : 0;
        int old, new;

        atomic_fetch_add(&e->samples, 1);
        if (contended)
                atomic_fetch_add(&e->contended_samples, 1);

        old = atomic_load(&e->history);
        do {
                new = old + ((sample - old) >> ewma_k);
                if (new == old)
                        return;
        } while (!atomic_compare_exchange_weak(&e->history, &old, new));
}

static int hotlock_read(const void *lock)
{
        struct hotlock_entry *e = hotlock_slot(lock);
        return atomic_load(&e->history);
}

static bool hotlock_is_hot(const void *lock)
{
        return hotlock_read(lock) > IVH_HOTLOCK_HALF;
}

/* ---- Model B: leaky-bucket attempt-rate estimator (decoupled from
 * contention, decays by elapsed wall-clock time, not by call count) ---- */

#define FREQ_HALF_LIFE_NS   20000000.0  /* 20ms: tune after seeing real numbers */
#define FREQ_HOT_THRESHOLD  5.0         /* tune after seeing real numbers */

struct freq_entry {
        const void      *lock;
        double           rate;
        unsigned long long last_ns;
        pthread_mutex_t  mtx;
};

static struct freq_entry freq_table[HOTLOCK_TABLE_SIZE];

static unsigned long long now_ns(void)
{
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
}

static void freq_table_init(void)
{
        for (int i = 0; i < HOTLOCK_TABLE_SIZE; i++)
                pthread_mutex_init(&freq_table[i].mtx, NULL);
}

/* Called on EVERY attempt, contended or not -- unlike Model A this never
 * skips a sample. Decay is applied for however much wall-clock time actually
 * elapsed since the last touch, so a lock that goes quiet really does cool
 * back down, instead of just freezing at whatever it last read. */
static void freq_update(const void *lock)
{
        struct freq_entry *e = &freq_table[hash_ptr(lock)];
        unsigned long long t = now_ns();

        pthread_mutex_lock(&e->mtx);
        if (e->lock != lock) {
                e->lock = lock;
                e->rate = 0.0;
                e->last_ns = t;
        }
        double halflives = (double)(t - e->last_ns) / FREQ_HALF_LIFE_NS;
        e->rate = e->rate * pow(0.5, halflives) + 1.0;
        e->last_ns = t;
        pthread_mutex_unlock(&e->mtx);
}

/* Applies the same time-based decay for a read that isn't itself a fresh
 * attempt, so the reported value reflects "how hot right now", not "how hot
 * as of the last attempt". */
static double freq_read(const void *lock)
{
        struct freq_entry *e = &freq_table[hash_ptr(lock)];
        unsigned long long t;
        double r;

        pthread_mutex_lock(&e->mtx);
        t = now_ns();
        r = (e->lock == lock) ? e->rate * pow(0.5, (double)(t - e->last_ns) / FREQ_HALF_LIFE_NS) : 0.0;
        pthread_mutex_unlock(&e->mtx);
        return r;
}

static bool freq_is_hot(const void *lock)
{
        return freq_read(lock) > FREQ_HOT_THRESHOLD;
}

/* ---- Model C: event-driven per-lock utilization / duty-cycle (rho) ----
 *
 * rho = fraction of wall-clock time the lock is actually HELD
 *     = throughput * mean_service_time  (queueing-theory utilization)
 *     = P(an arriving acquirer finds the lock busy)   [PASTA / M/G/1].
 *
 * The whole point vs Model B is that this is estimated *event-drivenly* at
 * each RELEASE, with NO wall-clock read-time decay. Per release we know:
 *   busy  = cs_ns   (how long THIS hold lasted)
 *   cycle = rel_ns - last_rel_ns   (time between consecutive releases of
 *           this lock, across all threads)
 * and the per-cycle duty fraction cs/cycle is EWMA'd over release events with
 * the exact same fixed-point machinery as Model A. Because the estimator only
 * ever moves on a real release event and the READ applies no time-based decay,
 * a lock that goes silent (e.g. because this vCPU's clock was stolen out from
 * under it) FREEZES at its last value -- exactly like Model A, and unlike
 * Model B which cools toward 0 by wall-clock during the very stall IVH exists
 * to catch. It captures the CS-length axis Model A ignores: a saturated lock
 * (handoff cycle ~= cs) reads ~1.0; a short-CS lock touched with real gaps in
 * between reads ~0 no matter how HIGH its raw attempt frequency is (fixing
 * Model B's inverse-frequency flicker on hot_lock).
 *
 * We also track a plain EWMA of the CS length itself (mean_cs_ns) so we can
 * evaluate the alternative, LHP-motivated combination "does-it-queue (Model A)
 * x how-long-is-the-hold (mean_cs)" separately from pure utilization.
 *
 * NOTE ON STEAL SEMANTICS: the kernel measures cs_ns with sched_clock(), which
 * on this KVM guest (CONFIG_PARAVIRT_CLOCK=y, kvm-clock) is host monotonic time
 * -- it INCLUDES stolen time, same as CLOCK_MONOTONIC used here. A hold that
 * was preempted mid-CS therefore shows an inflated cs_ns automatically, which
 * is a feature: mean_cs naturally spikes for locks whose holders keep getting
 * caught by steal. The kernel additionally has paravirt_steal_clock() (already
 * read by IVH's get_steal_and_preemptions()) if a steal-*adjusted* duty cycle
 * is ever wanted; see the report. */
#define UTIL_SCALE 1024
#define UTIL_HALF  512
static int util_k = 3;

struct util_entry {
        const void        *lock;
        atomic_int         util;         /* EWMA of cs/cycle, UTIL_SCALE fixed pt */
        atomic_ullong      last_rel_ns;  /* release ts of the previous hold */
        atomic_llong       mean_cs_ns;   /* EWMA of CS length, ns */
};

static struct util_entry util_table[HOTLOCK_TABLE_SIZE];

static void util_update_release(const void *lock, unsigned long long acq_ns,
                                unsigned long long rel_ns)
{
        struct util_entry *e = &util_table[hash_ptr(lock)];
        unsigned long long cs, cycle, prev;
        long long mc;
        int sample, old, new;

        if (atomic_load((_Atomic(const void *) *)&e->lock) != lock) {
                atomic_store((_Atomic(const void *) *)&e->lock, lock);
                atomic_store(&e->util, 0);
                atomic_store(&e->last_rel_ns, 0);
                atomic_store(&e->mean_cs_ns, 0);
        }

        cs = rel_ns - acq_ns;

        /* mean-CS EWMA (signed delta so it can decay down too) */
        mc = atomic_load(&e->mean_cs_ns);
        atomic_store(&e->mean_cs_ns, mc + (((long long)cs - mc) >> util_k));

        /* utilization EWMA over the release->release cycle */
        prev = atomic_exchange(&e->last_rel_ns, rel_ns);
        if (prev == 0 || rel_ns <= prev)
                return; /* first sample, or clock went backwards: skip */
        cycle = rel_ns - prev;
        sample = (int)((cs * (unsigned long long)UTIL_SCALE) / cycle);
        if (sample > UTIL_SCALE)
                sample = UTIL_SCALE; /* nested/overlap rounding guard */

        old = atomic_load(&e->util);
        do {
                new = old + ((sample - old) >> util_k);
                if (new == old)
                        return;
        } while (!atomic_compare_exchange_weak(&e->util, &old, new));
}

/* NO time-based decay on read -- this is the steal-robustness property. */
static int util_read(const void *lock)
{
        struct util_entry *e = &util_table[hash_ptr(lock)];
        return (atomic_load((_Atomic(const void *) *)&e->lock) == lock)
                       ? atomic_load(&e->util) : 0;
}

static long long util_mean_cs(const void *lock)
{
        struct util_entry *e = &util_table[hash_ptr(lock)];
        return (atomic_load((_Atomic(const void *) *)&e->lock) == lock)
                       ? atomic_load(&e->mean_cs_ns) : 0;
}

static bool util_is_hot(const void *lock)
{
        return util_read(lock) > UTIL_HALF;
}

/* ---- Model D: gated combination -- the actual recommendation ----
 *
 * hot := (live_waiters>0 OR Model-A history hot)   AND   mean_cs >= FLOOR
 *
 * Contention (does anyone else ever wait) is the necessary condition, exactly
 * as today -- Model D never fires on a lock nobody queues on, which is what
 * kept Model C's rho from producing the busy_lock false positive (rho alone
 * has no contention requirement at all). CS length is a second, SUPPRESSING
 * gate on top of that: even a lock that genuinely queues isn't worth
 * migrating over if the hold is shorter than IVH's own migration overhead
 * would take to react -- there's nothing to protect against by the time the
 * migration would land. FLOOR is a placeholder pending a real measurement of
 * that overhead; it only has to sit between "genuinely nanosecond" and
 * "genuinely millisecond" for this prototype to prove the concept. */
#define MEANCS_FLOOR_NS  1000 /* 1us: tune against measured IVH migration cost */

static bool modeld_is_hot(const void *lock, int live_waiters)
{
        bool contention_signal = (live_waiters > 0) || hotlock_is_hot(lock);
        return contention_signal && (util_mean_cs(lock) >= MEANCS_FLOOR_NS);
}

/* ---- Model E: contended utilization -- the proposed replacement for D ----
 *
 * hot := util(rho) is high   AND   the lock genuinely has contention history
 *
 * This is Model C (event-driven, steal-robust duty cycle) gated by the SAME
 * real-contention requirement Model D used to kill Model C's busy_lock false
 * positive -- but with the CS-length FLOOR DELETED. The owner's objection to D
 * was that the CS floor is an unconditional categorical veto: a short-CS lock
 * can NEVER qualify no matter how heavily used. Model E removes that veto and
 * lets utilization itself be the graded "popularity/importance" axis:
 *
 *   - A short-CS lock is NOT categorically excluded. It qualifies IF it is
 *     held a large fraction of wall-clock time -- which for a short CS simply
 *     requires a correspondingly high acquire rate. rho = throughput*mean_CS
 *     is exactly "fraction of time held," so a genuinely popular short-CS lock
 *     (very high throughput) rises on its own, while an incidental short-CS
 *     lock stays low. No hard floor, fully graded (owner's requirement).
 *   - busy_lock still suppressed: rho is high but there is NO contention
 *     history, so the AND-gate rejects it (single thread strands no one).
 *   - freq_lock / tiny_lock stay COLD not by a floor but because their rho is
 *     structurally tiny (0.001 / 0.03): a short CS with real gaps or merely
 *     nanosecond holds is simply not held much of the time, so its holder is
 *     almost never caught preempted mid-CS -- the real physical reason LHP
 *     protection can't help them, expressed as a continuous quantity instead
 *     of a cliff.
 *
 * The honest boundary (see report): E protects a short-CS lock only once it is
 * held >~50% of wall-clock. A merely-frequent-but-mostly-idle short-CS lock
 * (freq_lock at 61K/s but rho 0.001) still reads COLD. That is not a bug: such
 * a lock's holder is preempted-while-holding <0.1% of the time, so there is
 * almost nothing for IVH to protect regardless of how "popular" it looks. */
static bool modele_is_hot(const void *lock, int live_waiters)
{
        bool contention_signal = (live_waiters > 0) || hotlock_is_hot(lock);
        return contention_signal && util_is_hot(lock);
}

/* ---- Model F: contention as the popularity axis, SOFT CS weight ----
 *
 * The key realization (see report + measured hotpath/freq numbers): the
 * "workload lock vs daemon lock nobody cares about" distinction the owner
 * actually wants is ALREADY the contention signal (Model A), and it is
 * completely CS-length-independent -- daemon/idle locks (cold, busy, freq)
 * show no live waiters; real workload locks (hot, tiny, hotpath) do. Model A
 * alone already answers "is this a lock a real workload depends on."
 *
 * Model D's CS FLOOR was never a popularity signal at all -- it answered a
 * SECOND, orthogonal question: "is the hold long enough that a migration could
 * land in time to help." The owner's objection is exactly right: baking that
 * in as a hard categorical veto wrongly and permanently excludes a genuinely
 * important, genuinely contended, but short-CS lock (hotpath). rho (Model E)
 * does NOT rescue it -- measured hotpath rho ~0.28, lumped with tiny.
 *
 * Model F keeps contention as the (CS-independent) popularity gate, but
 * replaces the hard floor with a SOFT, saturating benefit weight:
 *
 *   score = P(contended) * (1 - exp(-mean_cs / TAU))
 *
 * TAU is the migration-cost scale (how long a hold must be for a migration to
 * be worth starting). Unlike a floor, nothing is categorically excluded: a
 * short-CS lock is proportionally down-ranked, not vetoed, and a strongly
 * contended one can still clear the bar. This is what lets hotpath (366ns,
 * ~100% contended) qualify while tiny (27ns) does not -- a graded ~13x
 * separation, not a cliff. TAU here reuses D's 1us placeholder as a soft KNEE
 * rather than a hard edge; it and the threshold both still need the real,
 * still-unmeasured kernel migration cost to be set for production. */
#define MODELF_TAU_NS   1000.0
#define MODELF_THRESH   0.10

static double modelf_score(const void *lock, int live_waiters)
{
        bool contended = (live_waiters > 0) || hotlock_is_hot(lock);
        double p = contended ? ((double)hotlock_read(lock) / (1 << IVH_HOTLOCK_SCALE)) : 0.0;
        if (contended && p < 0.5)
                p = 0.5; /* live waiter now but history still warming: floor the weight */
        double cs = (double)util_mean_cs(lock);
        double soft = 1.0 - exp(-cs / MODELF_TAU_NS);
        return p * soft;
}

static bool modelf_is_hot(const void *lock, int live_waiters)
{
        return modelf_score(lock, live_waiters) > MODELF_THRESH;
}

/* ---- the six locks under test ---- */

static volatile unsigned long hot_lock __attribute__((aligned(4096))) = 0;
static volatile unsigned long cold_lock __attribute__((aligned(4096))) = 0;
static volatile unsigned long freq_lock __attribute__((aligned(4096))) = 0;
/* BUSY: a single thread that holds a moderately-long CS and re-acquires
 * immediately with no gap -- so the lock is held ~100% of wall-clock time
 * (rho ~= 1.0) but is touched by exactly ONE thread, so nobody ever queues
 * and preempting its holder strands no one. This is the case that separates
 * "utilization" from "LHP risk": Model C (rho) should call it HOT (a FALSE
 * POSITIVE), while Model A (real waiters) correctly calls it COLD. */
static volatile unsigned long busy_lock __attribute__((aligned(4096))) = 0;
/* TINY: the actual case Model D exists for. Several threads, tight loop, NO
 * gap, CS as short as freq_lock's -- this genuinely queues (ground truth
 * contention should land near 100%, same mechanism as the original
 * over-eager freq_lock design before it was corrected to 2 threads + a work
 * gap: many threads with zero backoff on one lock WILL contend regardless of
 * CS length). Model A will correctly call this HOT (real waiters do exist).
 * The question is whether migrating is worth it when the hold is only tens
 * of nanoseconds -- Model D should override to COLD via the mean_cs floor. */
static volatile unsigned long tiny_lock __attribute__((aligned(4096))) = 0;
/* HOTPATH: the "important short-CS workload lock" the owner is worried about.
 * Many threads, real sustained contention, NO gap -- a genuinely heavily-used
 * lock -- but a SHORT CS deliberately placed BELOW Model D's 1000ns floor
 * (target ~400-700ns). This is the case Model D categorically vetoes purely
 * for being short (owner's exact complaint) while Model E must be able to call
 * HOT on its own merits when its utilization is genuinely high. It is longer
 * than tiny_lock's nanosecond CS but still "short" in absolute terms. */
static volatile unsigned long hotpath_lock __attribute__((aligned(4096))) = 0;
static volatile int done = 0;

/* Stall injection: simulates a host steal event. When set, every worker
 * parks WITHOUT touching any lock (no acquisitions, no model updates) -- i.e.
 * this vCPU's threads make no forward progress, exactly as if the host
 * descheduled the vCPU. Model A / Model C (event-driven) must freeze; Model B
 * (wall-clock decay) will cool toward 0 during the window. */
static atomic_int paused = 0;
static void cpu_pause(void) { asm volatile ("pause" ::: "memory"); }
static void park_if_paused(void)
{
        while (atomic_load(&paused))
                cpu_pause();
}

static unsigned long cmpxchg_ul(volatile unsigned long *ptr, unsigned long old, unsigned long new)
{
        unsigned long prev;
        asm volatile("lock; cmpxchg %2,%1"
                     : "=a"(prev), "+m"(*ptr)
                     : "r"(new), "0"(old)
                     : "memory");
        return prev;
}

static unsigned long long hot_acquires = 0, hot_contended_acquires = 0;
static unsigned long long cold_acquires = 0, cold_contended_acquires = 0;
static unsigned long long freq_acquires = 0, freq_contended_acquires = 0;
static unsigned long long busy_acquires = 0, busy_contended_acquires = 0;
static unsigned long long tiny_acquires = 0, tiny_contended_acquires = 0;
static unsigned long long hotpath_acquires = 0, hotpath_contended_acquires = 0;
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;

/* Live per-lock waiter counts -- matches the real kernel design
 * (ivh_hotlock_note_waiter_enter/exit + ivh_hotlock_waiters()) more
 * faithfully than sampling only "did my own attempt collide": a live count
 * of threads currently waiting is real-time and doesn't require two
 * threads' attempts to land in the same instant to register as
 * contention -- important on a host where real steal already spaces
 * thread execution out in wall-clock time. */
static atomic_int hot_waiters = 0;
static atomic_int cold_waiters = 0;
static atomic_int freq_waiters = 0;
static atomic_int busy_waiters = 0;
static atomic_int tiny_waiters = 0;
static atomic_int hotpath_waiters = 0;

/* HOT: NHextend-style tight spin, no backoff -- this is the "would really
 * suffer from LHP" workload: many threads, always runnable, always wanting
 * the lock right now. */
static void *hot_thread(void *arg)
{
        (void)arg;
        /* Deliberately unpinned -- threads float across all 16 CPUs,
         * including this host's stolen half. That's the point: Hotlock
         * needs to differentiate hot from cold locks under exactly this
         * kind of real host contention, not in a sanitized clean-CPU-only
         * environment. */
        while (!done) {
                park_if_paused();
                /* Matches ivh_pre_lock()'s actual check: sample how many
                 * OTHER threads are already waiting BEFORE I join, not
                 * whether my own cmpxchg happens to collide. */
                bool contended = atomic_load(&hot_waiters) > 0;

                atomic_fetch_add(&hot_waiters, 1);
                while (cmpxchg_ul(&hot_lock, 0, 1) != 0)
                        rmb();
                atomic_fetch_sub(&hot_waiters, 1);

                hotlock_update((const void *)&hot_lock, contended);
                freq_update((const void *)&hot_lock);
                pthread_mutex_lock(&stats_lock);
                hot_acquires++;
                if (contended) hot_contended_acquires++;
                pthread_mutex_unlock(&stats_lock);

                /* Real critical section (~NHextend's loop_spin pattern) --
                 * long enough that 8 threads genuinely queue on this lock
                 * instead of just missing each other in time. */
                unsigned long long acq_ns = now_ns();
                for (int i = 0; i < 3000000; i++)
                        asm volatile ("sfence" ::: "memory");

                hot_lock = 0;
                util_update_release((const void *)&hot_lock, acq_ns, now_ns());
        }
        return NULL;
}

/* COLD: sshd-like -- one thread, takes the lock briefly, then does real
 * "work" (sleeps, standing in for I/O/other syscalls) before coming back.
 * No other thread contends for this lock at all, so it should read as
 * completely uncontended. */
static void *cold_thread(void *arg)
{
        (void)arg;
        /* Also deliberately unpinned, same reasoning as hot_thread. */
        while (!done) {
                park_if_paused();
                bool contended = atomic_load(&cold_waiters) > 0;

                atomic_fetch_add(&cold_waiters, 1);
                while (cmpxchg_ul(&cold_lock, 0, 1) != 0)
                        rmb();
                atomic_fetch_sub(&cold_waiters, 1);

                hotlock_update((const void *)&cold_lock, contended);
                freq_update((const void *)&cold_lock);
                pthread_mutex_lock(&stats_lock);
                cold_acquires++;
                if (contended) cold_contended_acquires++;
                pthread_mutex_unlock(&stats_lock);

                unsigned long long acq_ns = now_ns();
                for (int i = 0; i < 2000; i++)
                        asm volatile ("sfence" ::: "memory");

                cold_lock = 0;
                util_update_release((const void *)&cold_lock, acq_ns, now_ns());

                /* the part that makes this "cold": real idle time between
                 * acquisitions, like a daemon waiting on a connection. */
                usleep(50000); /* 50ms */
        }
        return NULL;
}

/* FREQ: hackbench-style -- only 2 threads (a single connection's worth of
 * fan-in, matching hackbench's point-to-point sockets, NOT a big pool
 * hammering one lock), a very short CS, and -- critically -- genuine
 * "other work" between touches (message copy, poll(), scheduling -- the
 * real gap hackbench has between successive uses of any *one* connection's
 * lock). Without that gap, even 2 threads with zero backoff just ping-pong
 * and permanently contend regardless of CS length -- confirmed empirically
 * below before this comment was written: 2 threads, no gap, still measured
 * 100% ground-truth contention. The gap is what actually produces hackbench's
 * real pattern: high attempt rate *in aggregate across many connections*,
 * near-zero contention on any single one. */
#define FREQ_WORK_ITERS  8000 /* tune: unrelated CPU work between touches */

static void *freq_thread(void *arg)
{
        (void)arg;
        while (!done) {
                park_if_paused();
                bool contended = atomic_load(&freq_waiters) > 0;

                atomic_fetch_add(&freq_waiters, 1);
                while (cmpxchg_ul(&freq_lock, 0, 1) != 0)
                        rmb();
                atomic_fetch_sub(&freq_waiters, 1);

                hotlock_update((const void *)&freq_lock, contended);
                freq_update((const void *)&freq_lock);
                pthread_mutex_lock(&stats_lock);
                freq_acquires++;
                if (contended) freq_contended_acquires++;
                pthread_mutex_unlock(&stats_lock);

                /* Much shorter than hot_lock's CS -- matches the measured
                 * hackbench fast-path hold time. */
                unsigned long long acq_ns = now_ns();
                for (int i = 0; i < 20; i++)
                        asm volatile ("sfence" ::: "memory");

                freq_lock = 0;
                util_update_release((const void *)&freq_lock, acq_ns, now_ns());

                /* The part hackbench actually has that a bare spin-loop
                 * doesn't: real work between touches of THIS connection's
                 * lock (read/write/poll on the other end, message handling).
                 */
                for (int i = 0; i < FREQ_WORK_ITERS; i++)
                        asm volatile ("sfence" ::: "memory");
        }
        return NULL;
}

/* BUSY: one thread, moderate CS, NO gap between release and re-acquire.
 * Lock is held ~100% of the time (rho ~= 1) but only ever by this one thread,
 * so there is never a waiter and preempting the holder cannot strand anyone.
 * The purpose is to expose Model C's structural false positive. */
static void *busy_thread(void *arg)
{
        (void)arg;
        while (!done) {
                park_if_paused();
                bool contended = atomic_load(&busy_waiters) > 0;

                atomic_fetch_add(&busy_waiters, 1);
                while (cmpxchg_ul(&busy_lock, 0, 1) != 0)
                        rmb();
                atomic_fetch_sub(&busy_waiters, 1);

                hotlock_update((const void *)&busy_lock, contended);
                freq_update((const void *)&busy_lock);
                pthread_mutex_lock(&stats_lock);
                busy_acquires++;
                if (contended) busy_contended_acquires++;
                pthread_mutex_unlock(&stats_lock);

                unsigned long long acq_ns = now_ns();
                for (int i = 0; i < 30000; i++)
                        asm volatile ("sfence" ::: "memory");

                busy_lock = 0;
                util_update_release((const void *)&busy_lock, acq_ns, now_ns());
                /* No gap: immediately loop back and re-acquire. */
        }
        return NULL;
}

/* TINY: several threads, tight loop, no gap, nanosecond CS -- genuinely
 * queues (like the original miscalibrated freq_lock), but each hold is too
 * short for a migration to be worth triggering. */
static void *tiny_thread(void *arg)
{
        (void)arg;
        while (!done) {
                park_if_paused();
                bool contended = atomic_load(&tiny_waiters) > 0;

                atomic_fetch_add(&tiny_waiters, 1);
                while (cmpxchg_ul(&tiny_lock, 0, 1) != 0)
                        rmb();
                atomic_fetch_sub(&tiny_waiters, 1);

                hotlock_update((const void *)&tiny_lock, contended);
                freq_update((const void *)&tiny_lock);
                pthread_mutex_lock(&stats_lock);
                tiny_acquires++;
                if (contended) tiny_contended_acquires++;
                pthread_mutex_unlock(&stats_lock);

                unsigned long long acq_ns = now_ns();
                for (int i = 0; i < 20; i++)
                        asm volatile ("sfence" ::: "memory");

                tiny_lock = 0;
                util_update_release((const void *)&tiny_lock, acq_ns, now_ns());
                /* No gap -- unlike freq_lock/freq_thread, deliberately. */
        }
        return NULL;
}

/* HOTPATH: important, heavily-used, genuinely-contended workload lock with a
 * SHORT critical section (target a few hundred ns -- below Model D's 1us floor
 * but well above tiny_lock's nanoseconds). No gap, several threads. */
#define HOTPATH_CS_ITERS 320 /* tuned to land meanCS in the few-hundred-ns range */

static void *hotpath_thread(void *arg)
{
        (void)arg;
        while (!done) {
                park_if_paused();
                bool contended = atomic_load(&hotpath_waiters) > 0;

                atomic_fetch_add(&hotpath_waiters, 1);
                while (cmpxchg_ul(&hotpath_lock, 0, 1) != 0)
                        rmb();
                atomic_fetch_sub(&hotpath_waiters, 1);

                hotlock_update((const void *)&hotpath_lock, contended);
                freq_update((const void *)&hotpath_lock);
                pthread_mutex_lock(&stats_lock);
                hotpath_acquires++;
                if (contended) hotpath_contended_acquires++;
                pthread_mutex_unlock(&stats_lock);

                unsigned long long acq_ns = now_ns();
                for (int i = 0; i < HOTPATH_CS_ITERS; i++)
                        asm volatile ("sfence" ::: "memory");

                hotpath_lock = 0;
                util_update_release((const void *)&hotpath_lock, acq_ns, now_ns());
                /* No gap: heavily used. */
        }
        return NULL;
}

int main(int argc, char **argv)
{
        int num_hot_threads = argc > 1 ? atoi(argv[1]) : 8;
        int duration_s = argc > 2 ? atoi(argv[2]) : 10;
        int num_freq_threads = argc > 3 ? atoi(argv[3]) : 16;
        int num_tiny_threads = argc > 4 ? atoi(argv[4]) : 8;
        int num_hotpath_threads = argc > 5 ? atoi(argv[5]) : 6;
        pthread_t hot_threads[64];
        pthread_t freq_threads[64];
        pthread_t tiny_threads[64];
        pthread_t hotpath_threads[64];
        pthread_t cold_tid;

        if (num_hot_threads > 64)
                num_hot_threads = 64;
        if (num_freq_threads > 64)
                num_freq_threads = 64;
        if (num_tiny_threads > 64)
                num_tiny_threads = 64;
        if (num_hotpath_threads > 64)
                num_hotpath_threads = 64;

        freq_table_init();

        printf("hot  lock: %d threads, tight spin, long CS, no idle time\n", num_hot_threads);
        printf("cold lock: 1 thread, 50ms idle between acquisitions (sshd-like)\n");
        printf("freq lock: %d threads, tight spin, very short CS, no idle time (hackbench-like)\n\n", num_freq_threads);
        printf("[debug] hot_lock=%p (bucket %u)  cold_lock=%p (bucket %u)  freq_lock=%p (bucket %u)\n\n",
               (void *)&hot_lock, hash_ptr((const void *)&hot_lock),
               (void *)&cold_lock, hash_ptr((const void *)&cold_lock),
               (void *)&freq_lock, hash_ptr((const void *)&freq_lock));
        printf("busy lock: 1 thread, moderate CS, NO gap (rho~1 but zero contention)\n");
        printf("tiny lock: %d threads, tight spin, nanosecond CS, NO gap (real contention, too short to matter)\n",
               num_tiny_threads);
        printf("hotpath  : %d threads, short CS (~hundreds of ns, below D's floor), NO gap (important+contended)\n",
               num_hotpath_threads);
        {
                unsigned h[6] = { hash_ptr((const void *)&hot_lock),
                                  hash_ptr((const void *)&cold_lock),
                                  hash_ptr((const void *)&freq_lock),
                                  hash_ptr((const void *)&busy_lock),
                                  hash_ptr((const void *)&tiny_lock),
                                  hash_ptr((const void *)&hotpath_lock) };
                for (int i = 0; i < 6; i++)
                        for (int j = i + 1; j < 6; j++)
                                if (h[i] == h[j]) {
                                        fprintf(stderr, "FATAL: two locks collided in the "
                                                "hotlock table -- results meaningless. Aborting.\n");
                                        return 1;
                                }
        }

        pthread_t busy_tid;
        for (int i = 0; i < num_hot_threads; i++)
                pthread_create(&hot_threads[i], NULL, hot_thread, NULL);
        for (int i = 0; i < num_freq_threads; i++)
                pthread_create(&freq_threads[i], NULL, freq_thread, NULL);
        for (int i = 0; i < num_tiny_threads; i++)
                pthread_create(&tiny_threads[i], NULL, tiny_thread, NULL);
        for (int i = 0; i < num_hotpath_threads; i++)
                pthread_create(&hotpath_threads[i], NULL, hotpath_thread, NULL);
        pthread_create(&cold_tid, NULL, cold_thread, NULL);
        pthread_create(&busy_tid, NULL, busy_thread, NULL);

        int stall_at = duration_s / 2; /* inject one simulated steal mid-run */
        for (int t = 0; t < duration_s; t++) {
                sleep(1);
                printf("t=%2ds  hot : A=%4d(%s) B=%7.1f(%s) C=%4d(%s)   "
                       "cold: A=%4d(%s) B=%6.1f(%s) C=%4d(%s)   "
                       "freq: A=%4d(%s) B=%7.1f(%s) C=%4d(%s)\n",
                       t + 1,
                       hotlock_read((const void *)&hot_lock), hotlock_is_hot((const void *)&hot_lock) ? "H" : "c",
                       freq_read((const void *)&hot_lock), freq_is_hot((const void *)&hot_lock) ? "H" : "c",
                       util_read((const void *)&hot_lock), util_is_hot((const void *)&hot_lock) ? "H" : "c",
                       hotlock_read((const void *)&cold_lock), hotlock_is_hot((const void *)&cold_lock) ? "H" : "c",
                       freq_read((const void *)&cold_lock), freq_is_hot((const void *)&cold_lock) ? "H" : "c",
                       util_read((const void *)&cold_lock), util_is_hot((const void *)&cold_lock) ? "H" : "c",
                       hotlock_read((const void *)&freq_lock), hotlock_is_hot((const void *)&freq_lock) ? "H" : "c",
                       freq_read((const void *)&freq_lock), freq_is_hot((const void *)&freq_lock) ? "H" : "c",
                       util_read((const void *)&freq_lock), util_is_hot((const void *)&freq_lock) ? "H" : "c");
                printf("       busy: A=%4d(%s) D=%s E=%s   tiny: A=%4d(%s) D=%s E=%s   "
                       "hotpath: A=%4d(%s) C=%4d D=%s E=%s\n",
                       hotlock_read((const void *)&busy_lock), hotlock_is_hot((const void *)&busy_lock) ? "H" : "c",
                       modeld_is_hot((const void *)&busy_lock, atomic_load(&busy_waiters)) ? "HOT" : "cold",
                       modele_is_hot((const void *)&busy_lock, atomic_load(&busy_waiters)) ? "HOT" : "cold",
                       hotlock_read((const void *)&tiny_lock), hotlock_is_hot((const void *)&tiny_lock) ? "H" : "c",
                       modeld_is_hot((const void *)&tiny_lock, atomic_load(&tiny_waiters)) ? "HOT" : "cold",
                       modele_is_hot((const void *)&tiny_lock, atomic_load(&tiny_waiters)) ? "HOT" : "cold",
                       hotlock_read((const void *)&hotpath_lock), hotlock_is_hot((const void *)&hotpath_lock) ? "H" : "c",
                       util_read((const void *)&hotpath_lock),
                       modeld_is_hot((const void *)&hotpath_lock, atomic_load(&hotpath_waiters)) ? "HOT" : "cold",
                       modele_is_hot((const void *)&hotpath_lock, atomic_load(&hotpath_waiters)) ? "HOT" : "cold");

                /* Simulated host steal event: freeze all workers for 100ms
                 * (no acquisitions happen) and snapshot every model for
                 * hot_lock immediately before vs. immediately after the
                 * window. This is the exact scenario that killed Model B. */
                if (t + 1 == stall_at) {
                        int a_pre = hotlock_read((const void *)&hot_lock);
                        double b_pre = freq_read((const void *)&hot_lock);
                        int c_pre = util_read((const void *)&hot_lock);

                        atomic_store(&paused, 1);
                        usleep(100000); /* 100ms of "stolen" wall-clock time */
                        int a_post = hotlock_read((const void *)&hot_lock);
                        double b_post = freq_read((const void *)&hot_lock);
                        int c_post = util_read((const void *)&hot_lock);
                        atomic_store(&paused, 0);

                        printf("  >>> SIMULATED 100ms STEAL on hot_lock: "
                               "Model A %d->%d  Model B %.1f->%.1f  Model C %d->%d\n",
                               a_pre, a_post, b_pre, b_post, c_pre, c_post);
                        printf("      (event-driven A & C freeze through the steal; "
                               "wall-clock-decayed B collapses)\n");
                }
        }

        done = 1;
        for (int i = 0; i < num_hot_threads; i++)
                pthread_join(hot_threads[i], NULL);
        for (int i = 0; i < num_freq_threads; i++)
                pthread_join(freq_threads[i], NULL);
        for (int i = 0; i < num_tiny_threads; i++)
                pthread_join(tiny_threads[i], NULL);
        for (int i = 0; i < num_hotpath_threads; i++)
                pthread_join(hotpath_threads[i], NULL);
        pthread_join(cold_tid, NULL);
        pthread_join(busy_tid, NULL);

        double dur = (double)duration_s;
        printf("\n=== Final ===\n");
        printf("%-9s %11s %8s %10s   %-6s %-6s %-18s %-6s %-6s %-12s\n",
               "lock", "acq/s", "gt%", "meanCS", "ModelA", "ModelB", "ModelC (rho)", "ModelD", "ModelE", "ModelF(score)");
        printf("hot_lock : %11.0f %6.2f%% %8lldns   %-6s %-6s %4d/1024=%.3f %-6s %-6s %-4s(%.2f)\n",
               hot_acquires / dur,
               100.0 * hot_contended_acquires / (hot_acquires ? hot_acquires : 1),
               util_mean_cs((const void *)&hot_lock),
               hotlock_is_hot((const void *)&hot_lock) ? "HOT" : "COLD",
               freq_is_hot((const void *)&hot_lock) ? "HOT" : "COLD",
               util_read((const void *)&hot_lock), util_read((const void *)&hot_lock) / 1024.0,
               modeld_is_hot((const void *)&hot_lock, atomic_load(&hot_waiters)) ? "HOT" : "COLD",
               modele_is_hot((const void *)&hot_lock, atomic_load(&hot_waiters)) ? "HOT" : "COLD",
               modelf_is_hot((const void *)&hot_lock, atomic_load(&hot_waiters)) ? "HOT" : "COLD",
               modelf_score((const void *)&hot_lock, atomic_load(&hot_waiters)));
        printf("cold_lock: %11.0f %6.2f%% %8lldns   %-6s %-6s %4d/1024=%.3f %-6s %-6s %-4s(%.2f)\n",
               cold_acquires / dur,
               100.0 * cold_contended_acquires / (cold_acquires ? cold_acquires : 1),
               util_mean_cs((const void *)&cold_lock),
               hotlock_is_hot((const void *)&cold_lock) ? "HOT" : "COLD",
               freq_is_hot((const void *)&cold_lock) ? "HOT" : "COLD",
               util_read((const void *)&cold_lock), util_read((const void *)&cold_lock) / 1024.0,
               modeld_is_hot((const void *)&cold_lock, atomic_load(&cold_waiters)) ? "HOT" : "COLD",
               modele_is_hot((const void *)&cold_lock, atomic_load(&cold_waiters)) ? "HOT" : "COLD",
               modelf_is_hot((const void *)&cold_lock, atomic_load(&cold_waiters)) ? "HOT" : "COLD",
               modelf_score((const void *)&cold_lock, atomic_load(&cold_waiters)));
        printf("freq_lock: %11.0f %6.2f%% %8lldns   %-6s %-6s %4d/1024=%.3f %-6s %-6s %-4s(%.2f)  <-- hackbench: high freq, ~0 contention -> A/D/E/F all COLD\n",
               freq_acquires / dur,
               100.0 * freq_contended_acquires / (freq_acquires ? freq_acquires : 1),
               util_mean_cs((const void *)&freq_lock),
               hotlock_is_hot((const void *)&freq_lock) ? "HOT" : "COLD",
               freq_is_hot((const void *)&freq_lock) ? "HOT" : "COLD",
               util_read((const void *)&freq_lock), util_read((const void *)&freq_lock) / 1024.0,
               modeld_is_hot((const void *)&freq_lock, atomic_load(&freq_waiters)) ? "HOT" : "COLD",
               modele_is_hot((const void *)&freq_lock, atomic_load(&freq_waiters)) ? "HOT" : "COLD",
               modelf_is_hot((const void *)&freq_lock, atomic_load(&freq_waiters)) ? "HOT" : "COLD",
               modelf_score((const void *)&freq_lock, atomic_load(&freq_waiters)));
        printf("busy_lock: %11.0f %6.2f%% %8lldns   %-6s %-6s %4d/1024=%.3f %-6s %-6s %-4s(%.2f)  <-- rho FALSE POSITIVE; D/E/F all fix it (no contention)\n",
               busy_acquires / dur,
               100.0 * busy_contended_acquires / (busy_acquires ? busy_acquires : 1),
               util_mean_cs((const void *)&busy_lock),
               hotlock_is_hot((const void *)&busy_lock) ? "HOT" : "COLD",
               freq_is_hot((const void *)&busy_lock) ? "HOT" : "COLD",
               util_read((const void *)&busy_lock), util_read((const void *)&busy_lock) / 1024.0,
               modeld_is_hot((const void *)&busy_lock, atomic_load(&busy_waiters)) ? "HOT" : "COLD",
               modele_is_hot((const void *)&busy_lock, atomic_load(&busy_waiters)) ? "HOT" : "COLD",
               modelf_is_hot((const void *)&busy_lock, atomic_load(&busy_waiters)) ? "HOT" : "COLD",
               modelf_score((const void *)&busy_lock, atomic_load(&busy_waiters)));
        printf("tiny_lock: %11.0f %6.2f%% %8lldns   %-6s %-6s %4d/1024=%.3f %-6s %-6s %-4s(%.2f)  <-- queues but nanosecond CS; D floor/E low-rho/F soft-knee all COLD\n",
               tiny_acquires / dur,
               100.0 * tiny_contended_acquires / (tiny_acquires ? tiny_acquires : 1),
               util_mean_cs((const void *)&tiny_lock),
               hotlock_is_hot((const void *)&tiny_lock) ? "HOT" : "COLD",
               freq_is_hot((const void *)&tiny_lock) ? "HOT" : "COLD",
               util_read((const void *)&tiny_lock), util_read((const void *)&tiny_lock) / 1024.0,
               modeld_is_hot((const void *)&tiny_lock, atomic_load(&tiny_waiters)) ? "HOT" : "COLD",
               modele_is_hot((const void *)&tiny_lock, atomic_load(&tiny_waiters)) ? "HOT" : "COLD",
               modelf_is_hot((const void *)&tiny_lock, atomic_load(&tiny_waiters)) ? "HOT" : "COLD",
               modelf_score((const void *)&tiny_lock, atomic_load(&tiny_waiters)));
        printf("hotpath  : %11.0f %6.2f%% %8lldns   %-6s %-6s %4d/1024=%.3f %-6s %-6s %-4s(%.2f)  <-- SHORT CS + REAL contention; D VETOES, E lumps w/tiny, F rescues\n",
               hotpath_acquires / dur,
               100.0 * hotpath_contended_acquires / (hotpath_acquires ? hotpath_acquires : 1),
               util_mean_cs((const void *)&hotpath_lock),
               hotlock_is_hot((const void *)&hotpath_lock) ? "HOT" : "COLD",
               freq_is_hot((const void *)&hotpath_lock) ? "HOT" : "COLD",
               util_read((const void *)&hotpath_lock), util_read((const void *)&hotpath_lock) / 1024.0,
               modeld_is_hot((const void *)&hotpath_lock, atomic_load(&hotpath_waiters)) ? "HOT" : "COLD",
               modele_is_hot((const void *)&hotpath_lock, atomic_load(&hotpath_waiters)) ? "HOT" : "COLD",
               modelf_is_hot((const void *)&hotpath_lock, atomic_load(&hotpath_waiters)) ? "HOT" : "COLD",
               modelf_score((const void *)&hotpath_lock, atomic_load(&hotpath_waiters)));
        printf("\nModel D (gated):   (live_waiters>0 OR Model-A hot) AND meanCS>=%dns  [HARD CS floor]\n"
               "Model E (con-util): (live_waiters>0 OR Model-A hot) AND rho>0.5\n"
               "Model F (PROPOSED): P(contended) * (1 - exp(-meanCS/%.0fns)) > %.2f  [SOFT CS knee]\n"
               "  Contention = the CS-length-INDEPENDENT 'is this a real workload lock vs a\n"
               "  daemon' signal the owner wants (cold/busy/freq have no waiters; hot/tiny/\n"
               "  hotpath do). F keeps that as the gate but replaces D's hard floor with a\n"
               "  soft knee: hotpath (short CS, real contention) is rescued to HOT; tiny\n"
               "  (nanosecond CS) stays COLD -- graded by ~13x, no categorical veto.\n"
               "  NOTE: rho (E) does NOT rescue hotpath -- it lumps it with tiny (both low\n"
               "  rho), so utilization is the WRONG axis for the owner's ask; contention is.\n",
               MEANCS_FLOOR_NS, MODELF_TAU_NS, MODELF_THRESH);

        return 0;
}
