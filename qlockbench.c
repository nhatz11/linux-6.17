// SPDX-License-Identifier: GPL-2.0
/*
 * qlockbench -- drive a DEEP MCS queue on a SINGLE kernel spinlock.
 *
 * WHY THIS EXISTS
 * ===============
 * hackbench is this project's usual kernel-lock workload, but it spreads its
 * contention across many pipe/socket locks with only ~2 waiters each. That is
 * the wrong shape for measuring anything about MCS queue *depth*: with two
 * waiters there is never a queue to walk, so a mechanism like handoff-time
 * rotation (skip a preempted successor, promote the first live waiter) has
 * nothing to act on and nothing to measure.
 *
 * This benchmark instead points every thread at ONE kernel lock, so the MCS
 * queue behind it is as deep as the machine allows.
 *
 * WHICH LOCK
 * ==========
 * A shared eventfd. Both eventfd_write() and eventfd_read() take
 * ctx->wqh.lock (fs/eventfd.c), a spinlock acquired via spin_lock_irq(),
 * which routes to _raw_spin_lock_irq() -- one of the three functions
 * carrying IVH's ivh_pre_lock() hook (kernel/locking/spinlock.c). So this
 * exercises the real PV qspinlock slowpath, the MCS queue, and the IVH
 * pre-lock migration hook, with no filesystem or block I/O in the way.
 *
 * HOW DEEP CAN THE QUEUE ACTUALLY GET
 * ===================================
 * spin_lock() calls preempt_disable(), so a thread spinning in the MCS queue
 * CANNOT be preempted by the guest scheduler. Consequences, both important:
 *
 *   1. Queue depth is bounded by the number of vCPUs, not by thread count.
 *      At most one task per vCPU can be spinning at any instant. Running 64
 *      threads on 16 vCPUs does NOT produce a 64-deep queue -- it produces a
 *      <=16-deep queue plus 48 threads descheduled outside the lock. Default
 *      thread count is therefore nproc, and raising it mainly raises the
 *      acquisition rate, not the depth.
 *
 *   2. A queued waiter can only be "preempted" by the HOST taking its vCPU
 *      away. That is precisely lock-holder preemption, and it is why this
 *      benchmark is only meaningful with real host contention present (a
 *      co-running VM). On an uncontended host every waiter stays runnable,
 *      no successor ever looks preempted, and the interesting counters stay
 *      at zero -- which is a correct result, not a broken run.
 *
 * BUILT-IN CONTROL ARM
 * ====================
 * -p gives each thread its own eventfd instead of sharing one. Same syscall
 * rate, same code path, but the contention is spread across N independent
 * locks -- i.e. deliberately hackbench-shaped. Any queue-depth-dependent
 * effect must appear with the default (shared) mode and vanish with -p. If it
 * shows up in both, it is not about queue depth and the measurement is
 * telling you something else.
 *
 * WHY ops/sec IS NOT ENOUGH
 * =========================
 * Lock-holder preemption is a TAIL phenomenon: one preempted holder or head
 * stalls the whole queue for up to a host timeslice (milliseconds), a few
 * hundred times a run. Averaged into a 15s throughput figure that is a
 * rounding error -- so a mechanism that removes a rare multi-millisecond
 * stall at the cost of a few tens of cycles per handoff LOSES on ops/sec and
 * WINS on p99.9. Scoring such a mechanism on mean throughput alone measures
 * only the half of it that loses.
 *
 * So each syscall is bracketed with rdtsc and binned into a per-thread
 * log2+3bit histogram (~9% resolution), reported as p50/p99/p99.9/p99.99/max
 * plus the count of acquisitions over 1ms. Cost is ~2 rdtsc (~60 cycles)
 * against a ~5.7us mean operation, i.e. ~0.2%, and it is IDENTICAL across
 * arms, so it cancels in a paired comparison.
 *
 * ops VS iters -- READ THE DIFFERENCE
 * ===================================
 * ->ops counts SUCCESSFUL syscalls only, which is what every historical run
 * in this project reported; it is kept bit-identical so old CSVs stay
 * comparable. But a read() of a drained counter returns EAGAIN *after*
 * taking the lock, and whether it drains depends on who won the lock first --
 * which is exactly what a queue-reordering mechanism changes. So ops/sec can
 * move with no change in lock throughput at all.
 *
 * ->iters counts ATTEMPTS, which is the unbiased lock-acquisition rate.
 * Report both: if ops/iters (the "hit rate") differs between arms, the ops
 * comparison is contaminated and the iters comparison is the real one.
 *
 * Build:  gcc -O2 -Wall -o qlockbench qlockbench.c -lpthread
 * Usage:  ./qlockbench [-t threads] [-d seconds] [-p] [-q|-Q] [-a]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/eventfd.h>
#include <time.h>
#include <sched.h>
#include <x86intrin.h>

static int	 nthreads;
static int	 duration = 10;
static bool	 per_thread_fd;		/* -p: control arm, one fd per thread */
static bool	 quiet;
static bool	 quiet_csv;	/* -Q: one machine-readable line */
static bool	 pin_threads;	/* -a: pin thread i to vCPU i */
static int	 shared_fd = -1;
static volatile int done;


/*
 * Latency histogram: log2 with 3 mantissa bits, i.e. 8 buckets per octave,
 * ~9% width. Index for v < 8 is v itself; above that it is (exp << 3) | top3
 * mantissa bits, so indices 8..23 are unused and the mapping is monotonic.
 * 512 buckets * 8 bytes = 4KB per thread, private, no sharing.
 */
#define HBITS	3
#define NBUCK	(64 * (1 << HBITS))

static inline unsigned hbucket(uint64_t v)
{
	unsigned e, m;

	if (v < (1u << HBITS))
		return (unsigned)v;
	e = 63 - __builtin_clzll(v);
	m = (unsigned)((v >> (e - HBITS)) & ((1u << HBITS) - 1));
	return (e << HBITS) | m;
}

/* Lower edge of a bucket, in cycles. */
static uint64_t hval(unsigned b)
{
	unsigned e, m;

	if (b < (1u << HBITS))
		return b;
	e = b >> HBITS;
	m = b & ((1u << HBITS) - 1);
	return (uint64_t)((1u << HBITS) | m) << (e - HBITS);
}

static uint64_t tsc_hz;

/*
 * Calibrate TSC against CLOCK_MONOTONIC. dmesg reports an exact 2200.000 MHz
 * on this host, but measuring costs 50ms once and survives a move to any
 * other box -- and a wrong constant would silently rescale every percentile.
 */
static void calibrate_tsc(void)
{
	struct timespec a, b;
	uint64_t c0, c1;
	double ns;

	clock_gettime(CLOCK_MONOTONIC, &a);
	c0 = __rdtsc();
	usleep(50000);
	c1 = __rdtsc();
	clock_gettime(CLOCK_MONOTONIC, &b);

	ns = (b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec);
	tsc_hz = (uint64_t)((c1 - c0) / ns * 1e9);
}

static double cyc_to_us(uint64_t c)
{
	return (double)c * 1e6 / (double)tsc_hz;
}

/* Lower edge, in us, of the bucket containing the p'th percentile. */
static double pctile(const uint64_t *h, uint64_t total, double p)
{
	uint64_t want = (uint64_t)(total * p), seen = 0;
	unsigned b;

	if (!total)
		return 0.0;
	for (b = 0; b < NBUCK; b++) {
		seen += h[b];
		if (seen >= want)
			return cyc_to_us(hval(b));
	}
	return cyc_to_us(hval(NBUCK - 1));
}

struct tdata {
	pthread_t	th;
	int		fd;		/* shared_fd, or this thread's own */
	unsigned long long ops;		/* SUCCESSFUL syscalls (historical metric) */
	unsigned long long iters;	/* ATTEMPTED syscalls (unbiased lock rate) */
	unsigned long long slow;	/* acquisitions over 1ms */
	uint64_t	max;		/* worst single acquisition, cycles */
	uint64_t	*hist;		/* NBUCK buckets, private to this thread */
	int		idx;
};

static pthread_barrier_t barrier;

static uint64_t slow_cycles;	/* 1ms, in TSC cycles; set after calibration */

static inline void bump(struct tdata *t, uint64_t d)
{
	/*
	 * A thread migrated between vCPUs mid-measurement could in principle
	 * see a backwards delta. TSC is invariant and synchronised here, but
	 * clamp rather than index a 512-entry array with garbage.
	 */
	if ((int64_t)d < 0)
		return;
	t->hist[hbucket(d)]++;
	if (d > t->max)
		t->max = d;
	if (d > slow_cycles)
		t->slow++;
}

static void *worker(void *arg)
{
	struct tdata *t = arg;
	uint64_t one = 1, val;

	if (pin_threads) {
		cpu_set_t set;

		CPU_ZERO(&set);
		CPU_SET(t->idx % (int)sysconf(_SC_NPROCESSORS_ONLN), &set);
		pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
	}

	pthread_barrier_wait(&barrier);

	while (!done) {
		uint64_t a, b, c;

		/*
		 * write() then read() -- both take ctx->wqh.lock, and pairing
		 * them keeps the counter bounded so write() never blocks on
		 * saturation. Under EFD_NONBLOCK a read of an empty counter
		 * returns EAGAIN, which still cost us the lock acquisition we
		 * are here to measure: it counts in ->iters and in the
		 * histogram, and (only) not in ->ops. See the header.
		 */
		a = __rdtsc();
		if (write(t->fd, &one, sizeof(one)) == sizeof(one))
			t->ops++;
		b = __rdtsc();
		if (read(t->fd, &val, sizeof(val)) == sizeof(val))
			t->ops++;
		c = __rdtsc();

		t->iters += 2;
		bump(t, b - a);
		bump(t, c - b);
	}
	return NULL;
}

static double now_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	struct tdata *td;
	unsigned long long total = 0, iters = 0, slow = 0;
	uint64_t *agg, worst = 0;
	double t0, t1, elapsed;
	int i, c;

	calibrate_tsc();
	slow_cycles = tsc_hz / 1000;		/* 1ms */

	nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);

	while ((c = getopt(argc, argv, "t:d:pqQah")) != -1) {
		switch (c) {
		case 't': nthreads = atoi(optarg); break;
		case 'd': duration = atoi(optarg); break;
		case 'p': per_thread_fd = true; break;
		case 'q': quiet = true; break;
		case 'Q': quiet_csv = true; break;
		case 'a': pin_threads = true; break;
		default:
			fprintf(stderr,
				"usage: %s [-t threads] [-d seconds] [-p] [-q]\n"
				"  -t N  worker threads (default: nproc)\n"
				"  -d N  run for N seconds (default: 10)\n"
				"  -p    CONTROL ARM: one eventfd per thread, so contention\n"
				"        spreads over N locks instead of queueing on one\n"
				"  -q    print only the ops/sec number (unchanged, for old harnesses)\n"
				"  -Q    print one CSV line: ops,iters,hit,p50,p99,p999,p9999,max,over1ms\n"
				"  -a    pin thread i to vCPU i (removes placement noise;\n"
				"        off by default so the baseline matches historical runs)\n",
				argv[0]);
			exit(c == 'h' ? 0 : 1);
		}
	}
	if (nthreads < 1 || duration < 1) {
		fprintf(stderr, "bad thread count or duration\n");
		exit(1);
	}

	td = calloc(nthreads, sizeof(*td));
	if (!td) { perror("calloc"); exit(1); }

	if (!per_thread_fd) {
		shared_fd = eventfd(0, EFD_NONBLOCK);
		if (shared_fd < 0) { perror("eventfd"); exit(1); }
	}
	agg = calloc(NBUCK, sizeof(*agg));
	if (!agg) { perror("calloc"); exit(1); }

	for (i = 0; i < nthreads; i++) {
		td[i].idx = i;
		td[i].hist = calloc(NBUCK, sizeof(*td[i].hist));
		if (!td[i].hist) { perror("calloc"); exit(1); }
		if (per_thread_fd) {
			td[i].fd = eventfd(0, EFD_NONBLOCK);
			if (td[i].fd < 0) { perror("eventfd"); exit(1); }
		} else {
			td[i].fd = shared_fd;
		}
	}

	pthread_barrier_init(&barrier, NULL, nthreads + 1);
	for (i = 0; i < nthreads; i++) {
		if (pthread_create(&td[i].th, NULL, worker, &td[i])) {
			perror("pthread_create"); exit(1);
		}
	}

	pthread_barrier_wait(&barrier);
	t0 = now_sec();
	sleep(duration);
	done = 1;
	for (i = 0; i < nthreads; i++)
		pthread_join(td[i].th, NULL);
	t1 = now_sec();
	elapsed = t1 - t0;

	for (i = 0; i < nthreads; i++) {
		unsigned b;

		total += td[i].ops;
		iters += td[i].iters;
		slow  += td[i].slow;
		if (td[i].max > worst)
			worst = td[i].max;
		for (b = 0; b < NBUCK; b++)
			agg[b] += td[i].hist[b];
	}

	if (quiet) {
		/* Unchanged: exactly one number, for the existing harness. */
		printf("%.0f\n", total / elapsed);
	} else if (quiet_csv) {
		printf("%.0f,%.0f,%.4f,%.2f,%.2f,%.2f,%.2f,%.2f,%llu\n",
		       total / elapsed, iters / elapsed,
		       iters ? (double)total / (double)iters : 0.0,
		       pctile(agg, iters, 0.50), pctile(agg, iters, 0.99),
		       pctile(agg, iters, 0.999), pctile(agg, iters, 0.9999),
		       cyc_to_us(worst), slow);
	} else {
		printf("threads=%d  duration=%.2fs  mode=%s%s\n",
		       nthreads, elapsed,
		       per_thread_fd ? "per-thread fds (CONTROL: many locks)"
				     : "shared fd (deep queue on ONE lock)",
		       pin_threads ? "  [pinned]" : "");
		printf("tsc_hz=%llu\n", (unsigned long long)tsc_hz);
		printf("total_ops=%llu\n", total);
		printf("ops_per_sec=%.0f\n", total / elapsed);
		printf("iters_per_sec=%.0f\n", iters / elapsed);
		printf("hit_rate=%.4f   (ops/iters; compare ACROSS ARMS -- if this\n"
		       "                 moves, the ops comparison is contaminated)\n",
		       iters ? (double)total / (double)iters : 0.0);
		printf("\nlock acquisition latency (us), n=%llu\n", iters);
		printf("  p50   =%9.2f\n", pctile(agg, iters, 0.50));
		printf("  p99   =%9.2f\n", pctile(agg, iters, 0.99));
		printf("  p99.9 =%9.2f\n", pctile(agg, iters, 0.999));
		printf("  p99.99=%9.2f\n", pctile(agg, iters, 0.9999));
		printf("  max   =%9.2f\n", cyc_to_us(worst));
		printf("  >1ms  =%9llu   (%.1f/s -- the LHP stalls; this is the\n"
		       "                    number eviction has to move)\n",
		       slow, slow / elapsed);
	}
	return 0;
}
