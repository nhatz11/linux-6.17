// SPDX-License-Identifier: GPL-2.0
/*
 * afl_mp_test -- prove ivh_adaptive_futex_lock works ACROSS PROCESSES.
 *
 * The single-process sweep cannot prove this: with IVH_AFL_SHARED=1 on
 * anonymous memory the kernel's get_futex_key() still resolves to an
 * (mm, addr) key after the shared-path work, so it exercises the extra cost
 * but never the cross-process case. Here the lock lives in a real
 * mmap(MAP_SHARED|MAP_ANONYMOUS) segment inherited across fork(), so two
 * processes genuinely map the same page with different mm.
 *
 * With FUTEX_*_PRIVATE this is a SILENT correctness bug, not an error: each
 * process derives its own key, waiters never see each other's wakes, and the
 * lock degrades to spin-only or hangs. Run with -p to see that failure mode.
 *
 * Build: gcc -O2 -Wall -o afl_mp_test afl_mp_test.c -lpthread
 * Usage: ./afl_mp_test [-n procs] [-d secs] [-p force-private]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include "ivh_adaptive_futex_lock.h"

struct shared {
	struct ivh_afl_lock lock;
	volatile unsigned long counter;   /* protected by lock */
	volatile int stop;
	unsigned long per_proc[64];
	/* Aggregated AFL stats. ivh_afl_stats is __thread, so each forked
	 * process keeps its own copy; children publish here before _exit(). */
	unsigned long sleeps, wakes_issued, woke_someone, woke_nobody, timeouts;
};

/* Long enough that processes genuinely contend and reach FUTEX_WAIT -- with a
 * trivial CS they would spin through the fast path and never sleep, and the
 * wake path (the only thing the futex key affects) would never be exercised. */
static void cs_work(volatile unsigned long *p, int n)
{
	for (int i = 0; i < n; i++)
		(*p)++;
}

int main(int argc, char **argv)
{
	int nproc = 8, dur = 5, force_private = 0, cs_len = 2000, c, i;
	while ((c = getopt(argc, argv, "n:d:pc:")) != -1) {
		switch (c) {
		case 'n': nproc = atoi(optarg); break;
		case 'd': dur = atoi(optarg); break;
		case 'p': force_private = 1; break;
		case 'c': cs_len = atoi(optarg); break;
		default: return 1;
		}
	}
	if (nproc < 2 || nproc > 64) { fprintf(stderr, "n must be 2..64\n"); return 1; }

	struct shared *sh = mmap(NULL, sizeof(*sh), PROT_READ | PROT_WRITE,
				 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sh == MAP_FAILED) { perror("mmap"); return 1; }
	memset(sh, 0, sizeof(*sh));

	ivh_afl_global_init();
	if (force_private)
		ivh_afl_init(&sh->lock);          /* the BUG: private key, shared memory */
	else
		ivh_afl_init_shared(&sh->lock);   /* correct for cross-process */

	printf("procs=%d duration=%ds futex=%s\n", nproc, dur,
	       force_private ? "PRIVATE (expected to misbehave)" : "SHARED");

	for (i = 0; i < nproc; i++) {
		pid_t p = fork();
		if (p < 0) { perror("fork"); return 1; }
		if (p == 0) {
			unsigned long n = 0;
			while (!sh->stop) {
				ivh_afl_lock(&sh->lock);
				sh->counter++;          /* racy iff the lock is broken */
				cs_work(&sh->counter, cs_len);
				sh->counter -= cs_len;
				ivh_afl_unlock(&sh->lock);
				n++;
			}
			sh->per_proc[i] = n;
			__sync_fetch_and_add(&sh->sleeps, ivh_afl_stats.sleeps);
			__sync_fetch_and_add(&sh->wakes_issued, ivh_afl_stats.wakes_issued);
			__sync_fetch_and_add(&sh->woke_someone, ivh_afl_stats.wakes_woke_someone);
			__sync_fetch_and_add(&sh->woke_nobody, ivh_afl_stats.wakes_woke_nobody);
			__sync_fetch_and_add(&sh->timeouts, ivh_afl_stats.timeouts);
			_exit(0);
		}
	}
	sleep(dur);
	sh->stop = 1;
	for (i = 0; i < nproc; i++) wait(NULL);

	unsigned long total = 0;
	for (i = 0; i < nproc; i++) total += sh->per_proc[i];
	printf("acquisitions counted by processes = %lu\n", total);
	printf("counter protected by the lock     = %lu\n", sh->counter);
	if (total == sh->counter)
		printf("RESULT: PASS - no lost update, mutual exclusion held across processes\n");
	else
		printf("RESULT: FAIL - %ld lost updates, lock did NOT provide mutual exclusion\n",
		       (long)total - (long)sh->counter);
	printf("throughput = %.0f acq/s\n", (double)total / dur);
	printf("\n  FUTEX_WAIT entered      : %lu\n", sh->sleeps);
	printf("  FUTEX_WAKE issued       : %lu\n", sh->wakes_issued);
	printf("  ...woke SOMEONE         : %lu\n", sh->woke_someone);
	printf("  ...woke NOBODY          : %lu\n", sh->woke_nobody);
	printf("  waits that TIMED OUT    : %lu\n", sh->timeouts);
	if (sh->wakes_issued)
		printf("  -> wake success rate    : %.1f%%\n",
		       100.0 * sh->woke_someone / sh->wakes_issued);
	puts(sh->woke_someone > 0
	     ? "  CROSS-PROCESS WAKES ARE WORKING"
	     : "  *** NO CROSS-PROCESS WAKE EVER SUCCEEDED -- futex key is wrong ***");
	return total == sh->counter ? 0 : 1;
}
