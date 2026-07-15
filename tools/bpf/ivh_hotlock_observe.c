// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
//
// Checkpoint H validation loader. Prints each observed (lock, waiters,
// verdict) event live, and on exit (Ctrl-C) dumps a per-TGID hot/cold
// breakdown -- this is the pass/fail table for Checkpoint H
// (tools/bpf/docs/ivh_build_and_evaluation_plan_2026-07-11.md).
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "ivh_hotlock_observe.skel.h"

static volatile int done;

struct hot_event {
	unsigned int tgid;
	unsigned int pid;
	char comm[16];
	unsigned long long lock_addr;
	unsigned long long waiters;
	unsigned long long verdict;
};

struct tgid_counts {
	unsigned long long hot;
	unsigned long long cold;
};

#define MAX_TGIDS 4096
static unsigned int seen_tgid[MAX_TGIDS];
static char seen_comm[MAX_TGIDS][16];
static int nr_seen;

static void sig_handler(int sig) { done = 1; }

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
	return vfprintf(stderr, format, args);
}

static void bump_memlock_rlimit(void)
{
	struct rlimit rlim_new = { .rlim_cur = RLIM_INFINITY, .rlim_max = RLIM_INFINITY };
	setrlimit(RLIMIT_MEMLOCK, &rlim_new);
}

static void record_comm(unsigned int tgid, const char *comm)
{
	int i;

	for (i = 0; i < nr_seen; i++) {
		if (seen_tgid[i] == tgid) {
			memcpy(seen_comm[i], comm, 16);
			return;
		}
	}
	if (nr_seen < MAX_TGIDS) {
		seen_tgid[nr_seen] = tgid;
		memcpy(seen_comm[nr_seen], comm, 16);
		nr_seen++;
	}
}

static int handle_event(void *ctx, void *data, size_t size)
{
	struct hot_event *e = data;

	record_comm(e->tgid, e->comm);
	printf("tgid=%-8u pid=%-8u comm=%-16s lock=%#llx waiters=%llu verdict=%s\n",
	       e->tgid, e->pid, e->comm, e->lock_addr, e->waiters,
	       e->verdict ? "HOT" : "cold");
	return 0;
}

int main(void)
{
	struct ivh_hotlock_observe *skel;
	struct ring_buffer *rb = NULL;
	int err;

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	libbpf_set_print(libbpf_print_fn);
	bump_memlock_rlimit();

	skel = ivh_hotlock_observe__open_and_load();
	if (!skel) {
		fprintf(stderr, "Failed to open/load BPF skeleton\n");
		return 1;
	}

	err = ivh_hotlock_observe__attach(skel);
	if (err) {
		fprintf(stderr, "Failed to attach BPF skeleton (is __x64_sys_ivh_cs_enter really live? check /proc/kallsyms)\n");
		goto cleanup;
	}

	rb = ring_buffer__new(bpf_map__fd(skel->maps.rb), handle_event, NULL, NULL);
	if (!rb) {
		fprintf(stderr, "Failed to create ring buffer\n");
		err = -1;
		goto cleanup;
	}

	printf("Observing ivh_cs_enter Hotlock verdicts (Ctrl-C for summary).\n");

	while (!done) {
		err = ring_buffer__poll(rb, 100);
		if (err < 0 && err != -EINTR) {
			fprintf(stderr, "ring buffer poll error: %d\n", err);
			break;
		}
		err = 0;
	}

	printf("\n===== Checkpoint H per-TGID summary =====\n");
	printf("%-8s %-16s %10s %10s %8s\n", "tgid", "comm", "hot", "cold", "hot%");
	{
		int map_fd = bpf_map__fd(skel->maps.tgid_stats);
		unsigned int key, next_key;
		struct tgid_counts counts;
		int has_key = 0;

		while (bpf_map_get_next_key(map_fd, has_key ? &key : NULL, &next_key) == 0) {
			key = next_key;
			has_key = 1;
			if (bpf_map_lookup_elem(map_fd, &key, &counts) == 0) {
				unsigned long long total = counts.hot + counts.cold;
				double pct = total ? (100.0 * counts.hot / total) : 0.0;
				const char *comm = "?";
				int i;

				for (i = 0; i < nr_seen; i++) {
					if (seen_tgid[i] == key) { comm = seen_comm[i]; break; }
				}
				printf("%-8u %-16s %10llu %10llu %7.2f%%\n",
				       key, comm, counts.hot, counts.cold, pct);
			}
		}
	}

cleanup:
	ring_buffer__free(rb);
	ivh_hotlock_observe__destroy(skel);
	return err ? 1 : 0;
}
