// SPDX-License-Identifier: LGPL-2.1
/*
 * sched_state_test.c
 *
 * Demonstrate the rseq sched_state feature: one thread spins in a busy loop
 * (main) while a second thread (test_thread) periodically reads the main
 * thread's ON_CPU bit via the registered rseq_abi_sched_state and prints it.
 *
 * Expected output: ON_CPU=1 almost every sample while the main thread busy-
 * loops, because it is almost never preempted for the full 100 ms poll window.
 *
 * To observe ON_CPU=0, replace the busy loop with the commented-out poll()
 * calls at the bottom of main(): the main thread will be off-CPU during each
 * 75 ms sleep and the monitoring thread will see ON_CPU=0.
 *
 * Copyright (C) 2024 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#define _GNU_SOURCE
#include <assert.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <poll.h>

#include "rseq.h"

/* Pointer to the main thread's sched_state; set before the test thread starts. */
static struct rseq_abi_sched_state *target_thread_state;

static void show_sched_state(struct rseq_abi_sched_state *rseq_thread_state)
{
	uint32_t state;

	state = rseq_thread_state->state;
	printf("Target thread: %u, ON_CPU=%d\n",
	       rseq_thread_state->tid,
	       !!(state & RSEQ_ABI_SCHED_STATE_FLAG_ON_CPU));
}

static void *test_thread(void *arg)
{
	int i;

	for (i = 0; i < 1000; i++) {
		show_sched_state(target_thread_state);
		(void) poll(NULL, 0, 100);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t test_thread_id;
	int i;

	if (rseq_register_current_thread()) {
		fprintf(stderr,
			"Error: rseq_register_current_thread() failed(%d): %s\n",
			errno, strerror(errno));
		goto init_thread_error;
	}
	target_thread_state = rseq_get_sched_state(rseq_get_abi());

	pthread_create(&test_thread_id, NULL, test_thread, NULL);

	/* Busy loop: main thread stays on-CPU, monitoring thread sees ON_CPU=1. */
	for (i = 0; i < 1000000000; i++)
		rseq_barrier();

	/*
	 * Alternatively, replace the busy loop above with sleeping calls to
	 * observe ON_CPU=0:
	 *
	 *   for (i = 0; i < 10000; i++)
	 *       (void) poll(NULL, 0, 75);
	 */

	pthread_join(test_thread_id, NULL);

	if (rseq_unregister_current_thread()) {
		fprintf(stderr,
			"Error: rseq_unregister_current_thread() failed(%d): %s\n",
			errno, strerror(errno));
		goto init_thread_error;
	}
	return 0;

init_thread_error:
	return -1;
}
