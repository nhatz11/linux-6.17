/*
 * hotthreads_twostage_test.c
 *
 * Validation of the "drop preempt_decay from the Hot Threads gate" proposal.
 *
 * ===========================================================================
 * WHY THIS FILE EXISTS (the live finding that supersedes the cooldown work)
 * ---------------------------------------------------------------------------
 * The asymmetric-cooldown fix (hotthreads_cooldown_test.c, k_rise=3/k_fall=8)
 * was ported to the real kernel and REGRESSED HARD live: 11.95-12.58%
 * host-preempted-CS vs 1.8-2.4% baseline. Live bpftrace (1.96M samples) showed
 * the AND-gate's preempt_decay half was the failing leg essentially always:
 * preempt_decay read EXACTLY 0 for 90.4% of samples and cleared its threshold
 * (40) only 2.45% of the time; wait_decay was NEVER the sole failure
 * (0 / 1.96M). Root cause: preempt_decay is fed from real kernel raw-spinlock
 * holds via cs_enter()/cs_exit() (kernel/locking/spinlock.c), which are
 * SUB-MICROSECOND. Host steal arrives in ~5-6ms chunks. For a steal to register
 * as "this CS was stolen" it must land INSIDE one of those ~1us windows
 * (IVH_HOT_STEAL_FLOOR_NS=100us floor) -- which almost never happens by chance.
 * A genuine signal-source mismatch; unfixable by threshold tuning (threshold=0
 * still regressed because the counter is not merely low, it is EXACTLY ZERO 90%
 * of the time). The cooldown DESIGN is fine; the SIGNAL feeding it is near-null.
 *
 * ===========================================================================
 * THE PROPOSAL UNDER TEST: gate on wait_decay ALONE, drop preempt_decay.
 * ---------------------------------------------------------------------------
 * Rationale to VALIDATE (not assume): IVH's actual migration attempt,
 * bpf_sched_pre_lock_migrate() (kernel/sched/fair.c), already runs its OWN
 * real-time health check DOWNSTREAM of the Hot Threads gate:
 * ivh_steal_imminent() -- Gate 1+2: rq->cpu_capacity vs ivh_capacity_threshold
 * plus a time-left formula. That is a FRESH, per-CPU, real-time signal, not a
 * stale per-thread EWMA. preempt_decay's job ("is this thread's situation
 * currently dangerous") is REDUNDANT with Gate 1+2, which answers it more
 * accurately and downstream anyway. wait_decay's job ("does this workload
 * structurally create stranding contention") is NOT provided by anything else,
 * so it stays. Dropping preempt_decay: at most a wasted EVALUATION (not a wasted
 * migration) for a contended-but-currently-safe thread, since Gate 1+2 declines
 * the actual migration downstream.
 *
 * ===========================================================================
 * MODEL: TWO STAGES, faithful to the real kernel pipeline
 * ---------------------------------------------------------------------------
 * Every hold on the real kernel passes through, in order:
 *   Stage A  Hot Threads gate      (spinlock.c ivh_pre_lock, lines 193-200)
 *   [cooldown ivh_eval_cooldown_ok, fair.c -- per-vCPU 50us rate limiter]
 *   Stage B  Gate 1+2 ivh_steal_imminent (fair.c) -- real-time per-vCPU check
 * A migration (protection) engages ONLY if A passes AND B says "danger now".
 * Crucially Gate 1+2 EXISTS IN ALL CONDITIONS including the no-gate baseline --
 * the Hot Threads gate is an ADDITIONAL selectivity layer ON TOP of it. So the
 * yardstick baseline (~2.1%) already includes Gate 1+2 doing its job; the only
 * variable across conditions is Stage A.
 *
 * Latent per-hold "danger" (the vCPU is in a host-steal window during this
 * hold) ~ Bernoulli(p_danger). Grounded in this session's /proc/vcap_info
 * measurements: ~half the vCPUs "stolen" with meaningful live deltas, half
 * safe -> p_danger = 0.5. Stage B (ivh_steal_imminent) is the physical measure
 * of that danger (cpu_capacity dropping IS the steal), modelled as a detector
 * with sensitivity sB_sens (default 1.0; swept down to show robustness) and a
 * false-positive rate sB_fpr (accounting only -- a false alarm costs a wasted
 * MIGRATION but never increases catch, since there was no steal to catch).
 *
 * Steal outcome per hold:
 *   no danger            -> never stolen (no steal present to be caught by)
 *   danger, protected    -> stolen w.p. q * r   (r = residual after migration)
 *   danger, unprotected  -> stolen w.p. q
 * Calibrated: p_danger=0.5, q=0.246, r=0.171 =>
 *   ceiling (fully unprotected)   = p_danger*q      = 12.3%   (== live regression,
 *                                                     because the AND-gate is
 *                                                     ~always closed)
 *   baseline (always protected)   = p_danger*q*r    =  2.10%  (== live baseline)
 *
 * preempt_decay is modelled MECHANISTICALLY (event-driven EWMA, project
 * standard k=3) fed a SCALE sample only when a host steal lands inside the
 * sub-us CS -- a rare event CONDITIONAL on danger (given danger, the ~1us CS
 * overlaps the ms-scale steal with tiny probability p_cs_catch). p_cs_catch is
 * calibrated so the resulting counter reads EXACTLY 0 ~90% of holds and clears
 * threshold ~2.45% -- reproducing the live sparsity, which is the whole point.
 *
 * wait_decay is driven by GENUINE shared-lock contention (real waiters, real
 * spins), exactly as every prior file. Only the steal/protection half is
 * simulated (it must be: protection = a real vCPU migration that cannot happen
 * in userspace).
 *
 * COOLDOWN NOTE. The 50us per-vCPU ivh_eval_cooldown is NOT modelled in the
 * catch-rate numbers (matching every prior reference file, none of which had a
 * cooldown): danger persists for ms, the cooldown is 50us ~= 1/100th of a
 * danger window, so within any danger window a fresh eval re-detects it almost
 * immediately -- the cooldown cannot hide an ongoing danger. It IS accounted for
 * separately in the wasted-evaluation analysis (section 4), which is the only
 * place it bites.
 *
 * CONDITIONS:
 *   (a) NO GATE     Stage A always passes for eligible threads (baseline ~2.1%)
 *   (b) AND-GATE    Stage A = wait>WAIT && preempt>PREEMPT, preempt with the
 *                   REAL measured sparsity (must reproduce ~12% regression)
 *   (c) WAIT-ONLY   Stage A = wait>WAIT ; Stage B unchanged (the proposal)
 *
 * POPULATIONS:
 *   hot        NHextend-style sustained contention (long hold, no idle gap)
 *   cold       daemon-style (brief hold, real idle gap, rare contention)
 *   busy_safe  the ORIGINAL false-positive: single-owner heavily-used lock that
 *              NOBODY contends -> wait_decay never rises -> must stay excluded
 *
 * Build: gcc -O2 -o hotthreads_twostage_test hotthreads_twostage_test.c -lpthread -lm
 * Run:   ./hotthreads_twostage_test [hot_n] [measure_s] [sB_sens]
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

/* ---- project-standard fixed-point EWMA (identical to the reference files) ---- */
#define SCALE           1024
#define EWMA_K          3
#define WAIT_THRESH     512
static int preempt_thresh = 40;

/* ---- gate modes (Stage A) ---- */
enum gate_mode { MODE_NOGATE = 0, MODE_AND = 1, MODE_WAITONLY = 2 };

/* ---- steal / protection / danger model ---- */
static double p_danger  = 0.50;   /* per-hold P(vCPU in a host-steal window)    */
static double q_catch   = 0.246;  /* P(caught | danger, unprotected)            */
static double r_residual= 0.171;  /* residual catch factor after IVH migration  */
/* => ceiling = p_danger*q = 0.1230 (live regression); base = *r = 0.0210 (live base) */

/* Stage B = ivh_steal_imminent() (Gate 1+2), the real-time per-vCPU check. */
static double sB_sens   = 1.00;   /* P(detect | danger)  -- swept down for robustness */
static double sB_fpr    = 0.05;   /* P(false alarm | no danger) -- accounting only     */

/* preempt_decay feed: P(steal lands in the sub-us CS | danger). Tiny by nature;
 * calibrated below to reproduce the live 90.4% exactly-zero / 2.45% over-thresh. */
static double p_cs_catch_given_danger = 0.0060;

/* ---- per-thread state ---- */
enum pop { POP_HOT = 0, POP_COLD = 1, POP_BUSY = 2 };
struct tstate {
        int  id;
        enum pop pop;
        bool eligible;              /* PF_IVH_ELIGIBLE analogue (cold is not)    */

        atomic_int wait_decay;      /* EWMA, REAL contention driven              */
        atomic_int preempt_decay;   /* EWMA, rare-CS-steal driven (for MODE_AND) */

        unsigned int rng;

        /* measurement tallies */
        _Atomic unsigned long long acquires;
        _Atomic unsigned long long contended;
        _Atomic unsigned long long steal_holds;      /* GROUND-TRUTH catches      */
        _Atomic unsigned long long protected_holds;  /* migration engaged         */
        _Atomic unsigned long long stageA_pass;      /* reached the cooldown/StageB */
        _Atomic unsigned long long stageB_reached;   /* passed A -> ran Gate 1+2   */
        _Atomic unsigned long long stageB_declined;  /* WASTED eval (A pass, B no) */
        _Atomic unsigned long long preempt_zero;     /* preempt_decay read == 0    */
        _Atomic unsigned long long preempt_overthr;  /* preempt_decay read >  thr  */
};

static enum gate_mode g_mode;
static volatile int g_done = 0;
static volatile int g_measuring = 0;

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

/* ---- EWMA feeds (event-driven, write-time only -- kernel-faithful) ---- */
static void wait_decay_update(struct tstate *t, bool contended)
{
        int old = atomic_load(&t->wait_decay);
        int sample = SCALE;
        if (contended)   /* kernel-current: only the contended slowpath feeds SCALE */
                atomic_store(&t->wait_decay, old + ((sample - old) >> EWMA_K));
}
static void preempt_decay_update(struct tstate *t, bool cs_stolen)
{
        int old = atomic_load(&t->preempt_decay);
        int sample = cs_stolen ? SCALE : 0;
        atomic_store(&t->preempt_decay, old + ((sample - old) >> EWMA_K));
}

/* Stage A: the Hot Threads gate as it exists in ivh_pre_lock() lines 193-200. */
static bool stageA_pass(struct tstate *t)
{
        if (!t->eligible)
                return false;                    /* cold is never PF_IVH_ELIGIBLE */
        switch (g_mode) {
        case MODE_NOGATE:
                return true;                     /* no Hot Threads gate at all    */
        case MODE_AND:
                return atomic_load(&t->wait_decay) > WAIT_THRESH &&
                       atomic_load(&t->preempt_decay) > preempt_thresh;
        case MODE_WAITONLY:
                return atomic_load(&t->wait_decay) > WAIT_THRESH;
        }
        return false;
}

struct lockctx {
        volatile unsigned long lock __attribute__((aligned(4096)));
        atomic_int waiters;
};
static struct lockctx hot_ctx, cold_ctx;
static struct lockctx busy_ctx[64];   /* one PRIVATE lock per busy thread -> uncontended */

static unsigned long cmpxchg_ul(volatile unsigned long *p, unsigned long o, unsigned long n)
{
        unsigned long prev;
        asm volatile("lock; cmpxchg %2,%1" : "=a"(prev), "+m"(*p) : "r"(n), "0"(o) : "memory");
        return prev;
}

/* One acquire / hold / release, closing the two-stage feedback loop. */
static void locked_op(struct tstate *t, struct lockctx *c, int cs_iters)
{
        bool contended = atomic_load(&c->waiters) > 0;
        atomic_fetch_add(&c->waiters, 1);
        while (cmpxchg_ul(&c->lock, 0, 1) != 0)
                rmb();
        atomic_fetch_sub(&c->waiters, 1);

        wait_decay_update(t, contended);

        /* ---- Stage A: Hot Threads gate ---- */
        bool A = stageA_pass(t);

        /* critical section */
        for (int i = 0; i < cs_iters; i++)
                asm volatile ("sfence" ::: "memory");
        c->lock = 0;

        /* ---- latent danger + Stage B (Gate 1+2) ---- */
        bool danger = rnd01(&t->rng) < p_danger;
        bool B_detect = danger ? (rnd01(&t->rng) < sB_sens)
                               : (rnd01(&t->rng) < sB_fpr);
        bool protect = A && B_detect;    /* migration engages only if both agree  */

        /* ---- steal draw (ground truth) ---- */
        bool stolen = false;
        if (danger) {
                double pcatch = protect ? q_catch * r_residual : q_catch;
                stolen = rnd01(&t->rng) < pcatch;
        }

        /* ---- preempt_decay feed: rare CS-steal event, conditional on danger ---- */
        bool cs_stolen = danger && (rnd01(&t->rng) < p_cs_catch_given_danger);
        preempt_decay_update(t, cs_stolen);

        if (g_measuring) {
                atomic_fetch_add(&t->acquires, 1);
                if (contended) atomic_fetch_add(&t->contended, 1);
                if (stolen)    atomic_fetch_add(&t->steal_holds, 1);
                if (protect)   atomic_fetch_add(&t->protected_holds, 1);
                if (A)         atomic_fetch_add(&t->stageA_pass, 1);
                if (A)         atomic_fetch_add(&t->stageB_reached, 1);
                if (A && !B_detect) atomic_fetch_add(&t->stageB_declined, 1);
                /* preempt_decay read sparsity (the live-finding credibility check) */
                int pv = atomic_load(&t->preempt_decay);
                if (pv == 0)            atomic_fetch_add(&t->preempt_zero, 1);
                if (pv > preempt_thresh) atomic_fetch_add(&t->preempt_overthr, 1);
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
/* busy_safe: single-owner, heavily used, NEVER contended (private lock). */
static void *busy_thread(void *arg)
{
        struct tstate *t = arg;
        struct lockctx *mine = &busy_ctx[t->id % 64];
        while (!g_done)
                locked_op(t, mine, hot_cs_iters);   /* same heavy use as hot, but alone */
        return NULL;
}

struct cond_result {
        double hot_catch_pct, hot_protect_pct;
        double cold_catch_pct;
        double busy_catch_pct;
        int    cold_hot_verdict, busy_hot_verdict;
        /* Stage-B / wasted-eval accounting (hot pop) */
        double hot_stageB_reach_pct;   /* of hot acquires, how many ran Gate 1+2 */
        double hot_wasted_pct;         /* of stage-A passes, how many B declined  */
        /* preempt sparsity (hot pop), the live-finding credibility check         */
        double hot_preempt_zero_pct, hot_preempt_overthr_pct;
};

static void init_pop(struct tstate *a, int n, enum pop pop, bool eligible, unsigned seed0)
{
        memset(a, 0, sizeof(struct tstate) * n);
        for (int i = 0; i < n; i++) {
                a[i].id = i; a[i].pop = pop; a[i].eligible = eligible;
                a[i].rng = seed0 ^ (0x9e3779b9u * (i + 1));
        }
}

static struct cond_result run_condition(enum gate_mode mode, int hot_n, int cold_n, int busy_n,
                                         int warmup_s, int measure_s)
{
        struct tstate hot[64], cold[16], busy[64];
        pthread_t htid[64], ctid[16], btid[64];
        if (hot_n > 64) hot_n = 64;
        if (cold_n > 16) cold_n = 16;
        if (busy_n > 64) busy_n = 64;

        init_pop(hot,  hot_n,  POP_HOT,  true,  0xC0FFEEu);
        init_pop(cold, cold_n, POP_COLD, false, 0x1234567u);   /* cold: not eligible */
        init_pop(busy, busy_n, POP_BUSY, true,  0xB16B00Bu);   /* busy: eligible but uncontended */

        g_mode = mode; g_done = 0; g_measuring = 0;
        hot_ctx.lock = 0; atomic_store(&hot_ctx.waiters, 0);
        cold_ctx.lock = 0; atomic_store(&cold_ctx.waiters, 0);
        for (int i = 0; i < 64; i++) { busy_ctx[i].lock = 0; atomic_store(&busy_ctx[i].waiters, 0); }

        for (int i = 0; i < hot_n; i++)  pthread_create(&htid[i], NULL, hot_thread,  &hot[i]);
        for (int i = 0; i < cold_n; i++) pthread_create(&ctid[i], NULL, cold_thread, &cold[i]);
        for (int i = 0; i < busy_n; i++) pthread_create(&btid[i], NULL, busy_thread, &busy[i]);

        for (int s = 0; s < warmup_s; s++) sleep(1);
        g_measuring = 1;
        for (int s = 0; s < measure_s; s++) sleep(1);
        g_done = 1;

        for (int i = 0; i < hot_n; i++)  pthread_join(htid[i], NULL);
        for (int i = 0; i < cold_n; i++) pthread_join(ctid[i], NULL);
        for (int i = 0; i < busy_n; i++) pthread_join(btid[i], NULL);

        struct cond_result r; memset(&r, 0, sizeof(r));
        unsigned long long acq=0, steal=0, prot=0, Apass=0, Breach=0, Bdecl=0, pz=0, po=0;
        for (int i = 0; i < hot_n; i++) {
                acq   += atomic_load(&hot[i].acquires);
                steal += atomic_load(&hot[i].steal_holds);
                prot  += atomic_load(&hot[i].protected_holds);
                Apass += atomic_load(&hot[i].stageA_pass);
                Breach+= atomic_load(&hot[i].stageB_reached);
                Bdecl += atomic_load(&hot[i].stageB_declined);
                pz    += atomic_load(&hot[i].preempt_zero);
                po    += atomic_load(&hot[i].preempt_overthr);
        }
        r.hot_catch_pct       = acq ? 100.0*steal/acq : 0;
        r.hot_protect_pct     = acq ? 100.0*prot /acq : 0;
        r.hot_stageB_reach_pct= acq ? 100.0*Breach/acq: 0;
        r.hot_wasted_pct      = Apass ? 100.0*Bdecl/Apass : 0;
        r.hot_preempt_zero_pct= acq ? 100.0*pz/acq : 0;
        r.hot_preempt_overthr_pct = acq ? 100.0*po/acq : 0;

        unsigned long long cacq=0, csteal=0;
        for (int i = 0; i < cold_n; i++) {
                cacq   += atomic_load(&cold[i].acquires);
                csteal += atomic_load(&cold[i].steal_holds);
                if (stageA_pass(&cold[i])) r.cold_hot_verdict++;
        }
        r.cold_catch_pct = cacq ? 100.0*csteal/cacq : 0;

        unsigned long long bacq=0, bsteal=0;
        for (int i = 0; i < busy_n; i++) {
                bacq   += atomic_load(&busy[i].acquires);
                bsteal += atomic_load(&busy[i].steal_holds);
                if (stageA_pass(&busy[i])) r.busy_hot_verdict++;
        }
        r.busy_catch_pct = bacq ? 100.0*bsteal/bacq : 0;
        return r;
}

static const char *mode_name(enum gate_mode m)
{
        return m == MODE_NOGATE   ? "(a) NO GATE            " :
               m == MODE_AND      ? "(b) AND wait+preempt   " :
                                    "(c) WAIT-ONLY + Gate1+2";
}

int main(int argc, char **argv)
{
        int hot_n     = argc > 1 ? atoi(argv[1]) : 16;
        int measure_s = argc > 2 ? atoi(argv[2]) : 8;
        if (argc > 3) sB_sens = atof(argv[3]);
        int cold_n = 4, busy_n = 4, warmup_s = 3;

        double ceiling = p_danger * q_catch;
        double base    = ceiling * r_residual;

        printf("Hot Threads TWO-STAGE (wait-only + downstream Gate 1+2) validation\n");
        printf("SCALE=%d k=%d WAIT_THRESH=%d PREEMPT_THRESH=%d\n",
               SCALE, EWMA_K, WAIT_THRESH, preempt_thresh);
        printf("model: p_danger=%.2f q_catch=%.3f r_residual=%.3f "
               "sB_sens=%.2f sB_fpr=%.2f p_cs_catch|danger=%.4f\n",
               p_danger, q_catch, r_residual, sB_sens, sB_fpr, p_cs_catch_given_danger);
        printf("       => analytic ceiling(unprot)=%.2f%%  baseline(prot)=%.2f%%\n",
               100*ceiling, 100*base);
        printf("hot_n=%d cold_n=%d busy_safe_n=%d warmup=%ds measure=%ds\n\n",
               hot_n, cold_n, busy_n, warmup_s, measure_s);

        struct cond_result a = run_condition(MODE_NOGATE,   hot_n, cold_n, busy_n, warmup_s, measure_s);
        struct cond_result b = run_condition(MODE_AND,      hot_n, cold_n, busy_n, warmup_s, measure_s);
        struct cond_result c = run_condition(MODE_WAITONLY, hot_n, cold_n, busy_n, warmup_s, measure_s);

        printf("=================================== THREE CONDITIONS ===================================\n");
        printf("%-24s %10s %10s %11s %11s %10s %10s\n",
               "condition", "HOTcatch%", "HOTprot%", "COLDverdict", "BUSYverdict", "COLDcatch%", "BUSYcatch%");
        printf("%-24s %9.2f%% %9.2f%% %8d/%-2d %8d/%-2d %9.2f%% %9.2f%%\n", mode_name(MODE_NOGATE),
               a.hot_catch_pct, a.hot_protect_pct, a.cold_hot_verdict, cold_n, a.busy_hot_verdict, busy_n,
               a.cold_catch_pct, a.busy_catch_pct);
        printf("%-24s %9.2f%% %9.2f%% %8d/%-2d %8d/%-2d %9.2f%% %9.2f%%\n", mode_name(MODE_AND),
               b.hot_catch_pct, b.hot_protect_pct, b.cold_hot_verdict, cold_n, b.busy_hot_verdict, busy_n,
               b.cold_catch_pct, b.busy_catch_pct);
        printf("%-24s %9.2f%% %9.2f%% %8d/%-2d %8d/%-2d %9.2f%% %9.2f%%\n", mode_name(MODE_WAITONLY),
               c.hot_catch_pct, c.hot_protect_pct, c.cold_hot_verdict, cold_n, c.busy_hot_verdict, busy_n,
               c.cold_catch_pct, c.busy_catch_pct);

        printf("\n--- CREDIBILITY: (b) must reproduce the live regression before (c) is trusted ---\n");
        printf("  (b) preempt_decay read EXACTLY 0: %.1f%% of holds   (live bpftrace: 90.4%%)\n",
               b.hot_preempt_zero_pct);
        printf("  (b) preempt_decay read >thresh:   %.2f%% of holds   (live bpftrace: 2.45%%)\n",
               b.hot_preempt_overthr_pct);
        printf("  (b) HOT catch = %.2f%%  vs baseline (a) = %.2f%%   [%.1fx]  -> live was 11.95-12.58%% vs 1.8-2.4%%\n",
               b.hot_catch_pct, a.hot_catch_pct,
               a.hot_catch_pct > 0 ? b.hot_catch_pct/a.hot_catch_pct : 0);
        bool cred = b.hot_catch_pct > 3.0 * a.hot_catch_pct &&
                    b.hot_preempt_zero_pct > 80.0;
        printf("  credibility: %s\n", cred ? "PASS (sim reflects the live regression)"
                                           : "FAIL (do not trust (c))");

        printf("\n--- PROPOSAL VERDICT: (c) wait-only + downstream Gate 1+2 ---\n");
        bool restored = c.hot_catch_pct <= 1.5 * a.hot_catch_pct;
        printf("  (c) HOT catch = %.2f%%  vs baseline (a) = %.2f%%   -> restores baseline: %s\n",
               c.hot_catch_pct, a.hot_catch_pct, restored ? "YES" : "NO");
        bool fp_ok = c.busy_hot_verdict == 0 && c.cold_hot_verdict == 0;
        printf("  false positives excluded under (c): busy_safe=%d/%d  cold=%d/%d  -> %s\n",
               c.busy_hot_verdict, busy_n, c.cold_hot_verdict, cold_n, fp_ok ? "YES" : "NO");

        printf("\n--- WASTED-EVALUATION COST (the honest downside of dropping preempt_decay) ---\n");
        printf("  Under (c), of hot acquisitions %.1f%% pass Stage A and run Gate 1+2 (Stage B).\n",
               c.hot_stageB_reach_pct);
        printf("  Of those Stage-A passes, %.1f%% are DECLINED by Gate 1+2 (contended-but-currently-safe)\n",
               c.hot_wasted_pct);
        printf("  = wasted EVALUATIONS (Gate 1+2 arithmetic), NOT wasted migrations.\n");
        printf("  Under (b) only %.1f%% of hot acquisitions reach Stage B at all (preempt gate ~stuck-closed),\n",
               b.hot_stageB_reach_pct);
        printf("  so (c) increases Stage-B entries ~%.0fx -- but each is rate-limited to <=1 per 50us per vCPU\n",
               b.hot_stageB_reach_pct > 0 ? c.hot_stageB_reach_pct/b.hot_stageB_reach_pct : 0);
        printf("  by ivh_eval_cooldown_ok(); danger persists ~5-6ms >> 50us, so a wasted eval delays\n");
        printf("  re-detection of a NEW danger by at most 50us (~1%% of a danger window). See report.\n");

        printf("\nDONE.\n");
        return 0;
}
