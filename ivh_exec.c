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
 * path. (2026-07-20: replaces an earlier BPF-based version of this tool.
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
		"  -v, --stats    also report host-preemption of the wrapped\n"
		"                 process's real kernel lock holds on exit\n"
		"  -n, --no-ivh   set ivh_exclude -- wrapped process stays IVH-off\n"
		"                 even if ivh_universal_eligible is on globally. Stats\n"
		"                 (-v) still work if combined, since they don't\n"
		"                 depend on eligibility.\n",
		argv0);
}

/* ------------------------------------------------------------------ */
/* Stats mode (/proc/ivh_debug delta)                                  */
/* ------------------------------------------------------------------ */

static int read_obs_counters(unsigned long long *total, unsigned long long *stolen)
{
	FILE *f;
	char line[256];
	int got_total = 0, got_stolen = 0;

	f = fopen("/proc/ivh_debug", "r");
	if (!f) {
		fprintf(stderr, "open /proc/ivh_debug: %s\n", strerror(errno));
		return -1;
	}

	*total = 0;
	*stolen = 0;
	while (fgets(line, sizeof(line), f)) {
		unsigned long long v;

		if (sscanf(line, "ivh_obs_total_holds: %llu", &v) == 1) {
			*total = v;
			got_total = 1;
		} else if (sscanf(line, "ivh_obs_stolen_holds: %llu", &v) == 1) {
			*stolen = v;
			got_stolen = 1;
		}
	}
	fclose(f);

	if (!got_total || !got_stolen) {
		fprintf(stderr, "/proc/ivh_debug: ivh_obs_* counters not found "
				"(old kernel?)\n");
		return -1;
	}
	return 0;
}

static int run_with_stats(char **cmd, int eligible)
{
	unsigned long long total_before, stolen_before;
	unsigned long long total_after, stolen_after;
	unsigned long long total, stolen;
	pid_t child;
	int wstatus;

	if (read_obs_counters(&total_before, &stolen_before))
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

	if (read_obs_counters(&total_after, &stolen_after)) {
		/* Still report the workload's own exit status even if we
		 * can't report stats. */
		goto done;
	}

	total = total_after - total_before;
	stolen = stolen_after - stolen_before;

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
