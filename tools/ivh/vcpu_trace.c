// SPDX-License-Identifier: GPL-2.0
/*
 * vcpu_trace -- like vcpu_gone, but records the individual gaps instead of
 * aggregating them, so the whole tick-gap estimator can be REPLAYED offline
 * against the same timeline (see replay_tks.py).
 *
 * Why this beats patching the kernel to histogram `excess`: the busy-spin loop
 * already observes, at ~20ns resolution, exactly the thing the tick estimator
 * can only sample at 1ms -- every interval in which this vCPU was not
 * executing. The kernel tick is a periodic timer on an absolute grid, so given
 * the gap list the tick delivery times are fully determined; nothing about the
 * estimator needs to be measured in-kernel, it can be computed. That also
 * means a parameter sweep costs microseconds of replay instead of 8s of wall
 * clock per cell, and every cell sees the IDENTICAL host load rather than a
 * fresh draw -- which is what made the earlier sweeps too noisy to rank.
 *
 * Uses rdtscp (the kernel's ivh_raw_tsc() is a raw rdtsc) so replayed
 * arithmetic matches the kernel's cycle domain exactly.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>
#include <x86intrin.h>

#define MAXG (8u*1024u*1024u)

int main(int argc, char **argv)
{
	int cpu      = argc > 1 ? atoi(argv[1]) : 0;
	double secs  = argc > 2 ? atof(argv[2]) : 15.0;
	int fifo     = argc > 3 ? atoi(argv[3]) : 1;
	unsigned long long khz = argc > 4 ? strtoull(argv[4],0,10) : 2200000ULL;
	const char *out = argc > 5 ? argv[5] : "/tmp/vcpu_trace.bin";
	unsigned long long minc = argc > 6 ? strtoull(argv[6],0,10) : 400ULL; /* ~180ns */

	cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set)) { perror("affinity"); return 1; }
	if (sched_getcpu() != cpu) { fprintf(stderr, "not pinned to %d\n", cpu); return 1; }
	if (fifo) {
		struct sched_param sp; sp.sched_priority = 1;
		if (sched_setscheduler(0, SCHED_FIFO, &sp)) {
			fprintf(stderr, "SCHED_FIFO failed: %s\n", strerror(errno)); return 1;
		}
	}
	unsigned long long *gs = malloc((size_t)MAXG*8), *gl = malloc((size_t)MAXG*8);
	if (!gs || !gl) { perror("malloc"); return 1; }

	unsigned long long dur = (unsigned long long)(secs * (double)khz * 1000.0);
	unsigned int aux;
	unsigned long long t0, t, prev, d, n = 0, samples = 0;
	t0 = prev = __rdtscp(&aux);
	for (;;) {
		t = __rdtscp(&aux);
		d = t - prev;
		samples++;
		if (d > minc && n < MAXG) { gs[n] = prev; gl[n] = d; n++; }
		prev = t;
		if ((samples & 0xffff) == 0 && (t - t0) >= dur) break;
	}
	unsigned long long t1 = __rdtscp(&aux);

	FILE *f = fopen(out, "wb");
	if (!f) { perror("fopen"); return 1; }
	/* header: magic, cpu, khz, t0, t1, n, minc */
	unsigned long long hdr[7] = { 0x56435047414F4E31ULL, (unsigned long long)cpu,
	                              khz, t0, t1, n, minc };
	fwrite(hdr, 8, 7, f);
	fwrite(gs, 8, n, f); fwrite(gl, 8, n, f);
	fclose(f);
	fprintf(stderr, "cpu=%d span_c=%llu gaps=%llu samples=%llu -> %s\n",
	        cpu, t1-t0, n, samples, out);
	return 0;
}
