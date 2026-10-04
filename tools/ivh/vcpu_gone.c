// SPDX-License-Identifier: GPL-2.0
/*
 * vcpu_gone -- measure, from inside the guest, how long THIS vCPU was gone.
 *
 * Pin to one vCPU, busy-loop on clock_gettime(CLOCK_MONOTONIC) (vDSO, ~20ns),
 * and record every gap between consecutive reads. In a tight loop consecutive
 * reads are tens of ns apart, so any gap above the threshold means this thread
 * did not run: either local interference (interrupt, kernel thread) or the host
 * descheduled the vCPU.
 *
 * Busy-spinning is deliberate, not incidental: the vCPU must look exactly as
 * runnable to the host as the loaded ones, or it would be preempted on
 * different terms and the comparison would be meaningless.
 *
 * Reports total elapsed, summed gap time at several thresholds, and a log2
 * histogram, so the analysis can pick a threshold rather than baking one in.
 *
 * The kernel-side quantity to compare against (kernel/sched/core.c):
 *     avail = elapsed - idle;  used = avail - steal
 *     capacity = EMA(used * 1024 / avail)
 * so with idle ~ 0 here,  1 - capacity/1024  IS the kernel's claimed
 * gone-fraction, and rq->ivh_tks_steal_ns is its claimed absolute steal.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#define NB 32
static inline unsigned long long now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned long long)t.tv_sec * 1000000000ULL + t.tv_nsec;
}

int main(int argc, char **argv)
{
	int cpu = argc > 1 ? atoi(argv[1]) : 0;
	double secs = argc > 2 ? atof(argv[2]) : 15.0;
	int fifo = argc > 3 ? atoi(argv[3]) : 0;   /* argv[3]=1 -> SCHED_FIFO prio 1 */
	cpu_set_t set;
	unsigned long long hist_n[NB] = {0}, hist_ns[NB] = {0};
	unsigned long long t0, t, prev, d, total_gap = 0, samples = 0, maxgap = 0;
	/* thresholds in ns for the summary */
	const unsigned long long TH[] = {1000, 5000, 20000, 50000, 100000, 500000};
	unsigned long long sum[6] = {0}, cnt[6] = {0};

	CPU_ZERO(&set); CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set)) { perror("affinity"); return 1; }
	if (sched_getcpu() != cpu) { fprintf(stderr, "not pinned to %d\n", cpu); return 1; }
	/* SCHED_FIFO preempts every normal and SCHED_IDLE guest thread, so a gap
	 * then means the vCPU itself was not on a physical CPU -- host preemption,
	 * not guest scheduling. Caveat: RT throttling still parks us for
	 * (period-runtime) once per period; those gaps are ~50ms here, ~500x
	 * larger than the ~100us host quanta, so they are separable by size and
	 * are reported in the histogram rather than hidden. */
	if (fifo) {
		struct sched_param sp; sp.sched_priority = 1;
		if (sched_setscheduler(0, SCHED_FIFO, &sp)) {
			fprintf(stderr, "SCHED_FIFO failed: %s\n", strerror(errno)); return 1;
		}
	}

	t0 = prev = now_ns();
	for (;;) {
		t = now_ns();
		d = t - prev;
		prev = t;
		samples++;
		if (d > 200) {                      /* ignore the loop's own ~20-60ns */
			int b = 0; unsigned long long x = d;
			while (x >>= 1) b++;
			if (b >= NB) b = NB - 1;
			hist_n[b]++; hist_ns[b] += d;
			if (d > maxgap) maxgap = d;
			total_gap += d;
			for (int i = 0; i < 6; i++)
				if (d >= TH[i]) { sum[i] += d; cnt[i]++; }
		}
		if ((samples & 0xffff) == 0 && (t - t0) >= (unsigned long long)(secs*1e9)) break;
	}
	{
		unsigned long long el = now_ns() - t0;
		printf("cpu=%d elapsed_ns=%llu samples=%llu maxgap_ns=%llu\n",
		       cpu, el, samples, maxgap);
		for (int i = 0; i < 6; i++)
			printf("thresh_ns=%llu gone_ns=%llu events=%llu gone_frac=%.6f\n",
			       TH[i], sum[i], cnt[i], (double)sum[i]/(double)el);
		for (int b = 0; b < NB; b++)
			if (hist_n[b]) printf("hist b=%d ge_ns=%llu n=%llu ns=%llu\n",
					      b, 1ULL<<b, hist_n[b], hist_ns[b]);
	}
	return 0;
}
