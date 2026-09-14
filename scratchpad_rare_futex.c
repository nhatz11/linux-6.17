/*
 * rare_futex — reliably drive ONE PF_IVH_ELIGIBLE thread "hot" on wait_decay via
 * genuine kernel raw_spinlock (futex hash-bucket lock) contention, then go idle.
 *
 * The observed (main) thread and N noise threads hammer FUTEX_WAKE on the SAME
 * address for a brief burst -> contended futex bucket spinlock -> qspinlock
 * slowpath -> ivh_hot_note_wait_event() feeds each thread's ivh_wait_decay.
 * After the burst ALL threads sleep (pure idle) -> no kernel raw_spinlock
 * releases -> no cs_exit 0-sample -> wait_decay frozen.
 *
 * Usage: rare_futex <noise_threads> <burst_ms> <idle_sec>
 * env RC_HEARTBEAT=<sec>: main thread wakes every <sec>s and does light futex
 *   activity (models a daemon doing a tiny bit of work), to measure event-indexed
 *   decay under minimal load instead of a pure freeze.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <linux/futex.h>

#ifndef PR_SET_IVH_ELIGIBLE
#define PR_SET_IVH_ELIGIBLE 79
#endif
#ifndef PR_GET_IVH_ELIGIBLE
#define PR_GET_IVH_ELIGIBLE 80
#endif

static int futex_word = 0;
static volatile int burst_go = 0;
static volatile int stop = 0;
static long burst_ns;

static long now_ns(void)
{
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000L + ts.tv_nsec;
}

static void hammer(long duration_ns)
{
	long end = now_ns() + duration_ns;
	while (now_ns() < end)
		syscall(SYS_futex, &futex_word, FUTEX_WAKE, 1, NULL, NULL, 0);
}

static void *noise_fn(void *arg)
{
	(void)arg;
	while (!burst_go) sched_yield();
	hammer(burst_ns);
	while (!stop) { struct timespec t = {.tv_sec=1}; nanosleep(&t,NULL); }
	return NULL;
}

int main(int argc, char **argv)
{
	int nthreads = argc > 1 ? atoi(argv[1]) : 7;
	int burst_ms = argc > 2 ? atoi(argv[2]) : 300;
	int idle     = argc > 3 ? atoi(argv[3]) : 30;
	burst_ns = (long)burst_ms * 1000000L;
	int i;

	if (prctl(PR_SET_IVH_ELIGIBLE, 1, 0, 0, 0) != 0) { perror("prctl"); return 1; }
	printf("rare_futex pid=%d tid=%ld eligible=%d noise=%d burst=%dms idle=%ds\n",
	       getpid(), syscall(SYS_gettid),
	       prctl(PR_GET_IVH_ELIGIBLE,0,0,0,0), nthreads, burst_ms, idle);
	fflush(stdout);

	pthread_t th[64];
	for (i = 0; i < nthreads; i++)
		pthread_create(&th[i], NULL, noise_fn, NULL);

	/* Fire the burst: everyone hammers the same futex bucket at once. */
	burst_go = 1;
	hammer(burst_ns);   /* main (observed) thread contends too */
	printf("burst done; main thread idling %ds\n", idle);
	fflush(stdout);

	char *hb = getenv("RC_HEARTBEAT");
	int hbsec = hb ? atoi(hb) : 0;
	if (hbsec > 0) {
		int e = 0;
		while (e < idle) {
			struct timespec ts = {.tv_sec = hbsec};
			nanosleep(&ts, NULL);
			/* light activity: a few uncontended futex wakes */
			for (int k = 0; k < 20; k++)
				syscall(SYS_futex, &futex_word, FUTEX_WAKE, 1, NULL, NULL, 0);
			e += hbsec;
		}
	} else {
		struct timespec ts = {.tv_sec = idle};
		nanosleep(&ts, NULL);        /* pure idle: main thread runs no code */
		/* single wake probe so a kprobe can read the frozen value */
		syscall(SYS_futex, &futex_word, FUTEX_WAKE, 1, NULL, NULL, 0);
	}

	stop = 1;
	burst_go = 1;
	for (i = 0; i < nthreads; i++) pthread_join(th[i], NULL);
	printf("rare_futex pid=%d done\n", getpid());
	return 0;
}
