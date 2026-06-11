// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <bpf/libbpf.h>
#include "lhp_movable.skel.h"

#define LOG_BUF_SIZE (1024 * 1024)
static char verifier_log[LOG_BUF_SIZE];

static volatile int done;

struct lhp_event {
	int cpu;
	int pid;
	int tgid;
	char comm[16];
	unsigned long cpumask_bits;
	int movable;
};

static void sig_handler(int sig)
{
	done = 1;
}

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
	return vfprintf(stderr, format, args);
}

static void bump_memlock_rlimit(void)
{
	struct rlimit rlim_new = {
		.rlim_cur = RLIM_INFINITY,
		.rlim_max = RLIM_INFINITY,
	};
	if (setrlimit(RLIMIT_MEMLOCK, &rlim_new))
		fprintf(stderr, "Warning: failed to increase RLIMIT_MEMLOCK limit\n");
}

static int handle_event(void *ctx, void *data, size_t size)
{
	struct lhp_event *e = data;
	printf("CPU %d: pid %d tgid %d (%s) cpumask=0x%lx movable=%s\n",
	       e->cpu, e->pid, e->tgid, e->comm, e->cpumask_bits,
	       e->movable ? "yes" : "no");
	return 0;
}

int main(void)
{
	struct lhp_movable *skel;
	struct ring_buffer *rb = NULL;
	struct bpf_program *p;
	int err;

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	libbpf_set_print(libbpf_print_fn);
	bump_memlock_rlimit();

	skel = lhp_movable__open();
	if (!skel) {
		fprintf(stderr, "Failed to open BPF skeleton\n");
		return 1;
	}

	p = skel->progs.check_movable;
	bpf_program__set_type(p, BPF_PROG_TYPE_SCHED);
	bpf_program__set_expected_attach_type(p, BPF_SCHED);
	bpf_program__set_log_level(p, 2);
	bpf_program__set_log_buf(p, verifier_log, LOG_BUF_SIZE);

	err = lhp_movable__load(skel);
	if (err) {
		fprintf(stderr, "Failed to load BPF skeleton\n");
		fprintf(stderr, "Verifier log:\n%s\n", verifier_log);
		goto cleanup;
	}

	err = lhp_movable__attach(skel);
	if (err) {
		fprintf(stderr, "Failed to attach BPF skeleton\n");
		goto cleanup;
	}

	rb = ring_buffer__new(bpf_map__fd(skel->maps.rb), handle_event, NULL, NULL);
	if (!rb) {
		fprintf(stderr, "Failed to create ring buffer\n");
		err = -1;
		goto cleanup;
	}

	printf("Monitoring CPU affinity (Ctrl-C to stop).\n");

	while (!done) {
		err = ring_buffer__poll(rb, 100);
		if (err < 0 && err != -EINTR) {
			fprintf(stderr, "ring buffer poll error: %d\n", err);
			break;
		}
		err = 0;
	}

cleanup:
	ring_buffer__free(rb);
	lhp_movable__destroy(skel);
	return err ? 1 : 0;
}
