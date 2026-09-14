/*
 * hotthreads_sweep_test.c
 *
 * TUNING SWEEP: can the EWMA AND-gate's own knobs (decay rate k, preempt
 * threshold) be retuned to fix the self-undermining feedback regression
 * documented in hotthreads_latch_test.c, WITHOUT resorting to a permanent
 * latch?
 *
 * Background (see hotthreads_latch_test.c for the full writeup): Hot Threads
 * gates IVH protection on (wait_decay > WAIT_THRESH) AND (preempt_decay >
 * PREEMPT_THRESH). preempt_decay is a k=3 EWMA fed SCALE on a real steal
 * catch, 0 otherwise. Protection SUPPRESSES catches (p_eff = p_base *
 * protect_factor while protected), so the moment protection engages, the
 * evidence that justified it starts decaying away -- closing the gate again
 * shortly after it opened. condition (b) in hotthreads_latch_test.c measured
 * this: ~8 passes vs ~13,729 cold-skips, and a 7-8x regression in real
 * catch rate vs an unconditional-protect baseline.
 *
 * This program asks: is that a consequence of the SPECIFIC (k=3, thresh=40)
 * operating point, fixable by retuning -- or is it structural, true for any
 * (k, thresh) pair a live recency-based EWMA gate could use?
 *
 * THE MATH (derived by hand, verified empirically below):
 *   EWMA update on a 0-feed (post-catch, now protected, catches suppressed):
 *       new = old + (0 - old) >> k  =~ old * (1 - 2^-k)
 *   so from a post-catch spike level S, decay to level L takes
 *       n*(S, L, k) = ln(L / S) / ln(1 - 2^-k)     [holds/events, not seconds]
 *   The post-catch spike from a cold start (old ~= 0) after ONE catch is
 *       S1 = SCALE * 2^-k
 *   which is already the single biggest lever: for k=3, S1 = 1024/8 = 128,
 *   so ONE catch is enough to clear a threshold anywhere below 128 -- the
 *   gate opens almost immediately after the first hit. The protected-window
 *   length (holds spent protected before decay re-closes the gate) is then
 *       n*(S1, THRESH, k) = ln(THRESH / (SCALE*2^-k)) / ln(1 - 2^-k)
 *
 *   THE STRUCTURAL LIMIT: under protection the decay does not fall to 0, it
 *   falls toward the PROTECTED equilibrium E_prot = SCALE * p_protected
 *   (p_protected = p_base_hot * protect_factor). If THRESH < E_prot, decay
 *   can never fall back below threshold once protected -- the "recency" gate
 *   degenerates into a de-facto permanent latch (protection never lapses).
 *   If THRESH > E_prot (any real "recency" semantics), decay MUST eventually
 *   fall back below threshold while protected, MUST close the gate, and the
 *   thread MUST go unprotected again to regenerate evidence -- reproducing
 *   the feedback loop. There is no THRESH that is simultaneously (a) high
 *   enough to mean "recently caught" and (b) immune to the collapse; (a) and
 *   (b) are mutually exclusive by construction. k only changes how fast you
 *   arrive at the same place: bigger k = slower response (needs more hits to
 *   open, decays more slowly once open -- longer window, but also slower to
 *   reopen after it closes); it does not change the qualitative outcome.
 *
 * This sweep grids (k, THRESH-as-fraction-of-SCALE) and measures, per cell:
 *   HOT catch% (want: near NOGATE baseline), COLD hot_verdict count (want:
 *   0 -- tuning must not be bought by breaking cold exclusion).
 *
 * Build: gcc -O2 -o hotthreads_sweep_test hotthreads_sweep_test.c -lpthread -lm
 * Run:   ./hotthreads_sweep_test [hot_n] [warmup_s] [measure_s]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <stdatomic.h>

#define rmb() asm volatile ("lfence" ::: "memory")

#define SCALE           1024
#define WAIT_THRESH     512

static int    g_ewma_k        = 3;    /* swept */
static int    preempt_thresh  = 40;   /* swept */

enum gate_mode { MODE_NOGATE = 0, MODE_EWMA = 1 };

static double p_base_hot     = 0.25;
static double p_base_cold    = 0.004;
static double protect_factor = 0.08;

struct tstate {
        int  id;
        bool is_cold;
        double p_base;
        atomic_int wait_decay;
        atomic_int preempt_decay;
        unsigned int rng;
        _Atomic unsigned long long acquires;
        _Atomic unsigned long long contended;
        _Atomic unsigned long long steal_holds;
        _Atomic unsigned long long protected_holds;
};

static enum gate_mode g_mode;
static volatile int g_done = 0;
static volatile int g_measuring = 0;

static void wait_decay_update(struct tstate *t, bool contended)
{
        int old = atomic_load(&t->wait_decay);
        int sample = contended ? SCALE : 0;
        if (contended)
                atomic_store(&t->wait_decay, old + ((sample - old) >> g_ewma_k));
}

static void preempt_decay_update(struct tstate *t, bool stolen)
{
        int old = atomic_load(&t->preempt_decay);
        int sample = stolen ? SCALE : 0;
        atomic_store(&t->preempt_decay, old + ((sample - old) >> g_ewma_k));
}

static bool verdict_hot(struct tstate *t)
{
        switch (g_mode) {
        case MODE_NOGATE:
                return !t->is_cold;
        case MODE_EWMA:
                return atomic_load(&t->wait_decay) > WAIT_THRESH &&
                       atomic_load(&t->preempt_decay) > preempt_thresh;
        }
        return false;
}

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

static void locked_op(struct tstate *t, struct lockctx *c, int cs_iters)
{
        bool contended = atomic_load(&c->waiters) > 0;
        atomic_fetch_add(&c->waiters, 1);
        while (cmpxchg_ul(&c->lock, 0, 1) != 0)
                rmb();
        atomic_fetch_sub(&c->waiters, 1);

        wait_decay_update(t, contended);
        bool protect = verdict_hot(t);

        for (int i = 0; i < cs_iters; i++)
                asm volatile ("sfence" ::: "memory");
        c->lock = 0;

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

static int hot_cs_iters = 20000;
static void *hot_thread(void *arg)
{
        struct tstate *t = arg;
        while (!g_done)
                locked_op(t, &hot_ctx, hot_cs_iters);
        return NULL;
}
static void *cold_thread(void *arg)
{
        struct tstate *t = arg;
        while (!g_done) {
                locked_op(t, &cold_ctx, 400);
                usleep(50000);
        }
        return NULL;
}

struct cond_result {
        double hot_catch_pct, hot_protect_pct;
        double cold_catch_pct;
        int    cold_hot_verdict;
};

static struct cond_result run_condition(enum gate_mode mode, int hot_n, int cold_n,
                                         int warmup_s, int measure_s, bool verbose)
{
        struct tstate hot[32], cold[8];
        pthread_t htid[32], ctid[8];
        if (hot_n > 32) hot_n = 32;
        if (cold_n > 8) cold_n = 8;

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
        g_measuring = 0;

        if (verbose)
                printf("=== k=%d thresh=%d (%.4f of SCALE) ===\n",
                       g_ewma_k, preempt_thresh, (double)preempt_thresh / SCALE);

        for (int i = 0; i < hot_n; i++) pthread_create(&htid[i], NULL, hot_thread, &hot[i]);
        for (int i = 0; i < cold_n; i++) pthread_create(&ctid[i], NULL, cold_thread, &cold[i]);

        for (int s = 0; s < warmup_s; s++) sleep(1);
        g_measuring = 1;
        for (int s = 0; s < measure_s; s++) sleep(1);

        g_done = 1;
        for (int i = 0; i < hot_n; i++) pthread_join(htid[i], NULL);
        for (int i = 0; i < cold_n; i++) pthread_join(ctid[i], NULL);

        struct cond_result r; memset(&r, 0, sizeof(r));
        unsigned long long acq = 0, steal = 0, prot = 0;
        for (int i = 0; i < hot_n; i++) {
                acq  += atomic_load(&hot[i].acquires);
                steal+= atomic_load(&hot[i].steal_holds);
                prot += atomic_load(&hot[i].protected_holds);
        }
        r.hot_catch_pct   = acq ? 100.0 * steal / acq : 0.0;
        r.hot_protect_pct = acq ? 100.0 * prot  / acq : 0.0;

        unsigned long long cacq = 0, csteal = 0;
        for (int i = 0; i < cold_n; i++) {
                cacq  += atomic_load(&cold[i].acquires);
                csteal+= atomic_load(&cold[i].steal_holds);
                /* re-check verdict post-hoc using final decay state */
                if (verdict_hot(&cold[i])) r.cold_hot_verdict++;
        }
        r.cold_catch_pct = cacq ? 100.0 * csteal / cacq : 0.0;
        return r;
}

int main(int argc, char **argv)
{
        int hot_n     = argc > 1 ? atoi(argv[1]) : 8;
        int warmup_s  = argc > 2 ? atoi(argv[2]) : 2;
        int measure_s = argc > 3 ? atoi(argv[3]) : 5;
        int cold_n = 4;

        printf("Hot Threads EWMA-gate TUNING SWEEP (k x preempt_thresh)\n");
        printf("SCALE=%d WAIT_THRESH=%d  p_base_hot=%.3f protect_factor=%.3f "
               "-> p_protected=%.4f  p_base_cold=%.4f\n",
               SCALE, WAIT_THRESH, p_base_hot, protect_factor,
               p_base_hot * protect_factor, p_base_cold);
        printf("hot_n=%d cold_n=%d warmup=%ds measure=%ds per cell\n\n",
               hot_n, cold_n, warmup_s, measure_s);

        /* baseline: unconditional protect, independent of k/thresh */
        g_ewma_k = 3; preempt_thresh = 40;
        printf(">>> BASELINE (a) NO GATE\n");
        struct cond_result base = run_condition(MODE_NOGATE, hot_n, cold_n, warmup_s, measure_s, true);
        printf("    HOT catch=%.2f%% protect=%.1f%%  COLD catch=%.2f%%\n\n",
               base.hot_catch_pct, base.hot_protect_pct, base.cold_catch_pct);

        int ks[]        = {2, 3, 4, 5, 6, 8};
        double fracs[]   = {0.005, 0.01, 0.02, 0.039, 0.078, 0.15, 0.25};
        int nk = sizeof(ks)/sizeof(ks[0]);
        int nf = sizeof(fracs)/sizeof(fracs[0]);

        printf("%-4s %-8s %-8s %-10s %-10s %-10s %-10s %-8s\n",
               "k", "frac", "thresh", "spike_S1", "pred_n*", "HOTcatch%", "HOTprot%", "COLDviol");
        int fixed_count = 0, total = 0, cold_break = 0;
        for (int ik = 0; ik < nk; ik++) {
                for (int jf = 0; jf < nf; jf++) {
                        g_ewma_k = ks[ik];
                        preempt_thresh = (int)(fracs[jf] * SCALE);
                        if (preempt_thresh < 1) preempt_thresh = 1;

                        double spike1 = SCALE * pow(2.0, -g_ewma_k);
                        double predn;
                        double e_prot = SCALE * (p_base_hot * protect_factor);
                        if (preempt_thresh < e_prot) {
                                predn = INFINITY; /* never closes: de-facto latch */
                        } else if (preempt_thresh < spike1) {
                                double ratio = (double)preempt_thresh / spike1;
                                predn = log(ratio) / log(1.0 - pow(2.0, -g_ewma_k));
                        } else {
                                predn = -1; /* needs >1 catch to even open */
                        }

                        struct cond_result r = run_condition(MODE_EWMA, hot_n, cold_n,
                                                              warmup_s, measure_s, false);
                        bool fixed = r.hot_catch_pct <= 1.5 * base.hot_catch_pct;
                        bool cold_v = r.cold_hot_verdict > 0;
                        total++;
                        if (fixed) fixed_count++;
                        if (cold_v) cold_break++;

                        if (predn == INFINITY)
                                printf("%-4d %-8.4f %-8d %-10.1f %-10s %-10.2f %-10.1f %-8s\n",
                                       g_ewma_k, fracs[jf], preempt_thresh, spike1, "latch!",
                                       r.hot_catch_pct, r.hot_protect_pct,
                                       cold_v ? "YES" : "no");
                        else if (predn < 0)
                                printf("%-4d %-8.4f %-8d %-10.1f %-10s %-10.2f %-10.1f %-8s\n",
                                       g_ewma_k, fracs[jf], preempt_thresh, spike1, ">1hit",
                                       r.hot_catch_pct, r.hot_protect_pct,
                                       cold_v ? "YES" : "no");
                        else
                                printf("%-4d %-8.4f %-8d %-10.1f %-10.2f %-10.2f %-10.1f %-8s\n",
                                       g_ewma_k, fracs[jf], preempt_thresh, spike1, predn,
                                       r.hot_catch_pct, r.hot_protect_pct,
                                       cold_v ? "YES" : "no");
                        fflush(stdout);
                }
        }

        printf("\n=============================== SWEEP SUMMARY ===============================\n");
        printf("baseline (a) HOT catch%% = %.2f%% (fix threshold: <= %.2f%%)\n",
               base.hot_catch_pct, 1.5 * base.hot_catch_pct);
        printf("cells tested: %d   cells that 'fixed' the regression (<=1.5x baseline): %d\n",
               total, fixed_count);
        printf("cells where cold population broke gate (false-positive HOT verdict): %d\n",
               cold_break);
        printf("\nFINAL VERDICT:\n");
        printf("  Tuning k/threshold alone fixes the regression WITHOUT approximating a\n"
               "  permanent latch (i.e. a cell fixed HOT catch%% while pred_n* stayed finite\n"
               "  and small, not 'latch!'): %s\n",
               "see table -- any 'fixed' row should be cross-checked against its pred_n*/latch column");
        return 0;
}
