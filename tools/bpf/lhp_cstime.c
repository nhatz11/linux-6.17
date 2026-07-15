// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <bpf/libbpf.h>
#include "lhp_cstime.skel.h"

#define LOG_BUF_SIZE (1024 * 1024)
static char verifier_log[LOG_BUF_SIZE];

static volatile int done;

struct cs_event {
	int cpu;
	int pid;
	int tgid;
	char comm[16];
	unsigned long long duration_ns;
	unsigned int cs_type;
	unsigned int host_preempted;
};

static unsigned long long kern_cs_count;
static unsigned long long kern_cs_host_preempted;

static void sig_handler(int sig) { done = 1; }

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
		fprintf(stderr, "Warning: failed to increase RLIMIT_MEMLOCK\n");
}

static int handle_event(void *ctx, void *data, size_t size)
{
	struct cs_event *e = data;
	const char *type = e->cs_type == 0 ? "userspace" : "kernel";

	if (e->cs_type == 1) {
		kern_cs_count++;
		if (e->host_preempted)
			kern_cs_host_preempted++;
	}

	printf("pid %d (%s) cpu %d [%s] duration=%llu ns%s\n",
	       e->pid, e->comm, e->cpu, type, e->duration_ns,
	       (e->cs_type == 1 && e->host_preempted) ? "  HOST-PREEMPTED" : "");
	return 0;
}

int main(void)
{
	struct lhp_cstime *skel;
	struct ring_buffer *rb = NULL;
	struct bpf_program *p;
	int err;

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	libbpf_set_print(libbpf_print_fn);
	bump_memlock_rlimit();

	skel = lhp_cstime__open();
	if (!skel) {
		fprintf(stderr, "Failed to open BPF skeleton\n");
		return 1;
	}

	/* tick program — BPF_PROG_TYPE_SCHED */
	p = skel->progs.track_user_cs;
	bpf_program__set_type(p, BPF_PROG_TYPE_SCHED);
	bpf_program__set_expected_attach_type(p, BPF_SCHED);
	bpf_program__set_log_level(p, 2);
	bpf_program__set_log_buf(p, verifier_log, LOG_BUF_SIZE);

	/* fexit/_raw_spin_lock and fentry/_raw_spin_unlock use default types */

	err = lhp_cstime__load(skel);
	if (err) {
		fprintf(stderr, "Failed to load BPF skeleton\n");
		fprintf(stderr, "Verifier log:\n%s\n", verifier_log);
		goto cleanup;
	}

	err = lhp_cstime__attach(skel);
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

	printf("Monitoring critical sections (Ctrl-C to stop).\n");

	while (!done) {
		err = ring_buffer__poll(rb, 100);
		if (err < 0 && err != -EINTR) {
			fprintf(stderr, "ring buffer poll error: %d\n", err);
			break;
		}
		err = 0;
	}

	printf("\nKernel CS host-preempted (PF_IVH_ELIGIBLE tasks only): %llu / %llu (%.4f%%)\n",
	       kern_cs_host_preempted, kern_cs_count,
	       kern_cs_count ? 100.0 * kern_cs_host_preempted / kern_cs_count : 0.0);

cleanup:
	ring_buffer__free(rb);
	lhp_cstime__destroy(skel);
	return err ? 1 : 0;
}
