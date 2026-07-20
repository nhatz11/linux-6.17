/*
 * hotthreads_cooldown_test.c
 *
 * COOLDOWN design + validation for the Hot Threads AND-gate self-undermining
 * feedback regression. Direct successor to hotthreads_latch_test.c: SAME closed
 * feedback loop, SAME simulated protection-suppressed steal model, SAME
 * workload, thresholds, p_base/protect_factor -- so every catch% here is
 * directly comparable to that file's baseline (a)~2.1% / regression (b) / latch
 * (c)~2.0% numbers. Do not "fix" this by changing the model; the model is the
 * shared yardstick.
 *
 * ===========================================================================
 * WHY A COOLDOWN, AND WHY THE PRIOR SWEEPS COULDN'T FIND ONE
 * ---------------------------------------------------------------------------
 * preempt_decay is a k=3 EWMA fed SCALE on a real steal catch, 0 otherwise.
 * Under protection catches are suppressed (p_eff = p_base*protect_factor), so
 * the evidence that opened the gate decays away and the gate re-closes -- the
 * feedback loop. hotthreads_sweep_test.c proved (140 cells, 2 files) that NO
 * symmetric (k, thresh) escapes this: under protection the counter falls toward
 *     E_prot = SCALE * p_protected              (== 20.5 here, k-INDEPENDENT)
 * so a threshold with any real "recency" meaning (THRESH > E_prot) MUST be
 * crossed on the way down -> gate closes -> collapse; and the only stable
 * cells (THRESH < E_prot=20.5) are de-facto permanent latches with a
 * meaningless sub-2%-of-SCALE bar. The killer fact: E_prot is fixed by
 * p_protected alone. ONE shift constant k cannot decouple this because it
 * scales rise and fall together.
 *
 * THE ASYMMETRY INSIGHT (Variant A). Give preempt_decay its OWN two shift
 * constants: k_rise on a catch (sample=SCALE), k_fall on a 0-feed (decay).
 * This has never been swept -- every prior sweep used ONE k per counter applied
 * to BOTH directions (decoupling wait_decay's k from preempt_decay's k, already
 * tested, is a different axis entirely; this decouples preempt_decay's OWN rise
 * from its OWN fall). Re-derive the protected equilibrium from the mean-update
 * fixed point with a=2^-k_rise, b=2^-k_fall:
 *     p*(SCALE-E)*a - (1-p)*E*b = 0
 *  => E_prot = SCALE / ( 1 + ((1-p)/p) * 2^-(k_fall-k_rise) )         (*)
 * Symmetric (k_fall=k_rise) recovers E_prot = SCALE*p = 20.5 exactly. But with
 * k_fall > k_rise the protected equilibrium is RAISED, and (crucially) it is
 * raised ABOVE the fixed threshold while STILL being a function of the thread's
 * TRUE current catch propensity p. With p_protected=0.02, THRESH=40:
 *     Dk=k_fall-k_rise:  0->20.5(below)  1->40.2  2->77.3  3->143.7  4->252
 *                        5->404  6->580  8->860  (all ABOVE thresh for Dk>=1)
 * So for Dk>=2 the protected equilibrium sits comfortably above threshold:
 * protection SUSTAINS itself (the rare p_protected catches + slow fall hold the
 * counter up) instead of self-undermining. This is NOT the rejected de-facto
 * latch: E_prot still tracks p. If the thread GENUINELY goes cold (its real
 * behaviour changes: p -> 0), (*) gives E_prot -> 0 and the counter decays to
 * below threshold over ~2^k_fall holds -> eligibility LAPSES. Fast to trigger
 * (k_rise=3: one catch spikes to S1=128 >> 40), slow, finite, self-adjusting
 * release (k_fall large). A real cooldown, memory ~= 2^k_fall holds.
 *
 * VARIANT B -- literal cooldown window. On a catch that crosses threshold, force
 * the gate OPEN for a fixed window, then require fresh evidence again. Modelled
 * two ways: COOL_HOLDS (window = N release events, no clock at all) and
 * COOL_WALL (window = N seconds of real time). Both err toward CONTINUED
 * protection, so neither reproduces the old "Model B" trap (which decayed a READ
 * value on wall-clock and made a hot lock look cold during the exact stall that
 * mattered -- hiding a present danger). But B has its own tax: at each window
 * expiry the gate re-closes for >=1 hold to re-test, re-exposing the thread
 * (one catch per window) -- a bounded, periodic residue of the regression -- and
 * COOL_WALL's window length in *holds* depends on the thread's hold rate, plus
 * "N real seconds" needs a clock. See the kernel-cost note at the end.
 *
 * ===========================================================================
 * CONDITIONS (same workload/duration; catch% directly comparable to prior files)
 *   (a) NO GATE        - protect every eligible thread; known-good low baseline
 *   (b) EWMA symmetric - shipped design; must REPRODUCE the regression
 *   (c) PERM LATCH     - reference fix (works, but never lapses -> owner dislikes)
 *   (d) ASYM EWMA      - Variant A, swept over k_fall (k_rise=3 fixed)
 *   (e) COOL_HOLDS     - Variant B, window = N release events, swept over N
 *   (f) COOL_WALL      - Variant B, window = N real seconds (one representative)
 * Plus a LAPSE test: make the hot pop genuinely go cold mid-run and measure how
 * many holds each mechanism takes to correctly drop eligibility (LATCH: never).
 *
 * Build: gcc -O2 -o hotthreads_cooldown_test hotthreads_cooldown_test.c -lpthread -lm
 * Run:   ./hotthreads_cooldown_test [hot_n] [measure_s] [p_base_hot] [protect_factor]
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
#define WAIT_THRESH     512
static int preempt_thresh = 40;

/* ---- gate modes ---- */
enum gate_mode {
        MODE_NOGATE = 0,
        MODE_EWMA   = 1,   /* symmetric shipped design (regression)  */
        MODE_LATCH  = 2,   /* permanent one-way latch (reference fix)*/
        MODE_ASYM   = 3,   /* Variant A: k_rise / k_fall             */
        MODE_COOL_HOLDS = 4, /* Variant B: window = N release events */
        MODE_COOL_WALL  = 5, /* Variant B: window = N real seconds   */
};

/* ---- EWMA shift constants ---- */
static int g_k_sym   = 3;   /* symmetric k for (b),(c),(e),(f)'s trigger EWMA */
static int g_k_rise  = 3;   /* ASYM: shift on a catch (sample=SCALE)          */
static int g_k_fall  = 8;   /* ASYM: shift on a 0-feed (decay)                */

/* ---- Variant B window sizes ---- */
static int    g_cool_holds  = 256;   /* COOL_HOLDS window, in release events   */
static double g_cool_wall_s = 0.20;  /* COOL_WALL window, in seconds           */

/* ---- simulated steal / protection loop parameters ---- */
static double p_base_hot   = 0.25;
static double p_base_cold  = 0.004;
static double protect_factor = 0.08; /* -> p_protected = 0.02, kernel gate-OFF ~2.1% */

/* ---- per-thread state ---- */
struct tstate {
        int  id;
        bool is_cold;
        double p_base;
        _Atomic double cur_p_base;  /* live p_base (lapse test flips this)     */

        atomic_int wait_decay;
        atomic_int preempt_decay;
        atomic_int preempt_latched;     /* MODE_LATCH                          */
        atomic_int cool_holds_left;     /* MODE_COOL_HOLDS countdown           */
        _Atomic double cool_deadline;   /* MODE_COOL_WALL deadline (sim clock) */

        unsigned int rng;

        _Atomic unsigned long long acquires;
        _Atomic unsigned long long contended;
        _Atomic unsigned long long steal_holds;
        _Atomic unsigned long long protected_holds;
};

static enum gate_mode g_mode;
static volatile int g_done = 0;
static volatile int g_measuring = 0;

static inline double now_s(void)
{
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ---- EWMA feeds (event-driven, write-time only) ---- */
static void wait_decay_update(struct tstate *t, bool contended)
{
        int old = atomic_load(&t->wait_decay);
        int sample = contended ? SCALE : 0;
        if (contended)
                atomic_store(&t->wait_decay, old + ((sample - old) >> g_k_sym));
}

/* preempt EWMA + per-mode trigger/window arming. Returns nothing; verdict reads
 * the resulting counter/latch/window state on the NEXT hold. */
static void preempt_decay_update(struct tstate *t, bool stolen)
{
        int old = atomic_load(&t->preempt_decay);
        int sample = stolen ? SCALE : 0;
        int k;

        if (g_mode == MODE_ASYM)
                k = stolen ? g_k_rise : g_k_fall;   /* THE asymmetry */
        else
                k = g_k_sym;

        int nw = old + ((sample - old) >> k);
        atomic_store(&t->preempt_decay, nw);

        /* permanent latch (reference) */
        if (g_mode == MODE_LATCH && nw > preempt_thresh &&
            !atomic_load(&t->preempt_latched))
                atomic_store(&t->preempt_latched, 1);

        /* Variant B: crossing threshold (re)arms the cooldown window */
        if (nw > preempt_thresh) {
                if (g_mode == MODE_COOL_HOLDS)
                        atomic_store(&t->cool_holds_left, g_cool_holds);
                else if (g_mode == MODE_COOL_WALL)
                        atomic_store(&t->cool_deadline, now_s() + g_cool_wall_s);
        }
}

/* verdict: is this thread HOT (-> protect this hold) under the active gate? */
static bool verdict_hot(struct tstate *t)
{
        bool wait_ok = atomic_load(&t->wait_decay) > WAIT_THRESH;
        switch (g_mode) {
        case MODE_NOGATE:
                return !t->is_cold;
        case MODE_EWMA:
        case MODE_ASYM:
                return wait_ok && atomic_load(&t->preempt_decay) > preempt_thresh;
        case MODE_LATCH:
                return wait_ok && atomic_load(&t->preempt_latched);
        case MODE_COOL_HOLDS:
                return wait_ok && (atomic_load(&t->preempt_decay) > preempt_thresh ||
                                   atomic_load(&t->cool_holds_left) > 0);
        case MODE_COOL_WALL:
                return wait_ok && (atomic_load(&t->preempt_decay) > preempt_thresh ||
                                   now_s() < atomic_load(&t->cool_deadline));
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

        double pb = atomic_load(&t->cur_p_base);
        double p_eff = protect ? pb * protect_factor : pb;
        bool stolen = rnd01(&t->rng) < p_eff;

        preempt_decay_update(t, stolen);

        /* Variant B (holds): every release burns one window credit */
        if (g_mode == MODE_COOL_HOLDS) {
                int left = atomic_load(&t->cool_holds_left);
                if (left > 0)
                        atomic_store(&t->cool_holds_left, left - 1);
        }

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
        double hot_catch_pct, hot_preempt, hot_protect_pct;
        double cold_catch_pct;
        int    cold_hot_verdict;
        double holds_per_s;   /* hot-pop hold rate, for holds<->seconds mapping */
};

static void init_threads(struct tstate *hot, int hot_n, struct tstate *cold, int cold_n)
{
        memset(hot, 0, sizeof(struct tstate) * hot_n);
        memset(cold, 0, sizeof(struct tstate) * cold_n);
        for (int i = 0; i < hot_n; i++) {
                hot[i].id = i; hot[i].is_cold = false; hot[i].p_base = p_base_hot;
                atomic_store(&hot[i].cur_p_base, p_base_hot);
                hot[i].rng = 0x9e3779b9u ^ (0xC0FFEEu * (i + 1));
        }
        for (int i = 0; i < cold_n; i++) {
                cold[i].id = 100 + i; cold[i].is_cold = true; cold[i].p_base = p_base_cold;
                atomic_store(&cold[i].cur_p_base, p_base_cold);
                cold[i].rng = 0x1234567u ^ (0xB16B00Bu * (i + 3));
        }
        hot_ctx.lock = 0; atomic_store(&hot_ctx.waiters, 0);
        cold_ctx.lock = 0; atomic_store(&cold_ctx.waiters, 0);
}

static struct cond_result run_condition(enum gate_mode mode, int hot_n, int cold_n,
                                         int warmup_s, int measure_s)
{
        struct tstate hot[64], cold[16];
        pthread_t htid[64], ctid[16];
        if (hot_n > 64) hot_n = 64;
        if (cold_n > 16) cold_n = 16;

        init_threads(hot, hot_n, cold, cold_n);
        g_mode = mode;
        g_done = 0;
        g_measuring = 0;

        for (int i = 0; i < hot_n; i++) pthread_create(&htid[i], NULL, hot_thread, &hot[i]);
        for (int i = 0; i < cold_n; i++) pthread_create(&ctid[i], NULL, cold_thread, &cold[i]);

        for (int s = 0; s < warmup_s; s++) sleep(1);
        g_measuring = 1;
        double t0 = now_s();
        for (int s = 0; s < measure_s; s++) sleep(1);
        double dt = now_s() - t0;

        g_done = 1;
        for (int i = 0; i < hot_n; i++) pthread_join(htid[i], NULL);
        for (int i = 0; i < cold_n; i++) pthread_join(ctid[i], NULL);

        struct cond_result r; memset(&r, 0, sizeof(r));
        unsigned long long acq = 0, steal = 0, prot = 0; long pd = 0;
        for (int i = 0; i < hot_n; i++) {
                acq  += atomic_load(&hot[i].acquires);
                steal+= atomic_load(&hot[i].steal_holds);
                prot += atomic_load(&hot[i].protected_holds);
                pd   += atomic_load(&hot[i].preempt_decay);
        }
        r.hot_catch_pct   = acq ? 100.0 * steal / acq : 0.0;
        r.hot_protect_pct = acq ? 100.0 * prot  / acq : 0.0;
        r.hot_preempt = (double)pd / hot_n;
        r.holds_per_s = dt > 0 ? acq / dt / hot_n : 0.0;

        unsigned long long cacq = 0, csteal = 0;
        for (int i = 0; i < cold_n; i++) {
                cacq  += atomic_load(&cold[i].acquires);
                csteal+= atomic_load(&cold[i].steal_holds);
                if (verdict_hot(&cold[i])) r.cold_hot_verdict++;
        }
        r.cold_catch_pct = cacq ? 100.0 * csteal / cacq : 0.0;
        return r;
}

/* =========================================================================
 * LAPSE TEST: does eligibility correctly expire when a hot thread's REAL
 * behaviour changes to cold? Warm up hot+being-caught, then set cur_p_base=0
 * for the whole hot pop (host no longer steals them even when exposed) and
 * measure how many of the thread's OWN holds elapse before verdict_hot goes
 * (and stays) false. LATCH must never lapse; ASYM/COOL must, in ~2^k_fall / ~N.
 * ========================================================================= */
static void lapse_test(enum gate_mode mode, const char *label, int hot_n)
{
        struct tstate hot[64], cold[16];
        pthread_t htid[64], ctid[16];
        int cold_n = 2;
        if (hot_n > 64) hot_n = 64;

        init_threads(hot, hot_n, cold, cold_n);
        g_mode = mode;
        g_done = 0;
        g_measuring = 1;   /* count acquires throughout so we can measure holds */

        for (int i = 0; i < hot_n; i++) pthread_create(&htid[i], NULL, hot_thread, &hot[i]);
        for (int i = 0; i < cold_n; i++) pthread_create(&ctid[i], NULL, cold_thread, &cold[i]);

        sleep(3);  /* warm up into the protected operating regime */

        /* snapshot acquires, then flip the whole hot pop genuinely cold */
        unsigned long long acq_at_flip = 0;
        for (int i = 0; i < hot_n; i++) acq_at_flip += atomic_load(&hot[i].acquires);
        for (int i = 0; i < hot_n; i++) atomic_store(&hot[i].cur_p_base, 0.0);
        double t_flip = now_s();

        /* poll until NO hot thread still reads HOT (sustained), or give up */
        double t_lapse = -1; unsigned long long acq_at_lapse = 0;
        for (int poll = 0; poll < 2000; poll++) {   /* up to ~20 s */
                usleep(10000);
                int hot_now = 0;
                for (int i = 0; i < hot_n; i++)
                        if (verdict_hot(&hot[i])) hot_now++;
                if (hot_now == 0) {
                        t_lapse = now_s() - t_flip;
                        acq_at_lapse = 0;
                        for (int i = 0; i < hot_n; i++)
                                acq_at_lapse += atomic_load(&hot[i].acquires);
                        break;
                }
        }

        g_done = 1;
        for (int i = 0; i < hot_n; i++) pthread_join(htid[i], NULL);
        for (int i = 0; i < cold_n; i++) pthread_join(ctid[i], NULL);

        if (t_lapse < 0) {
                printf("  %-26s LAPSE: NEVER (still eligible after 20s) -- "
                       "permanent, cannot follow behaviour change\n", label);
        } else {
                double holds = (double)(acq_at_lapse - acq_at_flip) / hot_n;
                printf("  %-26s LAPSE: %6.0f holds  (%.2f s of the thread's real activity)\n",
                       label, holds, t_lapse);
        }
}

static const char *mode_name(enum gate_mode m)
{
        switch (m) {
        case MODE_NOGATE: return "(a) NO GATE";
        case MODE_EWMA:   return "(b) EWMA symmetric (shipped)";
        case MODE_LATCH:  return "(c) PERM LATCH (ref fix)";
        case MODE_ASYM:   return "(d) ASYM EWMA (Variant A)";
        case MODE_COOL_HOLDS: return "(e) COOL_HOLDS (Variant B)";
        case MODE_COOL_WALL:  return "(f) COOL_WALL (Variant B)";
        }
        return "?";
}

int main(int argc, char **argv)
{
        int hot_n     = argc > 1 ? atoi(argv[1]) : 16;
        int measure_s = argc > 2 ? atoi(argv[2]) : 8;
        if (argc > 3) p_base_hot = atof(argv[3]);
        if (argc > 4) protect_factor = atof(argv[4]);
        int cold_n = 4, warmup_s = 3;
        double p_prot = p_base_hot * protect_factor;
        double e_prot_sym = SCALE * p_prot;

        printf("Hot Threads COOLDOWN design + validation\n");
        printf("SCALE=%d WAIT_THRESH=%d PREEMPT_THRESH=%d  k_sym=%d\n",
               SCALE, WAIT_THRESH, preempt_thresh, g_k_sym);
        printf("p_base_hot=%.3f protect_factor=%.3f -> p_protected=%.4f  "
               "E_prot(symmetric)=SCALE*p=%.1f  p_base_cold=%.4f\n",
               p_base_hot, protect_factor, p_prot, e_prot_sym, p_base_cold);
        printf("hot_n=%d cold_n=%d warmup=%ds measure=%ds\n\n", hot_n, cold_n, warmup_s, measure_s);

        /* ---- reference conditions (credibility) ---- */
        struct cond_result a = run_condition(MODE_NOGATE, hot_n, cold_n, warmup_s, measure_s);
        struct cond_result b = run_condition(MODE_EWMA,   hot_n, cold_n, warmup_s, measure_s);
        struct cond_result c = run_condition(MODE_LATCH,  hot_n, cold_n, warmup_s, measure_s);
        printf("REFERENCE CONDITIONS (must match prior files for the yardstick to hold):\n");
        printf("  %-30s HOTcatch=%6.2f%%  HOTprot=%5.1f%%  meanPreempt=%6.1f  COLDviol=%d\n",
               mode_name(MODE_NOGATE), a.hot_catch_pct, a.hot_protect_pct, a.hot_preempt, a.cold_hot_verdict);
        printf("  %-30s HOTcatch=%6.2f%%  HOTprot=%5.1f%%  meanPreempt=%6.1f  COLDviol=%d\n",
               mode_name(MODE_EWMA), b.hot_catch_pct, b.hot_protect_pct, b.hot_preempt, b.cold_hot_verdict);
        printf("  %-30s HOTcatch=%6.2f%%  HOTprot=%5.1f%%  meanPreempt=%6.1f  COLDviol=%d\n",
               mode_name(MODE_LATCH), c.hot_catch_pct, c.hot_protect_pct, c.hot_preempt, c.cold_hot_verdict);
        double reg = a.hot_catch_pct > 0 ? b.hot_catch_pct / a.hot_catch_pct : 0;
        printf("  credibility: (b) reproduces regression vs (a): %s [%.1fx];  "
               "(c) latch restores baseline: %s\n",
               b.hot_catch_pct > 1.5 * a.hot_catch_pct ? "YES" : "NO", reg,
               c.hot_catch_pct <= 1.5 * a.hot_catch_pct ? "YES" : "NO");
        printf("  hot-pop hold rate ~= %.0f holds/s/thread (for holds<->seconds mapping)\n\n",
               a.holds_per_s);

        double fixbar = 1.5 * a.hot_catch_pct;

        /* ---- Variant A: ASYM EWMA, sweep k_fall (k_rise=3) ---- */
        printf("############ VARIANT A: ASYMMETRIC EWMA  (k_rise=3 fixed, sweep k_fall) ############\n");
        printf("predicted E_prot = SCALE/(1+((1-p)/p)*2^-(k_fall-k_rise));  gate stable iff E_prot > %d\n",
               preempt_thresh);
        printf("%-6s %-6s %-10s %-11s %-10s %-9s %-9s %-8s\n",
               "krise", "kfall", "predE_prot", "measPreempt", "HOTcatch%", "HOTprot%", "vs base", "COLDviol");
        int kfalls[] = {3, 4, 5, 6, 8, 10};
        for (unsigned i = 0; i < sizeof(kfalls)/sizeof(kfalls[0]); i++) {
                g_k_rise = 3; g_k_fall = kfalls[i];
                int Dk = g_k_fall - g_k_rise;
                double e_pred = SCALE / (1.0 + ((1.0 - p_prot)/p_prot) * pow(2.0, -Dk));
                struct cond_result r = run_condition(MODE_ASYM, hot_n, cold_n, warmup_s, measure_s);
                printf("%-6d %-6d %-10.1f %-11.1f %-10.2f %-9.1f %-9s %-8d\n",
                       g_k_rise, g_k_fall, e_pred, r.hot_preempt, r.hot_catch_pct, r.hot_protect_pct,
                       r.hot_catch_pct <= fixbar ? "FIXED" : "regress", r.cold_hot_verdict);
                fflush(stdout);
        }
        g_k_rise = 3; g_k_fall = 8;

        /* ---- Variant B: COOL_HOLDS, sweep window N ---- */
        printf("\n############ VARIANT B: COOLDOWN WINDOW = N RELEASE EVENTS (sweep N) ############\n");
        printf("%-8s %-10s %-9s %-9s %-8s %-10s\n",
               "N_holds", "HOTcatch%", "HOTprot%", "vs base", "COLDviol", "~window_s");
        int Ns[] = {32, 64, 128, 256, 512, 1024, 4096};
        for (unsigned i = 0; i < sizeof(Ns)/sizeof(Ns[0]); i++) {
                g_cool_holds = Ns[i];
                struct cond_result r = run_condition(MODE_COOL_HOLDS, hot_n, cold_n, warmup_s, measure_s);
                printf("%-8d %-10.2f %-9.1f %-9s %-8d %-10.4f\n",
                       Ns[i], r.hot_catch_pct, r.hot_protect_pct,
                       r.hot_catch_pct <= fixbar ? "FIXED" : "regress",
                       r.cold_hot_verdict, a.holds_per_s > 0 ? Ns[i]/a.holds_per_s : 0);
                fflush(stdout);
        }
        g_cool_holds = 256;

        /* ---- Variant B: COOL_WALL, one representative window ---- */
        printf("\n############ VARIANT B: COOLDOWN WINDOW = N REAL SECONDS ############\n");
        printf("%-10s %-10s %-9s %-9s %-8s\n", "window_s", "HOTcatch%", "HOTprot%", "vs base", "COLDviol");
        double wins[] = {0.05, 0.20, 1.0};
        for (unsigned i = 0; i < sizeof(wins)/sizeof(wins[0]); i++) {
                g_cool_wall_s = wins[i];
                struct cond_result r = run_condition(MODE_COOL_WALL, hot_n, cold_n, warmup_s, measure_s);
                printf("%-10.2f %-10.2f %-9.1f %-9s %-8d\n",
                       wins[i], r.hot_catch_pct, r.hot_protect_pct,
                       r.hot_catch_pct <= fixbar ? "FIXED" : "regress", r.cold_hot_verdict);
                fflush(stdout);
        }

        /* ---- LAPSE TEST: does eligibility expire when behaviour truly changes? ---- */
        printf("\n############ LAPSE TEST: eligibility must expire when a thread goes truly cold ############\n");
        printf("(warm up hot+caught, then force whole hot pop cold p_base->0; measure holds to drop HOT)\n");
        lapse_test(MODE_LATCH, mode_name(MODE_LATCH), hot_n);
        g_k_rise = 3; g_k_fall = 5;  lapse_test(MODE_ASYM, "(d) ASYM k_fall=5", hot_n);
        g_k_rise = 3; g_k_fall = 8;  lapse_test(MODE_ASYM, "(d) ASYM k_fall=8", hot_n);
        g_k_rise = 3; g_k_fall = 10; lapse_test(MODE_ASYM, "(d) ASYM k_fall=10", hot_n);
        g_cool_holds = 256;  lapse_test(MODE_COOL_HOLDS, "(e) COOL_HOLDS N=256", hot_n);
        g_cool_holds = 1024; lapse_test(MODE_COOL_HOLDS, "(e) COOL_HOLDS N=1024", hot_n);

        printf("\nDONE.\n");
        return 0;
}
