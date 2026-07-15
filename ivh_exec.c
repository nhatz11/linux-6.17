#include <stdio.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>

#ifndef PR_SET_IVH_ELIGIBLE
#define PR_SET_IVH_ELIGIBLE 79
#endif

#define IVH_TRACE_SYSCTL "/proc/sys/kernel/ivh_trace_enabled"

static int read_trace_enabled(char *buf, size_t len)
{
	FILE *f = fopen(IVH_TRACE_SYSCTL, "r");

	if (!f)
		return -1;
	if (!fgets(buf, len, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

static int write_trace_enabled(const char *val)
{
	FILE *f = fopen(IVH_TRACE_SYSCTL, "w");

	if (!f) {
		fprintf(stderr, "warning: open(%s): %s -- running without -t\n",
			IVH_TRACE_SYSCTL, strerror(errno));
		return -1;
	}
	fputs(val, f);
	fclose(f);
	return 0;
}

int main(int argc, char **argv)
{
	int trace = 0;
	char saved[32] = "0";
	char **cmd;
	pid_t pid;
	int status;

	if (argc >= 2 && strcmp(argv[1], "-t") == 0) {
		trace = 1;
		argv++;
		argc--;
	}

	if (argc < 2) {
		fprintf(stderr, "usage: %s [-t] <prog> [args...]\n", argv[0]);
		return 1;
	}
	cmd = &argv[1];

	if (!trace) {
		if (prctl(PR_SET_IVH_ELIGIBLE, 1, 0, 0, 0) != 0) {
			fprintf(stderr, "prctl(PR_SET_IVH_ELIGIBLE): %s\n", strerror(errno));
			return 1;
		}
		execvp(cmd[0], cmd);
		fprintf(stderr, "execvp(%s): %s\n", cmd[0], strerror(errno));
		return 127;
	}

	/*
	 * -t: flip ivh_trace_enabled on for exactly the duration of this
	 * command, restoring whatever value it had before -- so a traced run
	 * doesn't leave every trace_printk() in the IVH path paying its cost
	 * for everything that runs afterward. Needs fork+wait: execvp()
	 * replaces this process, so there'd be nothing left to restore the
	 * sysctl once the child exits if we didn't fork first.
	 */
	read_trace_enabled(saved, sizeof(saved));
	if (write_trace_enabled("1") != 0) {
		/* Couldn't enable tracing (e.g. no permission on the sysctl) --
		 * still run the command untraced rather than refusing outright. */
		if (prctl(PR_SET_IVH_ELIGIBLE, 1, 0, 0, 0) != 0) {
			fprintf(stderr, "prctl(PR_SET_IVH_ELIGIBLE): %s\n", strerror(errno));
			return 1;
		}
		execvp(cmd[0], cmd);
		fprintf(stderr, "execvp(%s): %s\n", cmd[0], strerror(errno));
		return 127;
	}

	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "fork: %s\n", strerror(errno));
		write_trace_enabled(saved);
		return 1;
	}
	if (pid == 0) {
		if (prctl(PR_SET_IVH_ELIGIBLE, 1, 0, 0, 0) != 0) {
			fprintf(stderr, "prctl(PR_SET_IVH_ELIGIBLE): %s\n", strerror(errno));
			_exit(1);
		}
		execvp(cmd[0], cmd);
		fprintf(stderr, "execvp(%s): %s\n", cmd[0], strerror(errno));
		_exit(127);
	}

	waitpid(pid, &status, 0);
	write_trace_enabled(saved);

	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	return 128;
}
