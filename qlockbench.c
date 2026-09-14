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
 * ctx->wqh.lock (fs/eventfd.c), a spinlock acquired via spin_lock_irqsave(),
 * which routes to _raw_spin_lock_irqsave() -- one of the three functions
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
 * Build:  gcc -O2 -Wall -o qlockbench qlockbench.c -lpthread
 * Usage:  ./qlockbench [-t threads] [-d seconds] [-p] [-q]
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

static int	 nthreads;
static int	 duration = 10;
static bool	 per_thread_fd;		/* -p: control arm, one fd per thread */
static bool	 quiet;
static int	 shared_fd = -1;
static volatile int done;

struct tdata {
	pthread_t	th;
	int		fd;		/* shared_fd, or this thread's own */
	unsigned long long ops;
	int		idx;
};

static pthread_barrier_t barrier;

static void *worker(void *arg)
{
	struct tdata *t = arg;
	uint64_t one = 1, val;

	pthread_barrier_wait(&barrier);

	while (!done) {
		/*
		 * write() then read() -- both take ctx->wqh.lock, and pairing
		 * them keeps the counter bounded so write() never blocks on
		 * saturation. Errors are ignored deliberately: under EFD_NONBLOCK
		 * a read of an empty counter returns EAGAIN, which still cost us
		 * the lock acquisition we are here to measure.
		 */
		if (write(t->fd, &one, sizeof(one)) == sizeof(one))
			t->ops++;
		if (read(t->fd, &val, sizeof(val)) == sizeof(val))
			t->ops++;
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
	unsigned long long total = 0;
	double t0, t1, elapsed;
	int i, c;

	nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);

	while ((c = getopt(argc, argv, "t:d:pqh")) != -1) {
		switch (c) {
		case 't': nthreads = atoi(optarg); break;
		case 'd': duration = atoi(optarg); break;
		case 'p': per_thread_fd = true; break;
		case 'q': quiet = true; break;
		default:
			fprintf(stderr,
				"usage: %s [-t threads] [-d seconds] [-p] [-q]\n"
				"  -t N  worker threads (default: nproc)\n"
				"  -d N  run for N seconds (default: 10)\n"
				"  -p    CONTROL ARM: one eventfd per thread, so contention\n"
				"        spreads over N locks instead of queueing on one\n"
				"  -q    print only the ops/sec number\n", argv[0]);
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
	for (i = 0; i < nthreads; i++) {
		td[i].idx = i;
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

	for (i = 0; i < nthreads; i++)
		total += td[i].ops;

	if (quiet) {
		printf("%.0f\n", total / elapsed);
	} else {
		printf("threads=%d  duration=%.2fs  mode=%s\n",
		       nthreads, elapsed,
		       per_thread_fd ? "per-thread fds (CONTROL: many locks)"
				     : "shared fd (deep queue on ONE lock)");
		printf("total_ops=%llu\n", total);
		printf("ops_per_sec=%.0f\n", total / elapsed);
	}
	return 0;
}
