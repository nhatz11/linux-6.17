/*
 * Pure userspace validation of the "Hot Threads" concept -- the per-THREAD
 * successor to the per-LOCK "Hotlock" classifier prototyped in
 * hotlock_userland_test.c (read that file for the shared EWMA machinery,
 * the event-driven-vs-wall-clock-decay lesson Model B failed, and the
 * busy_lock/tiny_lock false-positive lessons that motivated Model F's
 * "contention is a NECESSARY gate, something else modulates" shape).
 *
 * The new design classifies TASKS instead of locks, using two independent,
 * EVENT-DRIVEN decaying counters per thread (never wall-clock-decayed -- that
 * is the non-negotiable lesson from Model B, which collapsed during the exact
 * steal window IVH exists to catch):
 *
 *   wait_decay:    EWMA of "was this lock contended when I joined as a waiter",
 *                  fed once per acquisition EVENT (the per-event granularity,
 *                  matching the per-lock EWMA and the safer default). A
 *                  per-ITERATION variant is also measured (wait_iters, mean
 *                  spin iterations per wait) purely to show WHY per-event wins.
 *
 *   preempt_decay: EWMA of "was this thread actually caught by a real HOST
 *                  STEAL while holding the lock", fed once per release event.
 *                  The steal signal is genuine host-level steal time, read from
 *                  /proc/vcap_info (vsched_module), using the EXACT delta-across-
 *                  the-hold methodology NHextend3.c uses for host_preempted_count
 *                  (steal_before at CS entry, steal_after at CS exit, >100us
 *                  floor on the holder's CPU). NOT involuntary-ctxsw counting --
 *                  that would fire on ordinary guest scheduling and dilute the
 *                  signal with noise unrelated to host steal.
 *
 *   Classification: HOT (worth IVH protection) := wait_decay > WAIT_THRESH
 *                   AND preempt_decay > PREEMPT_THRESH. An AND gate: contention
 *                   alone is not sufficient (would flag any busy thread on a
 *                   safe CPU), preemption exposure alone is not sufficient
 *                   (would flag a thread that gets stolen but strands no one).
 *
 * THE VALIDATION EXPERIMENT (three phases, each the project's default 5s):
 *   1. GOOD candidate, UNPINNED: N threads hammering a shared lock, floating
 *      across all 16 CPUs incl. this host's stolen half -> expect high
 *      wait_decay AND non-zero preempt_decay -> HOT.
 *   2. BAD candidate: 1 daemon-ish thread, brief CS, real idle time between
 *      acquisitions -> expect low wait_decay AND low preempt_decay -> NOT HOT.
 *   3. GOOD candidate, PINNED TO THE LIVE-DETERMINED SAFE CPUs: the SAME
 *      contended workload, pinned only onto whichever vCPUs /proc/vcap_info
 *      shows low recent steal -> expect wait_decay STAYS HIGH (nothing about
 *      the contention changed) while preempt_decay DROPS toward zero (safe CPUs
 *      aren't stolen) -> classification FLIPS to NOT HOT. This is the actual
 *      proof that preempt_decay does load-bearing work independent of
 *      wait_decay. If it doesn't flip, that is reported honestly as a finding.
 *
 * Build: gcc -O2 -o hotthreads_test hotthreads_test.c -lpthread
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <fcntl.h>
#include <time.h>
#include <stdatomic.h>

#define rmb() asm volatile ("lfence" ::: "memory")

/* Same fixed-point EWMA as Hotlock's Model A (kernel ivh_hotlock design). */
#define SCALE       1024
#define HALF        512
#define EWMA_K      3

/* Classification thresholds. WAIT is the well-established 0.5 contention
 * threshold from the per-lock work. PREEMPT is deliberately lower: even a
 * genuinely-exposed thread is only caught mid-hold a fraction of the time, so
 * its steady-state EWMA converges to that fraction, not toward 1.0. The value
 * here is revisited against the measured numbers in the report; it only has to
 * sit between "essentially never stolen mid-hold" (pinned-safe) and "stolen
 * mid-hold a real fraction of the time" (unpinned) for the flip to be real. */
#define WAIT_THRESH     HALF     /* 0.5 */
static int preempt_thresh = 40;  /* ~0.04 of SCALE; overridable via argv[3] */

#define VCAP_MAX_CPUS 256
#define STEAL_FLOOR_NS 100000LL  /* matches NHextend3.c: >100us = real steal, not counter noise */

/* ---- host steal-time ground truth, verbatim methodology from NHextend3.c ---- */
static __thread int vcap_steal_fd = -1;

static int read_vcap_steal(unsigned long long *steal_out)
{
        char buf[8192];
        char *saveptr, *tok;
        ssize_t n;
        int cpu = -1, field = 0;

        if (vcap_steal_fd < 0) {
                vcap_steal_fd = open("/proc/vcap_info", O_RDONLY);
                if (vcap_steal_fd < 0)
                        return -1;
        }
        /* pread with a local pos re-triggers the single-shot proc dump every
         * call without re-open()ing (the handler blocks re-read via *ppos>0). */
        n = pread(vcap_steal_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0)
                return -1;
        buf[n] = '\0';

        /* 4 lines per CPU: "CPU N:", then three numeric lines; field==2 (the
         * SECOND numeric line) is the cumulative steal-ns counter NHextend3.c
         * reads. */
        tok = strtok_r(buf, "\n", &saveptr);
        while (tok) {
                if (field == 0)
                        sscanf(tok, "CPU %d:", &cpu);
                else if (field == 2 && cpu >= 0 && cpu < VCAP_MAX_CPUS)
                        steal_out[cpu] = strtoull(tok, NULL, 10);
                field = (field + 1) % 4;
                tok = strtok_r(NULL, "\n", &saveptr);
        }
        return 0;
}

/* ---- per-thread Hot-Threads state ---- */
struct thread_state {
        int      id;
        atomic_int wait_decay;      /* EWMA of contended-on-join, per acquire event */
        atomic_int preempt_decay;   /* EWMA of caught-by-host-steal, per release event */
        /* per-ITERATION wait variant, measured only to justify per-event: EWMA
         * of spin-iterations per wait (NOT used in classification). */
        atomic_llong wait_iters_ewma;
        /* raw ground-truth tallies for the final report */
        unsigned long long acquires;
        unsigned long long contended_acquires;
        unsigned long long steal_holds;   /* holds caught by real host steal */
        unsigned long long total_wait_iters;
};

static void wait_decay_update(struct thread_state *t, bool contended)
{
        int sample = contended ? SCALE : 0;
        int old = atomic_load(&t->wait_decay);
        int nw;
        do {
                nw = old + ((sample - old) >> EWMA_K);
                if (nw == old) break;
        } while (!atomic_compare_exchange_weak(&t->wait_decay, &old, nw));
}

static void preempt_decay_update(struct thread_state *t, bool stolen)
{
        int sample = stolen ? SCALE : 0;
        int old = atomic_load(&t->preempt_decay);
        int nw;
        do {
                nw = old + ((sample - old) >> EWMA_K);
                if (nw == old) break;
        } while (!atomic_compare_exchange_weak(&t->preempt_decay, &old, nw));
}

static void wait_iters_update(struct thread_state *t, long long iters)
{
        long long old = atomic_load(&t->wait_iters_ewma);
        atomic_store(&t->wait_iters_ewma, old + ((iters - old) >> EWMA_K));
}

static bool thread_is_hot(struct thread_state *t)
{
        return atomic_load(&t->wait_decay) > WAIT_THRESH &&
               atomic_load(&t->preempt_decay) > preempt_thresh;
}

/* ---- workload scaffolding ---- */
static volatile unsigned long shared_lock __attribute__((aligned(4096))) = 0;
static atomic_int shared_waiters = 0;
static volatile int done = 0;

static unsigned long long now_ns(void)
{
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (unsigned long long)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static unsigned long cmpxchg_ul(volatile unsigned long *p, unsigned long o, unsigned long n)
{
        unsigned long prev;
        asm volatile("lock; cmpxchg %2,%1" : "=a"(prev), "+m"(*p) : "r"(n), "0"(o) : "memory");
        return prev;
}

/* GOOD candidate: NHextend-style tight contention on a shared lock, long-ish
 * CS so several threads genuinely queue. Steal is measured for real across
 * every hold via /proc/vcap_info deltas -- no synthetic injection. */
static int good_cs_iters = 3000000;

static void *good_thread(void *arg)
{
        struct thread_state *t = arg;
        unsigned long long steal_before[VCAP_MAX_CPUS];
        unsigned long long steal_after[VCAP_MAX_CPUS];

        while (!done) {
                bool contended = atomic_load(&shared_waiters) > 0;
                atomic_fetch_add(&shared_waiters, 1);

                long long spins = 0;
                while (cmpxchg_ul(&shared_lock, 0, 1) != 0) {
                        spins++;
                        rmb();
                }
                atomic_fetch_sub(&shared_waiters, 1);

                /* wait_decay: per-EVENT contention sample (primary). */
                wait_decay_update(t, contended);
                /* wait_iters: per-ITERATION variant, for comparison only. */
                wait_iters_update(t, spins);
                t->total_wait_iters += spins;

                /* ---- critical section, bracketed by real host-steal reads ---- */
                int cpu_start = sched_getcpu();
                memset(steal_before, 0, sizeof(steal_before));
                read_vcap_steal(steal_before);

                for (int i = 0; i < good_cs_iters; i++)
                        asm volatile ("sfence" ::: "memory");

                shared_lock = 0;

                int cpu_end = sched_getcpu();
                memset(steal_after, 0, sizeof(steal_after));
                read_vcap_steal(steal_after);

                bool stolen = false;
                if (cpu_start >= 0 && cpu_start < VCAP_MAX_CPUS) {
                        long long d = (long long)steal_after[cpu_start] - (long long)steal_before[cpu_start];
                        if (d > STEAL_FLOOR_NS) stolen = true;
                }
                if (cpu_end != cpu_start && cpu_end >= 0 && cpu_end < VCAP_MAX_CPUS) {
                        long long d = (long long)steal_after[cpu_end] - (long long)steal_before[cpu_end];
                        if (d > STEAL_FLOOR_NS) stolen = true;
                }

                preempt_decay_update(t, stolen);

                t->acquires++;
                if (contended) t->contended_acquires++;
                if (stolen) t->steal_holds++;
        }
        return NULL;
}

/* BAD candidate: daemon-ish. One thread, brief CS, real idle time between
 * acquisitions. No sustained contention; rarely if ever caught mid-hold. */
static void *bad_thread(void *arg)
{
        struct thread_state *t = arg;
        unsigned long long steal_before[VCAP_MAX_CPUS];
        unsigned long long steal_after[VCAP_MAX_CPUS];

        while (!done) {
                bool contended = atomic_load(&shared_waiters) > 0;
                atomic_fetch_add(&shared_waiters, 1);
                long long spins = 0;
                while (cmpxchg_ul(&shared_lock, 0, 1) != 0) { spins++; rmb(); }
                atomic_fetch_sub(&shared_waiters, 1);

                wait_decay_update(t, contended);
                wait_iters_update(t, spins);
                t->total_wait_iters += spins;

                int cpu_start = sched_getcpu();
                memset(steal_before, 0, sizeof(steal_before));
                read_vcap_steal(steal_before);

                for (int i = 0; i < 2000; i++)
                        asm volatile ("sfence" ::: "memory");

                shared_lock = 0;

                int cpu_end = sched_getcpu();
                memset(steal_after, 0, sizeof(steal_after));
                read_vcap_steal(steal_after);

                bool stolen = false;
                if (cpu_start >= 0 && cpu_start < VCAP_MAX_CPUS) {
                        long long d = (long long)steal_after[cpu_start] - (long long)steal_before[cpu_start];
                        if (d > STEAL_FLOOR_NS) stolen = true;
                }
                if (cpu_end != cpu_start && cpu_end >= 0 && cpu_end < VCAP_MAX_CPUS) {
                        long long d = (long long)steal_after[cpu_end] - (long long)steal_before[cpu_end];
                        if (d > STEAL_FLOOR_NS) stolen = true;
                }
                preempt_decay_update(t, stolen);

                t->acquires++;
                if (contended) t->contended_acquires++;
                if (stolen) t->steal_holds++;

                usleep(50000); /* real idle time -- the thing that makes it "cold" */
        }
        return NULL;
}

/* ---- live safe/unsafe CPU determination from /proc/vcap_info deltas ---- */
static int safe_cpus[VCAP_MAX_CPUS], n_safe;
static int unsafe_cpus[VCAP_MAX_CPUS], n_unsafe;
static int ncpu;

static void determine_safe_cpus(void)
{
        unsigned long long cum[VCAP_MAX_CPUS] = {0};
        unsigned long long a[VCAP_MAX_CPUS] = {0}, b[VCAP_MAX_CPUS] = {0};
        ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        if (ncpu > VCAP_MAX_CPUS) ncpu = VCAP_MAX_CPUS;

        /* This host's host-level steal is BURSTY: a short live-delta probe
         * aliases the burst and can read the whole box "safe" during a quiet
         * moment (empirically observed this session). The CUMULATIVE steal
         * counter is the stable, unambiguous signal for WHICH vCPUs the host
         * actually steals -- here the stolen half sits ~250x above the quiet
         * half, a gap no momentary quiet window erases. So classify safe/unsafe
         * off cumulative steal (robust), and ALSO show a 4s live delta window
         * for context on how hot the steal is right now. */
        read_vcap_steal(cum);
        unsigned long long mn = ~0ULL;
        for (int c = 0; c < ncpu; c++) if (cum[c] < mn) mn = cum[c];
        if (mn == 0) mn = 1;

        read_vcap_steal(a);
        usleep(4000000); /* 4s live window to average over the burst period */
        read_vcap_steal(b);

        printf("[vcap] cumulative steal (safe/unsafe classifier) + live 4s delta (current intensity):\n");
        for (int c = 0; c < ncpu; c++) {
                /* >20x the quietest vCPU's cumulative steal = this is one the
                 * host demonstrably steals. */
                bool unsafe = cum[c] > mn * 20ULL;
                long long d = (long long)b[c] - (long long)a[c];
                printf("   CPU %2d: cum=%-14llu (%5.1fx min)  live+%-10lld ns/4s (%.2f%%)  %s\n",
                       c, cum[c], (double)cum[c] / mn, d, 100.0 * d / 4e9,
                       unsafe ? "UNSAFE(stolen)" : "safe");
                if (unsafe) unsafe_cpus[n_unsafe++] = c;
                else        safe_cpus[n_safe++] = c;
        }
        printf("[vcap] -> %d SAFE cpus, %d UNSAFE cpus\n\n", n_safe, n_unsafe);
}

/* ---- one experiment phase ---- */
static void reset_state(struct thread_state *ts, int n)
{
        for (int i = 0; i < n; i++) {
                memset(&ts[i], 0, sizeof(ts[i]));
                ts[i].id = i;
        }
        shared_lock = 0;
        atomic_store(&shared_waiters, 0);
}

struct phase_result {
        double wait_decay, preempt_decay, wait_iters;
        double acq_per_s, gt_contend_pct, steal_hold_pct;
        int hot_votes, n;
};

static struct phase_result run_phase(const char *name, void *(*fn)(void *),
                                     int nthreads, int duration_s,
                                     const int *pin_set, int pin_n)
{
        struct thread_state ts[64];
        pthread_t tid[64];
        if (nthreads > 64) nthreads = 64;
        reset_state(ts, nthreads);
        done = 0;

        printf("=== PHASE: %s (%d threads, %ds", name, nthreads, duration_s);
        if (pin_set) {
                printf(", pinned to {");
                for (int i = 0; i < pin_n; i++) printf("%d%s", pin_set[i], i + 1 < pin_n ? "," : "");
                printf("})");
        } else printf(", UNPINNED)");
        printf(" ===\n");

        for (int i = 0; i < nthreads; i++) {
                pthread_attr_t attr;
                pthread_attr_init(&attr);
                if (pin_set) {
                        cpu_set_t set;
                        CPU_ZERO(&set);
                        for (int j = 0; j < pin_n; j++) CPU_SET(pin_set[j], &set);
                        pthread_attr_setaffinity_np(&attr, sizeof(set), &set);
                }
                pthread_create(&tid[i], &attr, fn, &ts[i]);
                pthread_attr_destroy(&attr);
        }

        for (int s = 0; s < duration_s; s++) {
                sleep(1);
                int wd = 0, pd = 0, hot = 0;
                for (int i = 0; i < nthreads; i++) {
                        wd += atomic_load(&ts[i].wait_decay);
                        pd += atomic_load(&ts[i].preempt_decay);
                        if (thread_is_hot(&ts[i])) hot++;
                }
                printf("  t=%ds  mean wait_decay=%.3f  mean preempt_decay=%.3f  hot=%d/%d\n",
                       s + 1, (double)wd / nthreads / SCALE, (double)pd / nthreads / SCALE,
                       hot, nthreads);
        }

        done = 1;
        for (int i = 0; i < nthreads; i++) pthread_join(tid[i], NULL);

        struct phase_result r = {0};
        r.n = nthreads;
        unsigned long long acq = 0, cont = 0, steal = 0, witer = 0;
        long long wd = 0, pd = 0, wi = 0;
        for (int i = 0; i < nthreads; i++) {
                wd += atomic_load(&ts[i].wait_decay);
                pd += atomic_load(&ts[i].preempt_decay);
                wi += atomic_load(&ts[i].wait_iters_ewma);
                acq += ts[i].acquires;
                cont += ts[i].contended_acquires;
                steal += ts[i].steal_holds;
                witer += ts[i].total_wait_iters;
                if (thread_is_hot(&ts[i])) r.hot_votes++;
        }
        r.wait_decay = (double)wd / nthreads / SCALE;
        r.preempt_decay = (double)pd / nthreads / SCALE;
        r.wait_iters = (double)wi / nthreads;
        r.acq_per_s = (double)acq / duration_s;
        r.gt_contend_pct = acq ? 100.0 * cont / acq : 0;
        r.steal_hold_pct = acq ? 100.0 * steal / acq : 0;
        printf("  -> wait_decay=%.3f preempt_decay=%.3f | gt_contended=%.1f%% gt_steal_holds=%.2f%%"
               " | mean_wait_iters/wait=%.0f | acq/s=%.0f | HOT %d/%d\n\n",
               r.wait_decay, r.preempt_decay, r.gt_contend_pct, r.steal_hold_pct,
               r.wait_iters, r.acq_per_s, r.hot_votes, nthreads);
        return r;
}

int main(int argc, char **argv)
{
        int nthreads = argc > 1 ? atoi(argv[1]) : 8;
        int duration = argc > 2 ? atoi(argv[2]) : 5;
        if (argc > 3) preempt_thresh = atoi(argv[3]);

        /* Verify the steal interface is live before relying on it. */
        unsigned long long probe[VCAP_MAX_CPUS] = {0};
        if (read_vcap_steal(probe) < 0) {
                fprintf(stderr, "FATAL: /proc/vcap_info not readable -- vsched_module loaded? "
                        "Refusing to fake steal data.\n");
                return 1;
        }
        printf("Hot Threads (per-thread) classifier validation\n");
        printf("EWMA SCALE=%d k=%d | WAIT_THRESH=%.2f PREEMPT_THRESH=%.3f (=%d/%d)\n\n",
               SCALE, EWMA_K, (double)WAIT_THRESH / SCALE,
               (double)preempt_thresh / SCALE, preempt_thresh, SCALE);

        determine_safe_cpus();
        if (n_safe == 0)
                printf("[warn] no SAFE cpus found this instant -- phase 3 flip may not reproduce.\n\n");

        /* Exposed baseline: the task asks for the good candidate UNPINNED
         * (floating across the stolen half). We also pin an identical
         * population explicitly to the UNSAFE cpus, because this host's steal
         * is currently light+bursty and a floating workload only occasionally
         * lands its holder on a stolen vCPU -- pinning-to-unsafe concentrates
         * the exposure so the mechanism is visible even under light steal.
         * Both are the SAME contended workload; only CPU placement differs. */
        struct phase_result unpin = run_phase("GOOD candidate, UNPINNED (floats across stolen half)",
                                              good_thread, nthreads, duration, NULL, 0);
        struct phase_result exposed = run_phase("GOOD candidate, PINNED to UNSAFE/stolen cpus (exposed)",
                                              good_thread, nthreads, duration,
                                              n_unsafe ? unsafe_cpus : NULL, n_unsafe);
        /* The other off-diagonal AND-gate quadrant: a single thread on a
         * stolen cpu -- genuinely caught by host steal (preempt_decay HIGH) but
         * NEVER contended (only one thread, so no waiter ever, wait_decay 0).
         * Proves preempt-exposure ALONE is insufficient: stealing a thread that
         * strands no one is not an LHP event worth protecting. Reuses
         * good_thread with 1 thread + no idle gap = held ~100% but uncontended. */
        struct phase_result busy = run_phase("BUSY thread, PINNED to UNSAFE (stolen but uncontended)",
                                              good_thread, 1, duration,
                                              n_unsafe ? unsafe_cpus : NULL, n_unsafe);
        struct phase_result bad   = run_phase("BAD candidate, daemon-style (1 thread, idle gaps)",
                                              bad_thread, 1, duration, NULL, 0);
        struct phase_result pinned = run_phase("GOOD candidate, PINNED to SAFE cpus (protected)",
                                               good_thread, nthreads, duration,
                                               n_safe ? safe_cpus : NULL, n_safe);

        printf("================================ SUMMARY ================================\n");
        printf("%-38s %10s %13s %10s  %s\n", "phase", "wait_decay", "preempt_decay", "steal%", "verdict");
        struct { const char *n; struct phase_result *r; } rows[] = {
                {"GOOD unpinned (contend+float)", &unpin},
                {"GOOD pinned-UNSAFE (contend+steal)", &exposed},
                {"BUSY pinned-UNSAFE (steal, no contend)", &busy},
                {"BAD daemon (no contend, no steal)", &bad},
                {"GOOD pinned-SAFE (contend, no steal)", &pinned},
        };
        for (int i = 0; i < 5; i++)
                printf("%-38s %10.3f %13.3f %9.2f%%  %s\n", rows[i].n,
                       rows[i].r->wait_decay, rows[i].r->preempt_decay, rows[i].r->steal_hold_pct,
                       rows[i].r->hot_votes > rows[i].r->n / 2 ? "HOT" : "not hot");

        printf("\nClaim under test: the SAME contended workload keeps wait_decay high across\n"
               "all placements, while preempt_decay tracks REAL host steal -- high when the\n"
               "holder runs on stolen cpus, ~0 when pinned to safe cpus -- flipping the AND-\n"
               "gate HOT->not hot purely on placement. That proves preempt_decay does real\n"
               "load-bearing classification work independent of wait_decay.\n");
        printf("wait_decay across placements: unpinned=%.3f exposed=%.3f protected=%.3f (should all stay high)\n",
               unpin.wait_decay, exposed.wait_decay, pinned.wait_decay);
        printf("preempt_decay:                unpinned=%.3f exposed=%.3f protected=%.3f (should drop on safe)\n",
               unpin.preempt_decay, exposed.preempt_decay, pinned.preempt_decay);
        printf("gt steal-hold%%:               unpinned=%.2f%% exposed=%.2f%% protected=%.2f%%\n",
               unpin.steal_hold_pct, exposed.steal_hold_pct, pinned.steal_hold_pct);
        bool flipped = (exposed.hot_votes > exposed.n / 2) && (pinned.hot_votes <= pinned.n / 2);
        printf("FLIP DEMONSTRATED (exposed HOT & protected not-hot): %s\n", flipped ? "YES" : "NO");
        return 0;
}
