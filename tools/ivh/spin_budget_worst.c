/*
 * spin_budget_worst.c -- how long does a PV qspinlock waiter take to deplete
 * ivh_pv_spin_threshold, with no preemption?
 *
 * v2 (2026-10-02) after review. v1 IS WRONG and is kept as .c.v1 only as a
 * record of the defect: it read the PREDECESSOR's line on every iteration and
 * so reported a 5.35 ms "worst case" that the kernel cannot reach.
 *
 *   pv_wait_early() (qspinlock_paravirt.h:1331) opens with
 *       if ((loop & PV_PREV_CHECK_MASK) != 0) return PV_BAIL_NONE;
 *   and PV_PREV_CHECK_MASK is 0xff (:47). The NODE loop therefore consults
 *   prev->state once per 256 iterations, not once per iteration. v1's figure
 *   was a property of the replica, not of the lock.
 *
 * WHAT THE TWO LOOPS ACTUALLY TOUCH
 *
 * NODE loop (-M node), qspinlock_paravirt.h:1920-2160, hot lane = 255/256:
 *     READ_ONCE(node->locked)   offset 0x08 of OUR pv_node
 *     READ_ONCE(pn->state)      offset 0x14 of the SAME pv_node -> SAME 64B line
 *     two read-mostly global sysctls (separate lines, far apart)
 *     cpu_relax()
 *   prev->state is read only when (loop & 0xff) == 0.
 *   Nothing stores into our node line during the spin: the predecessor writes
 *   node->locked exactly once and that store ENDS the loop. So in the shipped
 *   config every load in the node loop hits L1, on every iteration, and the
 *   node loop has NO cache-state range to report.
 *
 * HEAD loop (-M head), :3595-3684:
 *     trylock_clear_pending(lock) -> READ_ONCE(lock->locked) EVERY iteration.
 *   The lock word is written by every acquire, every release and every
 *   xchg_tail queue-join, all on one line. THIS is the cache-sensitive loop,
 *   and the one a dirty-line sweep is a valid model of.
 *
 * ivh_node_publish_in_spin() is NOT a per-256 store. It gates on
 * (loop & ivh_pv_beat_publish_mask) with the mask live at 4095 -> every 4096th;
 * it returns at its first line while ivh_pv_preempt_src == 0 (live: 0), so it
 * does not run at all in the measured config; and at ivh_pv_evict_node_stamp
 * == 0 (live: 0) it writes the per-CPU ivh_tsc_beat.stamp, not the node line.
 * v1's header claimed "every 256th" -- that conflated PV_PREV_CHECK_MASK with
 * ivh_pv_beat_publish_mask.
 *
 * ESTIMATOR. One sample == one whole 32768-iteration budget, so the sample IS
 * a budget duration. The MEDIAN is the TYPICAL budget duration in a regime --
 * not a worst case; do not label it one. min-over-reps is wrong (at >=2
 * writers it selects reps in which the WRITERS were descheduled and reports the
 * uncontended cost as the contended one). max/median is reported because the
 * tail IS the preempted population and that is a result, not contamination.
 * SCHED_FIFO plus a getrusage() context-switch check makes "no preemption" an
 * assertion per rep instead of a hope -- but note a TD cannot observe
 * PAUSE-loop exiting (PLE is a VMCS control the TDX module owns), so
 * "no preemption" remains unverifiable from inside the guest.
 *
 * Usage: spin_budget_worst [-M node|head] [-w writers] [-m mask] [-i iters]
 *                          [-r reps] [-t tsc_mhz] [-F] [-q]
 *   -m  cadence mask for the REMOTE consult (node: 0xff; head: 0 = every iter)
 *   -F  do not try SCHED_FIFO
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <immintrin.h>
#include <sys/resource.h>

static long ITERS = 32768, REPS = 400;
static double TSC_MHZ = 2200.0;
static unsigned long RMASK = 0xff;      /* remote consult cadence */
static int MODE_HEAD, NOFIFO, QUIET;

static volatile int go = 1;
/* our own pv_node: locked (0x08) and state (0x14) share ONE line, as in the
 * real struct. Nothing stores here during the spin. */
static _Alignas(64) volatile unsigned long own_node[8];
/* the predecessor's node line (node mode) / the lock word (head mode): the
 * line a writer dirties. */
static _Alignas(64) volatile unsigned long remote_line[8];
static _Alignas(64) volatile unsigned long g_probe[8];
static _Alignas(64) volatile unsigned long g_src[8];
static uint64_t *samples;
static long rejected;

static inline uint64_t rd(void)
{ unsigned a, d; __asm__ __volatile__("lfence;rdtsc" : "=a"(a), "=d"(d)); return ((uint64_t)d << 32) | a; }
static inline unsigned long RO(volatile unsigned long *p)
{ return __atomic_load_n(p, __ATOMIC_RELAXED); }
static void pin(int c)
{ cpu_set_t s; CPU_ZERO(&s); CPU_SET(c, &s); pthread_setaffinity_np(pthread_self(), sizeof s, &s); }
static long csw(void)
{ struct rusage r; getrusage(RUSAGE_THREAD, &r); return r.ru_nvcsw + r.ru_nivcsw; }

static void *writer(void *arg)
{
	pin((int)(long)arg);
	while (go) __atomic_store_n(&remote_line[0], 0, __ATOMIC_RELAXED);
	return 0;
}

static int cmp(const void *a, const void *b)
{ uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }

int main(int argc, char **argv)
{
	int nw = 0, c; long got = 0;
	volatile unsigned long sink = 0;
	pthread_t t[256];

	while ((c = getopt(argc, argv, "M:w:m:i:r:t:Fq")) != -1)
		switch (c) {
		case 'M': MODE_HEAD = (optarg[0] == 'h'); if (MODE_HEAD) RMASK = 0; break;
		case 'w': nw = atoi(optarg); break;
		case 'm': RMASK = strtoul(optarg, 0, 0); break;
		case 'i': ITERS = atol(optarg); break;
		case 'r': REPS = atol(optarg); break;
		case 't': TSC_MHZ = atof(optarg); break;
		case 'F': NOFIFO = 1; break;
		case 'q': QUIET = 1; break;
		default: return 2;
		}
	samples = calloc(REPS, sizeof *samples);
	pin(0);
	if (!NOFIFO) {
		struct sched_param sp = { .sched_priority = 50 };
		if (sched_setscheduler(0, SCHED_FIFO, &sp) && !QUIET)
			printf("  (note: SCHED_FIFO unavailable; preemption not suppressed)\n");
	}
	for (int i = 0; i < nw; i++)
		pthread_create(&t[i], 0, writer, (void *)(long)(i + 1));
	if (nw) usleep(300000);

	for (long r = 0; r < REPS; r++) {
		long c0 = csw();
		uint64_t t0 = rd();
		for (unsigned long loop = ITERS; loop; loop--) {
			if (MODE_HEAD) {
				/* head: polls the lock word EVERY iteration */
				if (RO(&remote_line[0]) == 0xdead) break;
			} else {
				/* node: own line, both fields, one cache line */
				if (RO(&own_node[1])) break;                /* node->locked */
				if (RO(&own_node[2]) == 0xdead) break;      /* pn->state    */
			}
			if (RO(&g_probe[0])) sink++;                        /* sysctl       */
			if ((loop & RMASK) == 0 &&
			    RO(&remote_line[0]) == 0xdead) break;           /* prev->state  */
			if (RO(&g_src[0])) sink++;                          /* sysctl       */
			_mm_pause();                                        /* cpu_relax    */
		}
		uint64_t dt = rd() - t0;
		if (csw() != c0) { rejected++; continue; }   /* descheduled: discard */
		samples[got++] = dt;
	}
	go = 0;
	for (int i = 0; i < nw; i++) pthread_join(t[i], 0);

	if (got < 8) { printf("  only %ld clean reps -- machine too busy\n", got); return 1; }
	qsort(samples, got, sizeof *samples, cmp);
	double pi(long k) { return (double)samples[k] / ITERS; }
	double med = pi(got / 2), mx = pi(got - 1);
	printf("%-5s w=%-2d mask=0x%-4lx  median %7.1f cyc (%6.2f ns)  BUDGET %8.1f us"
	       "  [min %6.1f p5 %6.1f p95 %6.1f max %8.1f]  clean %ld/%ld",
	       MODE_HEAD ? "head" : "node", nw, RMASK, med, med / TSC_MHZ * 1000.0,
	       ITERS * med / TSC_MHZ, pi(0), pi(got / 20), pi(19 * got / 20), mx, got, REPS);
	if (med > 0 && mx / med > 2.0)
		printf("  <- max/median %.1fx: tail is the preempted/exited population", mx / med);
	printf("\n");
	return 0;
}
