/*
 * rare_candidate — simulate a background process that contends the real
 * wait_decay-feeding kernel lock path only a couple of times, then goes idle.
 *
 * Mechanism: sets PF_IVH_ELIGIBLE directly via prctl (same flag ivh_exec sets),
 * then issues a small burst of syscall(470)=ivh_cs_enter. With ivh_selection_trylock=0
 * and a concurrent NHextend3 workload, my_spinlock inside bpf_sched_pre_lock_migrate()
 * is genuinely contended -> qspinlock slowpath -> ivh_hot_note_wait_event() ->
 * feeds this task's ivh_wait_decay. After the burst it sleeps, doing NOTHING, so it
 * acquires no kernel raw_spinlocks and generates no cs_exit 0-samples.
 *
 * Usage: rare_candidate <burst_count> <idle_seconds>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#ifndef PR_SET_IVH_ELIGIBLE
#define PR_SET_IVH_ELIGIBLE 79
#endif
#ifndef PR_GET_IVH_ELIGIBLE
#define PR_GET_IVH_ELIGIBLE 80
#endif
#ifndef __NR_ivh_cs_enter
#define __NR_ivh_cs_enter 470
#endif

int main(int argc, char **argv)
{
	int burst = argc > 1 ? atoi(argv[1]) : 3;
	int idle  = argc > 2 ? atoi(argv[2]) : 60;
	int i;

	if (prctl(PR_SET_IVH_ELIGIBLE, 1, 0, 0, 0) != 0) {
		perror("prctl(PR_SET_IVH_ELIGIBLE)");
		return 1;
	}
	int el = prctl(PR_GET_IVH_ELIGIBLE, 0, 0, 0, 0);
	printf("rare_candidate pid=%d eligible=%d burst=%d idle=%d\n",
	       getpid(), el, burst, idle);
	fflush(stdout);

	/* Brief burst of genuine wait_decay-feeding events. Tight loop so the
	 * burst is over in well under a second; the whole point is a short
	 * moment of contention followed by long idle. */
	for (i = 0; i < burst; i++) {
		syscall(__NR_ivh_cs_enter);
	}
	printf("burst done at t=0; now idling %d s doing NOTHING (no syscalls)\n", idle);
	fflush(stdout);

	/* Optional heartbeat mode: env RC_HEARTBEAT=<sec> makes the task wake
	 * every <sec> seconds and issue ONE syscall(470) as an in-band probe of
	 * its own wait_decay, for <idle> total seconds. Default (unset/0) = one
	 * long pure nanosleep (task runs zero code during idle), then a single
	 * wake syscall so a kprobe can read the frozen value. */
	char *hb = getenv("RC_HEARTBEAT");
	int hbsec = hb ? atoi(hb) : 0;
	if (hbsec > 0) {
		int elapsed = 0;
		while (elapsed < idle) {
			struct timespec ts = { .tv_sec = hbsec, .tv_nsec = 0 };
			nanosleep(&ts, NULL);
			syscall(__NR_ivh_cs_enter);   /* heartbeat probe */
			elapsed += hbsec;
		}
	} else {
		struct timespec ts = { .tv_sec = idle, .tv_nsec = 0 };
		nanosleep(&ts, NULL);
		syscall(__NR_ivh_cs_enter);       /* single wake probe */
	}

	printf("rare_candidate pid=%d done\n", getpid());
	return 0;
}
