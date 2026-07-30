// SPDX-License-Identifier: GPL-2.0
/*
 * ivh_exec — mark a process IVH-eligible (or explicitly excluded) and exec
 * a command under it.
 *
 * PF_IVH_ELIGIBLE is vestigial as of 2026-07-20: ivh_universal_eligible
 * (a global sysctl) is the sole switch for whether IVH does anything at
 * all, for anyone. This tool no longer controls eligibility by opting a
 * process IN -- it can only opt one OUT (via ivh_exclude, arg2=3 on the
 * PR_SET_IVH_ELIGIBLE prctl) when the global switch is on for everyone
 * else. Default mode still sets PF_IVH_ELIGIBLE (arg2=1) for introspection/
 * back-compat, but it has no effect on whether the process actually gets
 * IVH.
 *
 * Default mode (no -v): exec-replace this process with the target command,
 * after marking it excluded (-n) or leaving it alone (no -n, PF_IVH_ELIGIBLE
 * set but vestigial).
 *
 * Stats mode (-v / --stats): fork, set the child's PR_SET_IVH_ELIGIBLE bits
 * (observe-only always; excluded if -n, otherwise PF_IVH_ELIGIBLE for
 * back-compat), run it, and on exit report the delta in /proc/ivh_debug's
 * ivh_obs_total_holds/ivh_obs_stolen_holds -- the kernel's own cheap,
 * always-compiled cs_enter()/cs_exit() bookkeeping (kernel/locking/
 * spinlock.c), fed for this process because ivh_observe is set on it, not a
 * separate BPF trampoline attached to the whole system's raw-spinlock fast
 * path. Alongside that, -v reports the two halves of the timing story --
 * ivh_obs_cs_time_total_ns (how long locks are HELD, from the same
 * cs_enter()/cs_exit() pair) and ivh_obs_wait_total_ns/ivh_obs_wait_events
 * (how long acquisition WAITS, from the qspinlock slowpath,
 * kernel/locking/qspinlock.c) -- so a run can show whether a mechanism
 * actually shortened critical sections or contention rather than only moving
 * host preemption around. Note the two averages have different denominators
 * on purpose: CS time averages over every observed hold, wait time only over
 * acquisitions that were actually contended (uncontended fast-path
 * acquisitions never enter the slowpath and are excluded from the
 * denominator, not averaged in as zero) -- see the long comment above
 * ivh_obs_wait_begin() in kernel/locking/qspinlock.c.
 *
 * 2026-07-25: -v also reports the TAIL of the CS-hold-time distribution
 * (p50/p90/p95), read from ivh_obs_cs_hist, the kernel's log2-bucketed
 * histogram of the very same per-hold cs_ns that feeds cs_time_total_ns
 * (kernel/locking/spinlock.c). The average alone hides exactly the holds that
 * matter: real workloads average a couple hundred nanoseconds per hold, but a
 * hold actually caught by host steal runs past the 100us
 * IVH_HOT_STEAL_FLOOR_NS -- three orders of magnitude out, at well under 1% of
 * holds, so it moves the mean essentially not at all and only shows up in the
 * tail. Preempted and non-preempted holds are mixed in one histogram on
 * purpose: the question is where the combined worst case sits. The kernel
 * exposes raw bucket counts only; the cumulative-distribution walk that turns
 * them into percentiles happens here, off the before/after delta, for the same
 * reason the averages are computed here rather than read from the kernel's
 * lifetime *_avg_ns lines. (2026-07-20: replaces
 * an earlier BPF-based version of this tool.
 * That version's fentry/fexit hooks on the _raw_spin_lock and
 * _raw_spin_unlock family fired for every caller system-wide, inflating a
 * true ~12s hackbench run to ~16-21s, and its shadow per-task depth
 * tracking couldn't see the rq-lock ownership transfer across context
 * switches (acquired by prev, released by next in finish_lock_switch())
 * that the kernel's own lock_depth accounting has explicit fixups for --
 * every cross-CPU reschedule of the observed thread manufactured a fake
 * "stolen hold" spanning its whole off-CPU time, diffing two different
 * vCPUs' steal counters. Since IVH migrations are themselves extra forced
 * cross-CPU reschedules, that bias scaled with how much IVH was actually
 * working, which is what made the tool briefly report IVH making HP%
 * worse.)
 *
 * IMPORTANT LIMITATION: ivh_obs_total_holds/ivh_obs_stolen_holds are global
 * per-CPU counters, not scoped to a TGID -- any other ivh_observe'd process
 * (or, if ivh_universal_eligible is on, ANY other non-excluded process on
 * the system) contributes to the same counters during the measurement
 * window. This tool is meant for measuring one workload in isolation.
 *
 * No-IVH mode (-n / --no-ivh): set ivh_exclude on the wrapped process, so it
 * stays excluded from migration eligibility and the Hot Threads EWMA feed
 * even if ivh_universal_eligible is on for the rest of the system -- there
 * is no other way to opt one specific process out of that global switch.
 * Combine with -v to profile a workload's real host preemptions with IVH's
 * protection turned off, as a direct comparison against the same workload
 * run with -v alone (IVH on).
 *   ivh_exec -v hackbench        -- IVH on, with stats
 *   ivh_exec -v -n hackbench     -- IVH off, with stats
 *   ivh_exec -n hackbench        -- IVH off, no stats (same as unwrapped)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/prctl.h>
#include <sys/wait.h>

#ifndef PR_SET_IVH_ELIGIBLE
#define PR_SET_IVH_ELIGIBLE 79
#endif

/* PR_SET_IVH_ELIGIBLE arg2 values (kernel/sys.c): 1 = full eligibility,
 * 2 = observe-only (ivh_observe), independent bits, call once per bit. */
static int prctl_ivh(int arg2)
{
	if (prctl(PR_SET_IVH_ELIGIBLE, arg2, 0, 0, 0) != 0) {
		fprintf(stderr, "prctl(PR_SET_IVH_ELIGIBLE, %d): %s\n",
			arg2, strerror(errno));
		return -1;
	}
	return 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [-v|--stats] [-n|--no-ivh] <prog> [args...]\n"
		"  -v, --stats    also report host-preemption, average CS hold\n"
		"                 time, average contended-acquisition wait time,\n"
		"                 and the CS-hold-time tail (p50/p90/p95) for the\n"
		"                 wrapped process's real kernel lock holds on exit\n"
		"  -n, --no-ivh   set ivh_exclude -- wrapped process stays IVH-off\n"
		"                 even if ivh_universal_eligible is on globally. Stats\n"
		"                 (-v) still work if combined, since they don't\n"
		"                 depend on eligibility.\n",
		argv0);
}

/* ------------------------------------------------------------------ */
/* Stats mode (/proc/ivh_debug delta)                                  */
/* ------------------------------------------------------------------ */

/* Must match IVH_OBS_CS_HIST_BUCKETS in include/linux/bpf_sched.h. A kernel
 * with a different bucket count is detected at parse time (the line simply
 * won't yield this many fields) and degrades like any other missing counter,
 * rather than silently reporting percentiles off a misaligned axis. */
#define IVH_OBS_CS_HIST_BUCKETS 32

/* One snapshot of the kernel's observe-only counters. Only the raw totals
 * are read; averages are computed from the before/after DELTA, never from
 * the kernel's own *_avg_ns lines (those are lifetime averages, useful when
 * reading /proc directly but wrong for a scoped run). Same rule applies to
 * cs_hist: the percentile is computed from the per-bucket delta, so it
 * describes this run and not the machine's uptime. */
struct obs_counters {
	unsigned long long total_holds;
	unsigned long long stolen_holds;
	unsigned long long cs_time_ns;
	unsigned long long wait_events;
	unsigned long long wait_ns;
	unsigned long long cs_hist[IVH_OBS_CS_HIST_BUCKETS];
	int have_timing;	/* 0 on a kernel without the timing counters */
	int have_hist;		/* 0 on a kernel without ivh_obs_cs_hist */
};

/* Lower/upper nanosecond bound of log2 bucket b, rendered for printing.
 * Bucket b covers 2^b .. 2^(b+1)-1 ns, except b == 0 (0-1 ns, which also
 * absorbs zero-length holds) and the top bucket, which saturates and is
 * therefore open-ended -- see kernel/locking/spinlock.c. */
static void hist_bucket_str(int b, char *buf, size_t n)
{
	unsigned long long lo = (b == 0) ? 0ULL : 1ULL << b;

	if (b == IVH_OBS_CS_HIST_BUCKETS - 1)
		snprintf(buf, n, ">= %llu", lo);
	else
		snprintf(buf, n, "%llu-%llu", lo, (1ULL << (b + 1)) - 1ULL);
}

/* Index of the bucket containing the pct'th percentile of a distribution of
 * `total` samples: walk buckets low to high accumulating counts, stop at the
 * first bucket whose running total reaches pct% of the whole. Integer-only
 * (cum * 100 >= total * pct) to avoid a rounding wobble deciding which side
 * of a boundary a percentile lands on. Returns -1 for an empty histogram. */
static int hist_percentile_bucket(const unsigned long long *hist,
				  unsigned long long total, unsigned int pct)
{
	unsigned long long cum = 0;
	int b;

	if (!total)
		return -1;
	for (b = 0; b < IVH_OBS_CS_HIST_BUCKETS; b++) {
		cum += hist[b];
		if (cum * 100ULL >= total * (unsigned long long)pct)
			return b;
	}
	return IVH_OBS_CS_HIST_BUCKETS - 1;	/* unreachable: cum == total */
}

static int read_obs_counters(struct obs_counters *c)
{
	FILE *f;
	/* Wide enough for the whole ivh_obs_cs_hist line in one fgets(): 32
	 * u64s at 20 digits plus separators is under 700 bytes. A short buffer
	 * would split that line and the tail would be re-parsed as a bogus
	 * record, so this is sized deliberately, not by habit. */
	char line[1024];
	int got_total = 0, got_stolen = 0;
	int got_cs = 0, got_wait_ev = 0, got_wait_ns = 0;

	f = fopen("/proc/ivh_debug", "r");
	if (!f) {
		fprintf(stderr, "open /proc/ivh_debug: %s\n", strerror(errno));
		return -1;
	}

	memset(c, 0, sizeof(*c));
	while (fgets(line, sizeof(line), f)) {
		unsigned long long v;

		if (sscanf(line, "ivh_obs_total_holds: %llu", &v) == 1) {
			c->total_holds = v;
			got_total = 1;
		} else if (sscanf(line, "ivh_obs_stolen_holds: %llu", &v) == 1) {
			c->stolen_holds = v;
			got_stolen = 1;
		} else if (sscanf(line, "ivh_obs_cs_time_total_ns: %llu", &v) == 1) {
			c->cs_time_ns = v;
			got_cs = 1;
		} else if (sscanf(line, "ivh_obs_wait_events: %llu", &v) == 1) {
			c->wait_events = v;
			got_wait_ev = 1;
		} else if (sscanf(line, "ivh_obs_wait_total_ns: %llu", &v) == 1) {
			c->wait_ns = v;
			got_wait_ns = 1;
		} else if (!strncmp(line, "ivh_obs_cs_hist:", 16)) {
			/* One line, IVH_OBS_CS_HIST_BUCKETS space-separated
			 * counts in bucket order. Parsed positionally with
			 * strtoull rather than sscanf so a kernel printing a
			 * different number of buckets is caught (have_hist
			 * stays 0) instead of being read into the wrong bins.
			 * The preceding "# ivh_obs_cs_hist bucket i = ..."
			 * comment line documents the boundaries for anyone
			 * reading /proc by hand; it can't match here because
			 * of the leading '#'. */
			char *p = line + 16;
			int b;

			for (b = 0; b < IVH_OBS_CS_HIST_BUCKETS; b++) {
				char *end;

				c->cs_hist[b] = strtoull(p, &end, 10);
				if (end == p)
					break;
				p = end;
			}
			c->have_hist = (b == IVH_OBS_CS_HIST_BUCKETS);
		}
	}
	fclose(f);

	if (!got_total || !got_stolen) {
		fprintf(stderr, "/proc/ivh_debug: ivh_obs_* counters not found "
				"(old kernel?)\n");
		return -1;
	}
	/* Timing counters are newer than the hold counters -- degrade to the
	 * host-preemption section alone rather than failing outright. */
	c->have_timing = got_cs && got_wait_ev && got_wait_ns;
	return 0;
}

static int run_with_stats(char **cmd, int eligible)
{
	struct obs_counters before, after;
	unsigned long long total, stolen;
	unsigned long long cs_ns, wait_ev, wait_ns;
	pid_t child;
	int wstatus;

	if (read_obs_counters(&before))
		return 1;

	child = fork();
	if (child < 0) {
		fprintf(stderr, "fork: %s\n", strerror(errno));
		return 1;
	}

	if (child == 0) {
		/* Child: always set observe-only so cs_enter()/cs_exit() track
		 * this process's real kernel lock holds. Then either mark it
		 * eligible (arg2=1, vestigial now but harmless -- kept for
		 * introspection/back-compat) or, for -n, explicitly opt it out
		 * (arg2=3, ivh_exclude) so it stays excluded even if
		 * ivh_universal_eligible is on for the rest of the system --
		 * the global switch alone can't do that per-process. */
		if (prctl_ivh(2))
			_exit(127);
		if (eligible) {
			if (prctl_ivh(1))
				_exit(127);
		} else {
			if (prctl_ivh(3))
				_exit(127);
		}
		execvp(cmd[0], cmd);
		fprintf(stderr, "execvp(%s): %s\n", cmd[0], strerror(errno));
		_exit(127);
	}

	while (waitpid(child, &wstatus, 0) < 0 && errno == EINTR)
		;

	if (read_obs_counters(&after)) {
		/* Still report the workload's own exit status even if we
		 * can't report stats. */
		goto done;
	}

	total = after.total_holds - before.total_holds;
	stolen = after.stolen_holds - before.stolen_holds;
	cs_ns = after.cs_time_ns - before.cs_time_ns;
	wait_ev = after.wait_events - before.wait_events;
	wait_ns = after.wait_ns - before.wait_ns;

	printf("\n");
	printf("HOST-level steal during real kernel lock holds "
	       "(per-CPU steal_time delta, ground truth):\n");
	printf("  Host-preempted CS cycles : %llu / %llu  (%.4f%%)\n",
	       stolen, total,
	       total ? 100.0 * (double)stolen / (double)total : 0.0);
	printf("  (steal floor: 100000 ns / 100us, matches kernel "
	       "IVH_HOT_STEAL_FLOOR_NS)\n");
	printf("  (global counters, not TGID-scoped -- see source comment "
	       "if anything else was concurrently eligible/observed)\n");

	if (!before.have_timing || !after.have_timing) {
		printf("\n");
		printf("(no CS/wait timing: kernel lacks ivh_obs_cs_time_total_ns/"
		       "ivh_obs_wait_* counters)\n");
		goto done;
	}

	printf("\n");
	printf("CS and contention timing on the same real kernel lock holds:\n");
	printf("  Total CS hold time : %llu ns  (avg: %llu ns over %llu holds)\n",
	       cs_ns, total ? cs_ns / total : 0ULL, total);
	printf("  Total wait time    : %llu ns  (avg: %llu ns over %llu contended "
	       "acquisitions)\n",
	       wait_ns, wait_ev ? wait_ns / wait_ev : 0ULL, wait_ev);
	printf("  (CS time averages over EVERY observed hold; wait time only over\n");
	printf("   acquisitions that entered the qspinlock slowpath -- uncontended\n");
	printf("   fast-path acquisitions are excluded from that denominator, not\n");
	printf("   counted as zero-wait, so the fast path stays free of any probe)\n");

	/* Tail of the CS-hold-time distribution. Degrades the same way the
	 * timing block above does: an older kernel simply has no cs_hist line,
	 * so say so once and move on rather than failing the run. */
	if (before.have_hist && after.have_hist) {
		unsigned long long hist[IVH_OBS_CS_HIST_BUCKETS];
		unsigned long long hist_total = 0;
		int b, p50, p90, p95, top = -1;
		/* 48 bytes: the widest real rendering is bucket 30,
		 * "1073741824-2147483647" (21 chars), but sized so the
		 * compiler can prove no truncation without knowing b's range. */
		char b50[48], b90[48], b95[48], btop[48];

		for (b = 0; b < IVH_OBS_CS_HIST_BUCKETS; b++) {
			hist[b] = after.cs_hist[b] - before.cs_hist[b];
			hist_total += hist[b];
			if (hist[b])
				top = b;
		}

		/* hist_total is the histogram's own denominator rather than
		 * `total` above: both count the same holds, but using the
		 * distribution's own sum keeps the percentile walk internally
		 * consistent even if the two lines were read a hair apart. */
		p50 = hist_percentile_bucket(hist, hist_total, 50);
		p90 = hist_percentile_bucket(hist, hist_total, 90);
		p95 = hist_percentile_bucket(hist, hist_total, 95);

		printf("\n");
		printf("  CS hold time distribution (log2-bucketed, N=%llu holds):\n",
		       hist_total);
		if (hist_total) {
			hist_bucket_str(p50, b50, sizeof(b50));
			hist_bucket_str(p90, b90, sizeof(b90));
			hist_bucket_str(p95, b95, sizeof(b95));
			hist_bucket_str(top, btop, sizeof(btop));
			printf("    p50: ~%s ns\n", b50);
			printf("    p90: ~%s ns\n", b90);
			printf("    p95: ~%s ns\n", b95);
			printf("    max bucket reached: %s ns\n", btop);

			/* Every populated bucket from p95 onward: p50/p90/p95
			 * collapse the tail to 3 numbers, which hides the actual
			 * shape of exactly the region host preemption would show
			 * up in. Walk from the p95 bucket (not p95+1) so the
			 * boundary bucket's own count is visible for context, up
			 * through `top`; empty buckets in that range are skipped
			 * rather than printed as zero, since a log2 histogram over
			 * a wide dynamic range is naturally sparse out there and a
			 * long run of zero lines would bury the real entries. */
			if (p95 < top) {
				char brange[48];

				printf("    buckets p95..max (nonzero only):\n");
				for (b = p95; b <= top; b++) {
					if (!hist[b])
						continue;
					hist_bucket_str(b, brange, sizeof(brange));
					printf("      %-24s %12llu holds  (%.4f%%)\n",
					       brange, hist[b],
					       100.0 * (double)hist[b] / (double)hist_total);
				}
			}
		} else {
			printf("    (no holds observed in this window)\n");
		}
		printf("    (BUCKET-BOUNDARY APPROXIMATIONS, not exact percentiles: a\n");
		printf("     log2 histogram can only place a percentile inside the\n");
		printf("     power-of-2 range containing it -- the same caveat every\n");
		printf("     histogram-based percentile carries, e.g. Prometheus\n");
		printf("     histogram_quantile or HDR histogram)\n");
		printf("    (host-preempted and clean holds are mixed in one\n");
		printf("     distribution on purpose -- this is the combined worst case)\n");
	} else {
		printf("\n");
		printf("  (no CS hold time distribution: kernel lacks "
		       "ivh_obs_cs_hist)\n");
	}

done:
	if (WIFEXITED(wstatus))
		return WEXITSTATUS(wstatus);
	return 128 + WTERMSIG(wstatus);
}

int main(int argc, char **argv)
{
	int stats = 0;
	int eligible = 1;
	int i = 1;

	while (i < argc) {
		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--stats")) {
			stats = 1;
			i++;
		} else if (!strcmp(argv[i], "-n") || !strcmp(argv[i], "--no-ivh")) {
			eligible = 0;
			i++;
		} else {
			break;
		}
	}

	if (i >= argc) {
		usage(argv[0]);
		return 1;
	}

	if (stats)
		return run_with_stats(&argv[i], eligible);

	/* Default mode: exec-replace, exactly as the original wrapper.
	 * -n explicitly excludes (ivh_exclude) rather than just skipping
	 * the eligibility prctl -- needed so this still means "IVH off for
	 * this process" even when ivh_universal_eligible is on for the
	 * rest of the system, since that global switch has no other way to
	 * exclude one specific process. */
	if (eligible) {
		if (prctl_ivh(1))
			return 1;
	} else {
		if (prctl_ivh(3))
			return 1;
	}
	execvp(argv[i], &argv[i]);
	fprintf(stderr, "execvp(%s): %s\n", argv[i], strerror(errno));
	return 127;
}
