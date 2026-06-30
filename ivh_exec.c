#include <stdio.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <errno.h>
#include <string.h>

#ifndef PR_SET_IVH_ELIGIBLE
#define PR_SET_IVH_ELIGIBLE 79
#endif

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <prog> [args...]\n", argv[0]);
		return 1;
	}
	if (prctl(PR_SET_IVH_ELIGIBLE, 1, 0, 0, 0) != 0) {
		fprintf(stderr, "prctl(PR_SET_IVH_ELIGIBLE): %s\n", strerror(errno));
		return 1;
	}
	execvp(argv[1], &argv[1]);
	fprintf(stderr, "execvp(%s): %s\n", argv[1], strerror(errno));
	return 127;
}
