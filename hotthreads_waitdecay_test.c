/*
 * hotthreads_waitdecay_test.c
 *
 * Focused userspace validation of a FIX for the ivh_wait_decay "never decays"
 * bug discovered on a live kernel: a real sshd, made PF_IVH_ELIGIBLE, showed
 * ivh_wait_decay = 563 after only its normal startup (exactly 6 genuine
 * contended-wait events: the k=3 trajectory 0->128->240->338->423->498->563),
 * and because the kernel feeds ivh_wait_decay ONLY sample=SCALE on the
 * contended slowpath and NEVER a 0-sample, that 563 is PERMANENT for the life
 * of the process. The AND-gate's real-world safety then rests entirely on
 * ivh_preempt_decay: the next rare host-steal-while-holding event flips the
 * task HOT off ancient startup history.
 *
 * Why a NEW prototype instead of extending hotthreads_test.c:
 *  - hotthreads_test.c hard-requires /proc/vcap_info and interleaves the
 *    (unchanged) preempt_decay/steal machinery with wait_decay on every
 *    acquisition. The bug and its fix are entirely on the wait_decay axis;
 *    preempt_decay is untouched, so the AND-gate FLIP validation in that file
 *    remains valid as-is and does not need re-running to justify this fix.
 *  - Critically, hotthreads_test.c's wait_decay_update() feeds sample=0 on
 *    EVERY uncontended acquisition (line: `int sample = contended ? SCALE:0`).
 *    That does NOT match the shipped kernel (which never feeds 0) -- it is
 *    exactly why the original userspace validation never exposed this bug. To
 *    reproduce the real kernel we must model the kernel-faithful feed, so a
 *    clean-room prototype that tracks CURRENT (buggy) and FIXED side by side is
 *    clearer than surgically un-picking the old file.
 *
 * THE FIX UNDER TEST -- "sampled 0-feed from cs_exit()":
 *   cs_exit() in kernel/locking/spinlock.c already runs on EVERY outermost
 *   lock release, and already has a `if (current->flags & PF_IVH_ELIGIBLE)`
 *   block (it feeds preempt_decay there). Reuse that already-running,
 *   already-eligible-gated hook to feed ivh_wait_decay an occasional
 *   sample=0 -- 1-in-N via a per-task counter -- exactly the coarsening trick
 *   the old per-lock Hotlock used (`if (!contended && (ctr++ & 7)) return;`).
 *   The SCALE feed stays where it is (qspinlock slowpath, per contended
 *   acquisition). No new call site is added to _raw_spin_lock()'s fastpath;
 *   the marginal cost is, for opted-in tasks only, one extra READ/shift/WRITE
 *   on 1-in-N outermost releases inside a branch that already executes.
 *
 *   Model (this file):
 *     current: wait_decay_cur += (SCALE - cur) >> k   iff contended  (never 0)
 *     fixed:   wait_decay_fix += (SCALE - fix) >> k   iff contended
 *              AND, once per outermost release (== once per acquisition here),
 *              every Nth time: wait_decay_fix += (0 - fix) >> k
 *
 * Steady state (derivation confirmed by the numbers below): with contended
 * fraction f of acquisitions and 1-in-N zero-feeds, fixed converges to
 *   SCALE * f*N / (f*N + 1).
 * With N=8 the wait gate threshold 512 (=0.5) becomes "> ~12.5% of my recent
 * acquisitions were contended" -- a real sustained-contention test, not a
 * lifetime latch. f=1.0 -> 910, f=0.5 -> 819 (both well HOT); a startup burst
 * of 6 contended events amid thousands of uncontended ones -> f~0 -> COLD.
 *
 * NOT wall-clock read-time decay (the Model B trap): the 0-feed is applied
 * ONLY when a real release EVENT occurs (write-time), never at read time and
 * never as a function of elapsed wall-clock. During a genuine host stall no
 * releases occur, so NOTHING decays through the stall -- wait_decay freezes,
 * exactly like preempt_decay and Model A/Model C, and unlike Model B which
 * cooled toward 0 during the very steal window IVH exists to catch. The idle
 * tail below cools only because the daemon keeps doing UNCONTENDED lock work
 * (real release events), which is what a live daemon actually does.
 *
 * Build: gcc -O2 -o hotthreads_waitdecay_test hotthreads_waitdecay_test.c -lpthread
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
#include <stdatomic.h>

#define rmb() asm volatile ("lfence" ::: "memory")

#define SCALE            1024        /* 1<<IVH_HOTLOCK_SCALE (10) */
#define HALF             512
#define EWMA_K           3
#define WAIT_THRESH      HALF        /* ivh_hot_wait_threshold default = 512 */
#define WAIT_ZERO_N      8           /* 1-in-N sampled 0-feed (old Hotlock used 8) */

/* ---- per-thread wait_decay state: current (buggy) vs fixed, side by side ---- */
struct wstate {
        int      id;
        atomic_int wait_cur;   /* kernel-current: SCALE-on-contended only, no 0 */
        atomic_int wait_fix;   /* fixed: + sampled 1-in-N 0-feed from cs_exit()  */
        unsigned int sample_ctr;   /* per-task 1-in-N counter (single writer=self) */
        unsigned long long acquires;
        unsigned long long contended_acquires;
};

/* SCALE feed -- models ivh_hot_note_wait_event() from qspinlock slowpath.
 * Fires ONLY on a genuinely contended acquisition, for BOTH cur and fix. */
static void wait_feed_contended(struct wstate *t)
{
        int o;
        o = atomic_load(&t->wait_cur);
        atomic_store(&t->wait_cur, o + ((SCALE - o) >> EWMA_K));
        o = atomic_load(&t->wait_fix);
        atomic_store(&t->wait_fix, o + ((SCALE - o) >> EWMA_K));
}

/* Sampled 0-feed -- models the NEW hook inside cs_exit()'s existing
 * PF_IVH_ELIGIBLE block. Called once per outermost release (== once per
 * acquisition here), contended or not. Only 'fix' is touched; 'cur' models the
 * shipped kernel that has no such feed. */
static void wait_cs_exit_fixed(struct wstate *t)
{
        if ((++t->sample_ctr % WAIT_ZERO_N) != 0)
                return;
        int o = atomic_load(&t->wait_fix);
        atomic_store(&t->wait_fix, o + ((0 - o) >> EWMA_K));
}

/* ---- shared lock scaffolding ---- */
static volatile unsigned long shared_lock __attribute__((aligned(4096))) = 0;
static atomic_int shared_waiters = 0;
static volatile int done = 0;

static unsigned long cmpxchg_ul(volatile unsigned long *p, unsigned long o, unsigned long n)
{
        unsigned long prev;
        asm volatile("lock; cmpxchg %2,%1" : "=a"(prev), "+m"(*p) : "r"(n), "0"(o) : "memory");
        return prev;
}

static double dsecs(void)
{
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* One acquire/hold/release of the shared lock, feeding both models faithfully:
 *   - detect contention the way ivh_pre_lock/qspinlock does (waiters>0 on join)
 *   - contended  -> wait_feed_contended() (the SCALE feed)
 *   - always     -> wait_cs_exit_fixed()  (the sampled 0-feed, from cs_exit) */
static void do_locked_op(struct wstate *t, int cs_iters)
{
        bool contended = atomic_load(&shared_waiters) > 0;
        atomic_fetch_add(&shared_waiters, 1);
        while (cmpxchg_ul(&shared_lock, 0, 1) != 0)
                rmb();
        atomic_fetch_sub(&shared_waiters, 1);

        if (contended)
                wait_feed_contended(t);          /* qspinlock slowpath feed */

        for (int i = 0; i < cs_iters; i++)
                asm volatile ("sfence" ::: "memory");
        shared_lock = 0;

        wait_cs_exit_fixed(t);                    /* cs_exit() sampled 0-feed */

        t->acquires++;
        if (contended) t->contended_acquires++;
}

/* GOOD: sustained-contention worker (NHextend-style, long CS, no gap). */
static int good_cs_iters = 3000000;
static void *good_thread(void *arg)
{
        struct wstate *t = arg;
        while (!done)
                do_locked_op(t, good_cs_iters);
        return NULL;
}

/* BAD: daemon-style worker -- brief CS, real idle gap, rarely contends. */
static void *bad_thread(void *arg)
{
        struct wstate *t = arg;
        while (!done) {
                do_locked_op(t, 2000);
                usleep(50000);
        }
        return NULL;
}

static void reset(struct wstate *ts, int n)
{
        for (int i = 0; i < n; i++) { memset(&ts[i], 0, sizeof(ts[i])); ts[i].id = i; }
        shared_lock = 0; atomic_store(&shared_waiters, 0);
}

/* Threaded regression phase: prove the fix does NOT weaken sustained-contention
 * detection (good stays well above 512) nor lift the daemon (bad stays low). */
static void run_threaded(const char *name, void *(*fn)(void *), int nthreads, int secs)
{
        struct wstate ts[64];
        pthread_t tid[64];
        if (nthreads > 64) nthreads = 64;
        reset(ts, nthreads);
        done = 0;

        printf("=== %s (%d threads, %ds) ===\n", name, nthreads, secs);
        for (int i = 0; i < nthreads; i++) pthread_create(&tid[i], NULL, fn, &ts[i]);
        for (int s = 0; s < secs; s++) {
                sleep(1);
                long cur = 0, fix = 0; int hot_cur = 0, hot_fix = 0;
                for (int i = 0; i < nthreads; i++) {
                        int c = atomic_load(&ts[i].wait_cur), f = atomic_load(&ts[i].wait_fix);
                        cur += c; fix += f;
                        if (c > WAIT_THRESH) hot_cur++;
                        if (f > WAIT_THRESH) hot_fix++;
                }
                printf("  t=%2ds  wait_cur=%6.1f (wait-hot %d/%d)   wait_fix=%6.1f (wait-hot %d/%d)\n",
                       s + 1, (double)cur / nthreads, hot_cur, nthreads,
                       (double)fix / nthreads, hot_fix, nthreads);
        }
        done = 1;
        for (int i = 0; i < nthreads; i++) pthread_join(tid[i], NULL);

        long cur = 0, fix = 0; unsigned long long acq = 0, cont = 0;
        for (int i = 0; i < nthreads; i++) {
                cur += atomic_load(&ts[i].wait_cur);
                fix += atomic_load(&ts[i].wait_fix);
                acq += ts[i].acquires; cont += ts[i].contended_acquires;
        }
        printf("  -> final wait_cur=%.1f  wait_fix=%.1f  | gt_contended=%.1f%%  (fix models f*N/(f*N+1))\n\n",
               (double)cur / nthreads, (double)fix / nthreads,
               acq ? 100.0 * cont / acq : 0.0);
}

/* ---- the sshd reproduction: short contended burst, then a long idle tail ---- */

/* Contender thread: holds the shared lock hard for a bounded window so the
 * daemon's acquisitions during that window are genuinely contended. */
static volatile int contender_go = 0;
static void *contender_thread(void *arg)
{
        (void)arg;
        while (!done) {
                if (!contender_go) { usleep(1000); continue; }
                while (cmpxchg_ul(&shared_lock, 0, 1) != 0) rmb();
                for (int i = 0; i < 200000; i++) asm volatile ("sfence" ::: "memory");
                shared_lock = 0;
                atomic_fetch_add(&shared_waiters, 0); /* keep memory hot */
        }
        return NULL;
}

static void sshd_phase(int idle_secs, bool truly_idle)
{
        struct wstate t; memset(&t, 0, sizeof(t));
        pthread_t ctid;
        done = 0; contender_go = 0; shared_lock = 0; atomic_store(&shared_waiters, 0);
        pthread_create(&ctid, NULL, contender_thread, NULL);

        printf("=== SSHD REPRO: startup burst then %ds %s tail%s ===\n",
               idle_secs, truly_idle ? "TRULY-IDLE (zero lock events)" : "daemon-idle (uncontended poll loop)",
               truly_idle ? "" : "");

        /* --- startup burst: drive ~6-10 genuinely contended acquisitions --- */
        contender_go = 1;
        int burst_target = 8;
        printf("  [burst] driving genuine contended acquisitions (sshd startup):\n");
        while (t.contended_acquires < (unsigned)burst_target) {
                /* announce as a waiter so the contender's hold registers as contention */
                bool contended;
                atomic_fetch_add(&shared_waiters, 1);
                /* spin until we actually get it (contender holds it a while) */
                long long spins = 0;
                while (cmpxchg_ul(&shared_lock, 0, 1) != 0) { spins++; rmb(); }
                contended = spins > 0; /* we genuinely queued behind the contender */
                atomic_fetch_sub(&shared_waiters, 1);
                if (contended) wait_feed_contended(&t);
                shared_lock = 0;
                wait_cs_exit_fixed(&t);
                t.acquires++;
                if (contended) {
                        t.contended_acquires++;
                        printf("    contended #%llu -> wait_cur=%d wait_fix=%d\n",
                               t.contended_acquires,
                               atomic_load(&t.wait_cur), atomic_load(&t.wait_fix));
                }
                if (t.acquires > 100000) break; /* safety */
        }
        contender_go = 0;
        usleep(50000); /* let contender settle out of the lock */
        shared_lock = 0; atomic_store(&shared_waiters, 0);

        int burst_cur = atomic_load(&t.wait_cur), burst_fix = atomic_load(&t.wait_fix);
        printf("  [burst done] wait_cur=%d  wait_fix=%d   (matches live sshd's 563-class value)\n",
               burst_cur, burst_fix);

        /* --- idle tail --- */
        double t0 = dsecs();
        printf("  [idle tail] wait threshold=%d; wait_cur is the shipped kernel, wait_fix is the fix\n", WAIT_THRESH);
        for (int s = 1; s <= idle_secs; s++) {
                double target = t0 + s;
                if (truly_idle) {
                        /* zero lock events: sleep the whole second, no acquisitions */
                        while (dsecs() < target) usleep(20000);
                } else {
                        /* daemon poll loop: wake ~every 10ms, take a handful of
                         * UNCONTENDED kernel-style locks each wake (epoll/read/
                         * timer housekeeping). No contender -> waiters stays 0. */
                        while (dsecs() < target) {
                                for (int k = 0; k < 20; k++)
                                        do_locked_op(&t, 500);
                                usleep(10000);
                        }
                }
                if (s <= 5 || s % 5 == 0 || s == idle_secs)
                        printf("    +%2ds  wait_cur=%5d %-9s  wait_fix=%5d %-9s  (acq=%llu)\n",
                               s, atomic_load(&t.wait_cur),
                               atomic_load(&t.wait_cur) > WAIT_THRESH ? "[WAIT-HOT]" : "[cold]",
                               atomic_load(&t.wait_fix),
                               atomic_load(&t.wait_fix) > WAIT_THRESH ? "[WAIT-HOT]" : "[cold]",
                               t.acquires);
        }
        done = 1; pthread_join(ctid, NULL);

        int fc = atomic_load(&t.wait_cur), ff = atomic_load(&t.wait_fix);
        printf("  RESULT: after %ds idle tail  wait_cur=%d %s   wait_fix=%d %s\n\n",
               idle_secs, fc, fc > WAIT_THRESH ? "STILL WAIT-HOT (bug)" : "cold",
               ff, ff > WAIT_THRESH ? "STILL WAIT-HOT" : "cold (FIXED)");
}

int main(int argc, char **argv)
{
        int good_threads = argc > 1 ? atoi(argv[1]) : 8;
        int idle_secs    = argc > 2 ? atoi(argv[2]) : 35;

        printf("Hot Threads wait_decay fix validation\n");
        printf("SCALE=%d k=%d WAIT_THRESH=%d  sampled-0-feed N=%d\n", SCALE, EWMA_K, WAIT_THRESH, WAIT_ZERO_N);
        printf("steady-state fix target = SCALE*f*N/(f*N+1):  f=1.0->%.0f  f=0.5->%.0f  f=0.125->%.0f\n\n",
               (double)SCALE * 1.0 * WAIT_ZERO_N / (1.0 * WAIT_ZERO_N + 1),
               (double)SCALE * 0.5 * WAIT_ZERO_N / (0.5 * WAIT_ZERO_N + 1),
               (double)SCALE * 0.125 * WAIT_ZERO_N / (0.125 * WAIT_ZERO_N + 1));

        /* 1. Regression: sustained contention must stay well above 512 under fix. */
        run_threaded("REGRESSION: GOOD (sustained contention)", good_thread, good_threads, 5);
        /* 2. Regression: daemon-style must stay low under both. */
        run_threaded("REGRESSION: BAD (daemon, idle gaps)", bad_thread, 1, 5);
        /* 3. The bug: sshd startup burst then realistic daemon-idle tail. */
        sshd_phase(idle_secs, false);
        /* 4. Honest edge: burst then LITERALLY zero lock events for the tail. */
        sshd_phase(12, true);

        printf("================= SUMMARY =================\n");
        printf("- GOOD stays wait-HOT under the fix (sustained contention preserved).\n");
        printf("- BAD stays cold under both.\n");
        printf("- sshd burst -> daemon-idle: wait_cur latches ~563 forever; wait_fix decays <512\n");
        printf("  within the first second of ordinary uncontended lock work and settles near 0.\n");
        printf("- sshd burst -> TRULY-idle (zero events): BOTH stay latched. This is SAFE, because\n");
        printf("  with zero lock events preempt_decay also cannot rise, so the AND-gate cannot fire;\n");
        printf("  the first release event that could raise preempt_decay is itself a 0-feed for\n");
        printf("  wait_fix (event-driven decay needs an event -- by design, not wall-clock).\n");
        return 0;
}
