/*
 * hotthreads_latch_test.c
 *
 * CLOSED-LOOP validation of a proposed fix for the "Hot Threads" AND-gate
 * self-undermining feedback regression found on the live kernel.
 *
 * ---------------------------------------------------------------------------
 * THE REGRESSION (measured on the real kernel, not synthetically):
 *   Hot Threads classifies a thread HOT (worth an IVH pre-lock migration) iff
 *     wait_decay  > WAIT_THRESH      (contends real locks; event-driven EWMA)
 *   AND
 *     preempt_decay > PREEMPT_THRESH (recently CAUGHT mid-CS by real host steal;
 *                                     event-driven EWMA, fed 0/SCALE per release)
 *   With the gate OFF (protect every eligible thread) NHextend3 measured 2.1%
 *   host-preempted-CS. With the gate ON the SAME workload measured 15-17% -- a
 *   7-8x REGRESSION -- because almost nothing passed the gate (8 passes vs
 *   13,729 cold-skips across two runs of an obviously-contended workload).
 *
 *   Root cause is a FEEDBACK LOOP, not a signal-accuracy bug: preempt_decay
 *   needs RECENT evidence of being caught by steal to stay above threshold, but
 *   IVH's protection -- when it works -- PREVENTS exactly that evidence. So
 *   preempt_decay decays, the gate closes, protection lapses, the thread gets
 *   caught for real (now unprotected), preempt_decay spikes, the gate reopens,
 *   protection resumes, evidence stops, decay closes it again. The classifier
 *   requires proof of recent harm to justify preventing future harm, and
 *   successful prevention destroys its own justification.
 *
 * ---------------------------------------------------------------------------
 * WHY A CLOSED-LOOP SIM (and why simulated steal, not /proc/vcap_info):
 *   hotthreads_test.c / hotthreads_waitdecay_test.c validated SIGNAL ACCURACY
 *   in isolation (do the counters read high/low for known patterns). That is
 *   NOT the bug. The bug is causal: the classifier's OWN verdict controls
 *   whether protection is applied, and protection's success changes the future
 *   INPUT to the classifier. A test that re-measures accuracy in isolation
 *   cannot see it. We must model the loop:
 *       verdict(t) -> protect?(t) -> steal-catch prob -> preempt_decay feed
 *                  ^                                                 |
 *                  +-------------------------------------------------+
 *   The protection effect (IVH migrating off a stolen vCPU) CANNOT happen in a
 *   userspace prototype -- it must be simulated. If the steal DRAW came from
 *   real /proc/vcap_info while the protection DECISION was simulated, the loop
 *   would be incoherent: real host steal does not respond to our simulated
 *   protect/no-protect choice, so the feedback edge (protection lowering future
 *   catch probability) would be missing -- the exact thing under test. So the
 *   whole steal signal is a simulated per-thread Bernoulli draw whose success
 *   probability DOES respond to the verdict. wait_decay, by contrast, is driven
 *   by GENUINE shared-lock contention (real waiters, real spins), exactly as in
 *   the prior prototypes -- only the steal/protection half is modelled.
 *
 *   Steal-catch probability is modelled per critical section:
 *       p_eff = protected ? p_base * PROTECT_FACTOR : p_base
 *   p_base is the unprotected chance the host steals this vCPU DURING this hold;
 *   PROTECT_FACTOR (<1) is IVH's residual exposure after a successful pre-lock
 *   migration away from a stolen vCPU. Hot (long-hold) threads get a real
 *   p_base; cold (brief-hold daemon) threads get a tiny p_base because a short
 *   hold is unlikely to overlap a steal window -- catch prob scales with hold
 *   time. This reproduces (a) ~= kernel's 2.1% and lets (b) reproduce the
 *   regression before (c) is trusted.
 *
 * ---------------------------------------------------------------------------
 * EWMA machinery is the project standard: fixed-point SCALE=1024, k=3, and it
 * is EVENT-DRIVEN ONLY -- every decay step happens on a real acquire/release
 * EVENT, never at read time and never as a function of wall-clock (the Model B
 * trap documented to death in hotlock_userland_test.c's history). Nothing here
 * decays through an idle/steal window; it freezes, by design.
 *
 * ---------------------------------------------------------------------------
 * THREE CONDITIONS, same workload, same duration:
 *   (a) NO GATE      -- protect every eligible (hot-population) thread always.
 *                       Known-good baseline; expect a LOW real-steal-catch rate.
 *   (b) EWMA GATE    -- wait_decay>WAIT AND preempt_decay>PREEMPT, both symmetric
 *                       decaying EWMAs (the shipped design). Must REPRODUCE the
 *                       regression: a materially HIGHER catch rate than (a). If
 *                       it does not, the loop is not faithful -- fix before (c).
 *   (c) STICKY LATCH -- wait_decay unchanged; preempt_decay's half of the gate
 *                       becomes a PERMANENT one-way latch: once preempt_decay
 *                       ever crosses PREEMPT_THRESH, that half is satisfied for
 *                       the thread's whole (simulated) life -- no decay, no
 *                       cooldown, "has this EVER happened", not "recently".
 *                       Expect the catch rate back down near (a), cold excluded.
 *
 * DELIBERATELY OUT OF SCOPE (per project owner): the permanent latch will keep
 * a once-hot thread eligible forever even if its behaviour later changes. The
 * real long-term fix is a cooldown / very-long-timescale decay. We test the
 * PERMANENT version first as the simplest possible probe of whether Hot Threads
 * is a promising direction at all. Cooldown is future work, NOT built here.
 *
 * Build: gcc -O2 -o hotthreads_latch_test hotthreads_latch_test.c -lpthread -lm
 * Run:   ./hotthreads_latch_test [hot_threads] [measure_s] [p_base_hot] [protect_factor]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <math.h>
#include <stdatomic.h>

#define rmb() asm volatile ("lfence" ::: "memory")

/* ---- project-standard fixed-point EWMA ---- */
#define SCALE           1024
#define EWMA_K          3
#define WAIT_THRESH     512      /* 0.5 -- established contention threshold */
static int preempt_thresh = 40;  /* ~0.039 of SCALE -- as in hotthreads_test.c */

/* ---- gate modes ---- */
enum gate_mode { MODE_NOGATE = 0, MODE_EWMA = 1, MODE_LATCH = 2 };

/* ---- simulated steal / protection loop parameters (tunable via argv) ---- */
static double p_base_hot   = 0.25;   /* unprotected chance of catch per hot hold */
static double p_base_cold  = 0.004;  /* brief daemon hold -> tiny catch window   */
static double protect_factor = 0.08; /* residual exposure after IVH migration    */
/* -> p_protected = 0.25*0.08 = 0.02, matching the kernel's gate-OFF ~2.1%. */

/* ---- per-thread state ---- */
struct tstate {
        int  id;
        bool is_cold;
        double p_base;

        /* classifier state (single-writer = owning thread; atomic for monitor) */
        atomic_int wait_decay;      /* EWMA, real contention driven */
        atomic_int preempt_decay;   /* EWMA, simulated steal driven */
        atomic_int preempt_latched; /* one-way latch for MODE_LATCH (0/1) */

        unsigned int rng;           /* per-thread xorshift32, no shared RNG lock */

        /* measurement-window tallies (only counted while 'measuring') */
        _Atomic unsigned long long acquires;
        _Atomic unsigned long long contended;
        _Atomic unsigned long long steal_holds;     /* REAL catches (ground truth) */
        _Atomic unsigned long long protected_holds; /* verdict said hot -> protected */
        _Atomic unsigned long long ever_latched;     /* set once if it ever latched */
};

static enum gate_mode g_mode;
static volatile int g_done = 0;
static volatile int g_measuring = 0;

/* ---- EWMA feeds (event-driven, write-time only) ---- */
static void wait_decay_update(struct tstate *t, bool contended)
{
        int old = atomic_load(&t->wait_decay);
        int sample = contended ? SCALE : 0;
        /* kernel-faithful: only SCALE is fed on the contended slowpath. A 0-feed
         * on uncontended acquisitions is the SEPARATE wait_decay fix validated in
         * hotthreads_waitdecay_test.c; it is orthogonal to the preempt-latch
         * question under test here. For the HOT population contention is ~100%
         * so wait_decay saturates high either way; for the COLD population the
         * kernel-current (no-0-feed) path is the conservative choice -- if cold
         * stays correctly excluded even WITHOUT the wait_decay 0-feed helping it
         * cool, the latch fix is not leaning on that orthogonal fix. */
        if (contended)
                atomic_store(&t->wait_decay, old + ((sample - old) >> EWMA_K));
}

static void preempt_decay_update(struct tstate *t, bool stolen)
{
        int old = atomic_load(&t->preempt_decay);
        int sample = stolen ? SCALE : 0;
        int nw = old + ((sample - old) >> EWMA_K);
        atomic_store(&t->preempt_decay, nw);
        /* latch the instant the EWMA ever crosses the existing threshold */
        if (nw > preempt_thresh && !atomic_load(&t->preempt_latched)) {
                atomic_store(&t->preempt_latched, 1);
                if (g_measuring) atomic_store(&t->ever_latched, 1);
        }
}

/* verdict: is this thread HOT (-> protect this hold) under the active gate? */
static bool verdict_hot(struct tstate *t)
{
        switch (g_mode) {
        case MODE_NOGATE:
                /* protect every eligible thread unconditionally; the hot
                 * population is "eligible" (PF_IVH_ELIGIBLE analogue), cold is
                 * not -- cold is never made eligible in the kernel either. */
                return !t->is_cold;
        case MODE_EWMA:
                return atomic_load(&t->wait_decay) > WAIT_THRESH &&
                       atomic_load(&t->preempt_decay) > preempt_thresh;
        case MODE_LATCH:
                return atomic_load(&t->wait_decay) > WAIT_THRESH &&
                       atomic_load(&t->preempt_latched);
        }
        return false;
}

/* ---- xorshift32 per-thread RNG ---- */
static inline unsigned int xs32(unsigned int *s)
{
        unsigned int x = *s;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        return (*s = x);
}
static inline double rnd01(unsigned int *s)
{
        return (double)xs32(s) / 4294967296.0;
}

/* ---- two independent lock contexts (hot pop / cold pop don't cross-contend) ---- */
struct lockctx {
        volatile unsigned long lock __attribute__((aligned(4096)));
        atomic_int waiters;
};
static struct lockctx hot_ctx, cold_ctx;

static unsigned long cmpxchg_ul(volatile unsigned long *p, unsigned long o, unsigned long n)
{
        unsigned long prev;
        asm volatile("lock; cmpxchg %2,%1" : "=a"(prev), "+m"(*p) : "r"(n), "0"(o) : "memory");
        return prev;
}

/* One acquire / hold / release, closing the feedback loop:
 *   1. real contention -> wait_decay
 *   2. verdict BEFORE the hold decides protection for THIS hold
 *   3. simulated steal draw against protection-adjusted probability
 *   4. steal outcome -> preempt_decay (and maybe latch)
 */
static void locked_op(struct tstate *t, struct lockctx *c, int cs_iters)
{
        bool contended = atomic_load(&c->waiters) > 0;
        atomic_fetch_add(&c->waiters, 1);
        while (cmpxchg_ul(&c->lock, 0, 1) != 0)
                rmb();
        atomic_fetch_sub(&c->waiters, 1);

        wait_decay_update(t, contended);

        /* verdict uses current decay/latch state (reflects PRIOR holds) */
        bool protect = verdict_hot(t);

        /* simulated critical section */
        for (int i = 0; i < cs_iters; i++)
                asm volatile ("sfence" ::: "memory");
        c->lock = 0;

        /* closed-loop steal draw: protection lowers the catch probability */
        double p_eff = protect ? t->p_base * protect_factor : t->p_base;
        bool stolen = rnd01(&t->rng) < p_eff;

        preempt_decay_update(t, stolen);

        if (g_measuring) {
                atomic_fetch_add(&t->acquires, 1);
                if (contended) atomic_fetch_add(&t->contended, 1);
                if (stolen)    atomic_fetch_add(&t->steal_holds, 1);
                if (protect)   atomic_fetch_add(&t->protected_holds, 1);
        }
}

/* HOT: sustained-contention worker (NHextend-style, long hold, no idle gap). */
static int hot_cs_iters = 20000;
static void *hot_thread(void *arg)
{
        struct tstate *t = arg;
        while (!g_done)
                locked_op(t, &hot_ctx, hot_cs_iters);
        return NULL;
}

/* COLD: daemon-style worker -- brief hold, real idle gap, rarely contends. */
static void *cold_thread(void *arg)
{
        struct tstate *t = arg;
        while (!g_done) {
                locked_op(t, &cold_ctx, 400);
                usleep(50000);
        }
        return NULL;
}

/* ---- one full condition run ---- */
struct cond_result {
        double hot_catch_pct, hot_wait, hot_preempt, hot_protect_pct;
        int    hot_latched, hot_n;
        double cold_catch_pct, cold_wait;
        int    cold_latched, cold_hot_verdict, cold_n;
};

static const char *mode_name(enum gate_mode m)
{
        return m == MODE_NOGATE ? "(a) NO GATE (always protect eligible)" :
               m == MODE_EWMA   ? "(b) EWMA AND-gate (shipped design)"     :
                                  "(c) STICKY-LATCH AND-gate (fix)";
}

static struct cond_result run_condition(enum gate_mode mode, int hot_n, int cold_n,
                                         int warmup_s, int measure_s)
{
        struct tstate hot[64], cold[16];
        pthread_t htid[64], ctid[16];
        if (hot_n > 64) hot_n = 64;
        if (cold_n > 16) cold_n = 16;

        memset(hot, 0, sizeof(hot));
        memset(cold, 0, sizeof(cold));
        for (int i = 0; i < hot_n; i++) {
                hot[i].id = i; hot[i].is_cold = false; hot[i].p_base = p_base_hot;
                hot[i].rng = 0x9e3779b9u ^ (0xC0FFEEu * (i + 1));
        }
        for (int i = 0; i < cold_n; i++) {
                cold[i].id = 100 + i; cold[i].is_cold = true; cold[i].p_base = p_base_cold;
                cold[i].rng = 0x1234567u ^ (0xB16B00Bu * (i + 3));
        }
        hot_ctx.lock = 0; atomic_store(&hot_ctx.waiters, 0);
        cold_ctx.lock = 0; atomic_store(&cold_ctx.waiters, 0);

        g_mode = mode;
        g_done = 0;
        g_measuring = 0;   /* warmup: run the loop, do NOT count */

        printf("=== CONDITION %s ===\n", mode_name(mode));
        printf("    hot=%d cold=%d  p_base_hot=%.3f p_protected=%.3f p_base_cold=%.4f"
               "  warmup=%ds measure=%ds\n",
               hot_n, cold_n, p_base_hot, p_base_hot * protect_factor, p_base_cold,
               warmup_s, measure_s);

        for (int i = 0; i < hot_n; i++) pthread_create(&htid[i], NULL, hot_thread, &hot[i]);
        for (int i = 0; i < cold_n; i++) pthread_create(&ctid[i], NULL, cold_thread, &cold[i]);

        /* warmup: let the feedback loop reach its operating regime */
        for (int s = 0; s < warmup_s; s++) sleep(1);

        g_measuring = 1;   /* begin counting */
        for (int s = 0; s < measure_s; s++) {
                sleep(1);
                long wd = 0, pd = 0; int hot_now = 0, latched = 0;
                unsigned long long acq = 0, steal = 0;
                for (int i = 0; i < hot_n; i++) {
                        wd += atomic_load(&hot[i].wait_decay);
                        pd += atomic_load(&hot[i].preempt_decay);
                        if (verdict_hot(&hot[i])) hot_now++;
                        if (atomic_load(&hot[i].preempt_latched)) latched++;
                        acq += atomic_load(&hot[i].acquires);
                        steal += atomic_load(&hot[i].steal_holds);
                }
                printf("  t=%2ds  wait=%.3f preempt=%.3f  hot_now=%d/%d latched=%d/%d"
                       "  cum_catch=%.2f%%\n",
                       s + 1, (double)wd / hot_n / SCALE, (double)pd / hot_n / SCALE,
                       hot_now, hot_n, latched, hot_n,
                       acq ? 100.0 * steal / acq : 0.0);
        }

        g_done = 1;
        for (int i = 0; i < hot_n; i++) pthread_join(htid[i], NULL);
        for (int i = 0; i < cold_n; i++) pthread_join(ctid[i], NULL);

        struct cond_result r; memset(&r, 0, sizeof(r));
        r.hot_n = hot_n; r.cold_n = cold_n;

        unsigned long long acq = 0, cont = 0, steal = 0, prot = 0;
        long wd = 0, pd = 0;
        for (int i = 0; i < hot_n; i++) {
                acq  += atomic_load(&hot[i].acquires);
                cont += atomic_load(&hot[i].contended);
                steal+= atomic_load(&hot[i].steal_holds);
                prot += atomic_load(&hot[i].protected_holds);
                wd   += atomic_load(&hot[i].wait_decay);
                pd   += atomic_load(&hot[i].preempt_decay);
                if (atomic_load(&hot[i].preempt_latched)) r.hot_latched++;
        }
        r.hot_catch_pct   = acq ? 100.0 * steal / acq : 0.0;
        r.hot_protect_pct = acq ? 100.0 * prot  / acq : 0.0;
        r.hot_wait    = (double)wd / hot_n / SCALE;
        r.hot_preempt = (double)pd / hot_n / SCALE;

        unsigned long long cacq = 0, csteal = 0; long cwd = 0;
        for (int i = 0; i < cold_n; i++) {
                cacq  += atomic_load(&cold[i].acquires);
                csteal+= atomic_load(&cold[i].steal_holds);
                cwd   += atomic_load(&cold[i].wait_decay);
                if (atomic_load(&cold[i].preempt_latched)) r.cold_latched++;
                if (verdict_hot(&cold[i])) r.cold_hot_verdict++;
        }
        r.cold_catch_pct = cacq ? 100.0 * csteal / cacq : 0.0;
        r.cold_wait = (double)cwd / cold_n / SCALE;

        printf("  -> HOT : catch=%.2f%%  protected_holds=%.1f%%  wait=%.3f preempt=%.3f"
               "  latched=%d/%d  (acq=%llu)\n",
               r.hot_catch_pct, r.hot_protect_pct, r.hot_wait, r.hot_preempt,
               r.hot_latched, hot_n, acq);
        printf("  -> COLD: catch=%.2f%%  wait=%.3f  latched=%d/%d  hot_verdict=%d/%d"
               "  (acq=%llu)\n\n",
               r.cold_catch_pct, r.cold_wait, r.cold_latched, cold_n,
               r.cold_hot_verdict, cold_n, cacq);
        return r;
}

int main(int argc, char **argv)
{
        int hot_n     = argc > 1 ? atoi(argv[1]) : 16;
        int measure_s = argc > 2 ? atoi(argv[2]) : 10;
        if (argc > 3) p_base_hot = atof(argv[3]);
        if (argc > 4) protect_factor = atof(argv[4]);
        int cold_n = 4, warmup_s = 3;

        printf("Hot Threads sticky-latch closed-loop validation\n");
        printf("SCALE=%d k=%d WAIT_THRESH=%d PREEMPT_THRESH=%d (%.3f)\n",
               SCALE, EWMA_K, WAIT_THRESH, preempt_thresh, (double)preempt_thresh / SCALE);
        printf("simulated steal: p_base_hot=%.3f protect_factor=%.3f -> p_protected=%.3f;"
               " p_base_cold=%.4f\n\n",
               p_base_hot, protect_factor, p_base_hot * protect_factor, p_base_cold);

        struct cond_result a = run_condition(MODE_NOGATE, hot_n, cold_n, warmup_s, measure_s);
        struct cond_result b = run_condition(MODE_EWMA,   hot_n, cold_n, warmup_s, measure_s);
        struct cond_result c = run_condition(MODE_LATCH,  hot_n, cold_n, warmup_s, measure_s);

        printf("=============================== SUMMARY ===============================\n");
        printf("%-40s %12s %12s %10s\n", "condition", "HOT catch%", "HOT prot%", "COLD catch%");
        printf("%-40s %11.2f%% %11.2f%% %9.2f%%\n", mode_name(MODE_NOGATE),
               a.hot_catch_pct, a.hot_protect_pct, a.cold_catch_pct);
        printf("%-40s %11.2f%% %11.2f%% %9.2f%%\n", mode_name(MODE_EWMA),
               b.hot_catch_pct, b.hot_protect_pct, b.cold_catch_pct);
        printf("%-40s %11.2f%% %11.2f%% %9.2f%%\n", mode_name(MODE_LATCH),
               c.hot_catch_pct, c.hot_protect_pct, c.cold_catch_pct);

        printf("\nCREDIBILITY CHECK (must hold or (c) is meaningless):\n");
        double reg = a.hot_catch_pct > 0 ? b.hot_catch_pct / a.hot_catch_pct : 0;
        printf("  (b) EWMA reproduces the regression vs (a): %s"
               "  [%.2f%% vs %.2f%%, %.1fx]\n",
               b.hot_catch_pct > 1.5 * a.hot_catch_pct ? "YES" : "NO",
               b.hot_catch_pct, a.hot_catch_pct, reg);

        printf("\nFIX VERDICT:\n");
        bool fixed = c.hot_catch_pct <= 1.5 * a.hot_catch_pct;
        printf("  (c) latch brings HOT catch back near (a) baseline: %s"
               "  [%.2f%% vs baseline %.2f%%]\n",
               fixed ? "YES" : "NO", c.hot_catch_pct, a.hot_catch_pct);
        bool cold_ok = c.cold_hot_verdict == 0 && b.cold_hot_verdict == 0;
        printf("  cold population stays correctly excluded (never HOT) in (b) and (c): %s"
               "  [(b)=%d (c)=%d of %d]\n",
               cold_ok ? "YES" : "NO", b.cold_hot_verdict, c.cold_hot_verdict, c.cold_n);
        printf("\nNOTE: cooldown / long-timescale decay of the latch is deliberately OUT OF\n"
               "SCOPE for this pass -- the permanent latch is tested first as the simplest\n"
               "probe. If (c) holds, designing the cooldown version is the clear next step.\n");
        return 0;
}
