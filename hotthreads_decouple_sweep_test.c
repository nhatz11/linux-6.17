/*
 * hotthreads_decouple_sweep_test.c
 *
 * DECOUPLED-k EXTENSION of hotthreads_sweep_test.c.
 *
 * The shipped design (and the original 42-cell sweep in hotthreads_sweep_test.c)
 * uses ONE shared EWMA shift constant `g_ewma_k` for BOTH wait_decay AND
 * preempt_decay -- matching the real kernel's single shared sysctl
 * ivh_hot_threads_ewma_k. That sweep held WAIT_THRESH fixed at 512 and varied
 * the shared k (which therefore changed BOTH counters' reaction speed at once)
 * and preempt_thresh, and found 0 non-latch cells that fixed the self-
 * undermining feedback regression.
 *
 * QUESTION UNDER TEST (project owner): if wait_decay gets its OWN independent
 * shift k_wait, separate from preempt_decay's own k_preempt, does that open a
 * region of parameter space that matches what the permanent latch achieves,
 * WITHOUT being a latch?
 *
 * STRUCTURAL PREDICTION (from hotthreads_sweep_test.c's header math): the
 * constraint that killed every cell is a statement purely about preempt_decay's
 * OWN internal dynamics --
 *     E_prot = SCALE * p_protected           (protected equilibrium; k-independent)
 *     S1     = SCALE * 2^-k_preempt          (one-catch spike from cold start)
 *     a non-degenerate "recency window" exists only when E_prot < THRESH < S1,
 *     and any THRESH in that window MUST decay back below itself while protected
 *     (reopening the feedback loop); any THRESH below E_prot IS a de-facto latch.
 * This references ONLY preempt_decay's k. It never mentions wait_decay or its k.
 * So decoupling k_wait should NOT change the verdict: k_preempt is exactly the
 * dimension the original sweep already varied (it merely happened to be tied to
 * k_wait too). wait_decay for the HOT population is fed SCALE on ~100% of
 * acquisitions and never 0, so its steady state is SCALE regardless of k_wait --
 * enormous margin over the 512 gate; k_wait can only change how FAST it first
 * crosses 512 (an onset/warmup effect), not whether it stays above it.
 *
 * WHAT THIS PROGRAM DOES to verify that concretely rather than assert it:
 *   (1) Re-runs the SAME preempt_thresh x k_preempt grid the original ran, but
 *       with k_wait DECOUPLED and pinned at its already-validated value 3. If
 *       decoupling is inert (as predicted) this must reproduce the original
 *       0-non-latch-fix result cell-for-cell.
 *   (2) Tests something genuinely NEW: sweeps k_wait itself AWAY from 3 (up to
 *       very large values that drastically slow wait_decay's reaction), holding
 *       k_preempt at the shipped 3, to catch any non-obvious interaction the
 *       closed-form math misses -- warmup/onset timing, or coupling through when
 *       each side of the AND-gate first crosses its threshold.
 *
 * Everything else (closed feedback loop, simulated protection-suppressed steal,
 * cold population, thresholds, p_base/protect_factor) is IDENTICAL to
 * hotthreads_sweep_test.c so the numbers are directly comparable.
 *
 * Build: gcc -O2 -o hotthreads_decouple_sweep_test hotthreads_decouple_sweep_test.c -lpthread -lm
 * Run:   ./hotthreads_decouple_sweep_test [hot_n] [warmup_s] [measure_s]
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

/* DECOUPLED: two independent shift constants instead of one shared g_ewma_k. */
static int    g_k_wait        = 3;    /* wait_decay's own shift  */
static int    g_k_preempt     = 3;    /* preempt_decay's own shift (== old swept k) */
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

/* wait_decay now uses its OWN k_wait (was the shared g_ewma_k). */
static void wait_decay_update(struct tstate *t, bool contended)
{
        int old = atomic_load(&t->wait_decay);
        int sample = contended ? SCALE : 0;
        if (contended)
                atomic_store(&t->wait_decay, old + ((sample - old) >> g_k_wait));
}

/* preempt_decay now uses its OWN k_preempt (was the shared g_ewma_k). */
static void preempt_decay_update(struct tstate *t, bool stolen)
{
        int old = atomic_load(&t->preempt_decay);
        int sample = stolen ? SCALE : 0;
        atomic_store(&t->preempt_decay, old + ((sample - old) >> g_k_preempt));
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
        double hot_wait, hot_preempt;
        double cold_catch_pct;
        int    cold_hot_verdict;
};

static struct cond_result run_condition(enum gate_mode mode, int hot_n, int cold_n,
                                         int warmup_s, int measure_s)
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
        long wd = 0, pd = 0;
        for (int i = 0; i < hot_n; i++) {
                acq  += atomic_load(&hot[i].acquires);
                steal+= atomic_load(&hot[i].steal_holds);
                prot += atomic_load(&hot[i].protected_holds);
                wd   += atomic_load(&hot[i].wait_decay);
                pd   += atomic_load(&hot[i].preempt_decay);
        }
        r.hot_catch_pct   = acq ? 100.0 * steal / acq : 0.0;
        r.hot_protect_pct = acq ? 100.0 * prot  / acq : 0.0;
        r.hot_wait    = (double)wd / hot_n / SCALE;
        r.hot_preempt = (double)pd / hot_n / SCALE;

        unsigned long long cacq = 0, csteal = 0;
        for (int i = 0; i < cold_n; i++) {
                cacq  += atomic_load(&cold[i].acquires);
                csteal+= atomic_load(&cold[i].steal_holds);
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

        printf("Hot Threads DECOUPLED-k sweep (k_wait separate from k_preempt)\n");
        printf("SCALE=%d WAIT_THRESH=%d  p_base_hot=%.3f protect_factor=%.3f "
               "-> p_protected=%.4f (E_prot=%.1f)  p_base_cold=%.4f\n",
               SCALE, WAIT_THRESH, p_base_hot, protect_factor,
               p_base_hot * protect_factor, SCALE * p_base_hot * protect_factor, p_base_cold);
        printf("hot_n=%d cold_n=%d warmup=%ds measure=%ds per cell\n\n",
               hot_n, cold_n, warmup_s, measure_s);

        /* baseline: unconditional protect, independent of any k/thresh */
        g_k_wait = 3; g_k_preempt = 3; preempt_thresh = 40;
        struct cond_result base = run_condition(MODE_NOGATE, hot_n, cold_n, warmup_s, measure_s);
        printf(">>> BASELINE (a) NO GATE:  HOT catch=%.2f%% protect=%.1f%%  COLD catch=%.2f%%\n",
               base.hot_catch_pct, base.hot_protect_pct, base.cold_catch_pct);
        printf("    fix threshold (HOT catch must be <= 1.5x baseline): %.2f%%\n\n",
               1.5 * base.hot_catch_pct);

        int ks[]        = {2, 3, 4, 5, 6, 8};
        double fracs[]   = {0.005, 0.01, 0.02, 0.039, 0.078, 0.15, 0.25};
        int nk = sizeof(ks)/sizeof(ks[0]);
        int nf = sizeof(fracs)/sizeof(fracs[0]);
        double e_prot = SCALE * (p_base_hot * protect_factor);

        /* ================= PART A: decouple, hold k_wait=3, sweep k_preempt =========
         * This must reproduce the original shared-k sweep cell-for-cell if
         * decoupling is inert. */
        printf("############ PART A: k_wait PINNED at 3, sweep (k_preempt x thresh) ############\n");
        printf("(this is the ORIGINAL sweep grid with k_wait decoupled & held at its validated 3)\n\n");
        printf("%-6s %-6s %-8s %-8s %-10s %-10s %-10s %-9s %-8s\n",
               "kwait", "kprmt", "frac", "thresh", "spike_S1", "pred_n*", "HOTcatch%", "HOTprot%", "COLDviol");
        int fixed_count = 0, total = 0, cold_break = 0, nonlatch_fix = 0;
        for (int ik = 0; ik < nk; ik++) {
                for (int jf = 0; jf < nf; jf++) {
                        g_k_wait = 3;
                        g_k_preempt = ks[ik];
                        preempt_thresh = (int)(fracs[jf] * SCALE);
                        if (preempt_thresh < 1) preempt_thresh = 1;

                        double spike1 = SCALE * pow(2.0, -g_k_preempt);
                        double predn;
                        bool is_latch = false;
                        if (preempt_thresh < e_prot) { predn = INFINITY; is_latch = true; }
                        else if (preempt_thresh < spike1)
                                predn = log((double)preempt_thresh / spike1) /
                                        log(1.0 - pow(2.0, -g_k_preempt));
                        else predn = -1;

                        struct cond_result r = run_condition(MODE_EWMA, hot_n, cold_n,
                                                              warmup_s, measure_s);
                        bool fixed = r.hot_catch_pct <= 1.5 * base.hot_catch_pct;
                        bool cold_v = r.cold_hot_verdict > 0;
                        total++;
                        if (fixed) fixed_count++;
                        if (cold_v) cold_break++;
                        if (fixed && !is_latch && !cold_v) nonlatch_fix++;

                        char pncol[16];
                        if (is_latch) snprintf(pncol, sizeof(pncol), "latch!");
                        else if (predn < 0) snprintf(pncol, sizeof(pncol), ">1hit");
                        else snprintf(pncol, sizeof(pncol), "%.2f", predn);

                        printf("%-6d %-6d %-8.4f %-8d %-10.1f %-10s %-10.2f %-9.1f %-8s\n",
                               g_k_wait, g_k_preempt, fracs[jf], preempt_thresh, spike1, pncol,
                               r.hot_catch_pct, r.hot_protect_pct, cold_v ? "YES" : "no");
                        fflush(stdout);
                }
        }
        printf("\nPART A: %d cells, %d 'fixed' (<=1.5x baseline), %d cold-breaks, "
               "%d NON-LATCH non-cold-break fixes\n\n", total, fixed_count, cold_break, nonlatch_fix);

        /* ================= PART B: NEW -- vary k_wait, hold k_preempt=3 ==============
         * The structural math says wait_decay's k cannot matter (hot pop's
         * wait_decay steady state = SCALE >> 512 for any k_wait). Test it: push
         * k_wait far from 3 across the full preempt_thresh row, k_preempt fixed at
         * the shipped 3. Watch for ANY onset/timing interaction. */
        printf("############ PART B (NEW): sweep k_wait, k_preempt PINNED at 3 ############\n");
        printf("(does slowing wait_decay's OWN reaction change the aggregate result?)\n\n");
        int kwaits[] = {1, 2, 3, 4, 6, 8, 12, 16};
        int nkw = sizeof(kwaits)/sizeof(kwaits[0]);
        printf("%-6s %-6s %-8s %-8s %-10s %-9s %-9s %-9s %-8s\n",
               "kwait", "kprmt", "frac", "thresh", "HOTcatch%", "HOTprot%", "HOTwait", "HOTprmt", "COLDviol");
        int b_total = 0, b_fixed = 0, b_nonlatch_fix = 0, b_coldbreak = 0;
        for (int ikw = 0; ikw < nkw; ikw++) {
                for (int jf = 0; jf < nf; jf++) {
                        g_k_wait = kwaits[ikw];
                        g_k_preempt = 3;
                        preempt_thresh = (int)(fracs[jf] * SCALE);
                        if (preempt_thresh < 1) preempt_thresh = 1;
                        bool is_latch = preempt_thresh < e_prot;

                        struct cond_result r = run_condition(MODE_EWMA, hot_n, cold_n,
                                                              warmup_s, measure_s);
                        bool fixed = r.hot_catch_pct <= 1.5 * base.hot_catch_pct;
                        bool cold_v = r.cold_hot_verdict > 0;
                        b_total++;
                        if (fixed) b_fixed++;
                        if (cold_v) b_coldbreak++;
                        if (fixed && !is_latch && !cold_v) b_nonlatch_fix++;

                        printf("%-6d %-6d %-8.4f %-8d %-10.2f %-9.1f %-9.3f %-9.3f %-8s%s\n",
                               g_k_wait, g_k_preempt, fracs[jf], preempt_thresh,
                               r.hot_catch_pct, r.hot_protect_pct, r.hot_wait, r.hot_preempt,
                               cold_v ? "YES" : "no", is_latch ? "  (thresh<E_prot=latch)" : "");
                        fflush(stdout);
                }
        }
        printf("\nPART B: %d cells, %d 'fixed', %d cold-breaks, %d NON-LATCH non-cold-break fixes\n\n",
               b_total, b_fixed, b_coldbreak, b_nonlatch_fix);

        printf("=============================== OVERALL VERDICT ===============================\n");
        printf("baseline (a) HOT catch%% = %.2f%%   (a cell 'fixes' iff HOT catch <= %.2f%%)\n",
               base.hot_catch_pct, 1.5 * base.hot_catch_pct);
        printf("PART A (k_wait=3, sweep k_preempt): NON-LATCH non-cold-break fixes = %d / %d\n",
               nonlatch_fix, total);
        printf("PART B (sweep k_wait, k_preempt=3):  NON-LATCH non-cold-break fixes = %d / %d\n",
               b_nonlatch_fix, b_total);
        printf("\nAny non-latch fix in EITHER part that the original shared-k sweep lacked would\n"
               "be the finding. If both are 0, decoupling k_wait from k_preempt does NOT open new\n"
               "non-latch space -- consistent with the structural math: the collapse is intrinsic\n"
               "to preempt_decay's own dynamics (E_prot < THRESH < S1), a function of k_preempt\n"
               "alone; wait_decay's k is irrelevant because the hot pop's wait_decay sits at SCALE\n"
               "(>>512) for any k_wait, so the wait half of the AND-gate is never the binding one.\n");
        return 0;
}
