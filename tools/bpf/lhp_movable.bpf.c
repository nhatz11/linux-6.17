// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include "../../vmlinux.h"
#include "bpf_helpers.h"

extern const struct rq runqueues __ksym;

char LICENSE[] SEC("license") = "Dual BSD/GPL";

struct lhp_event {
	int cpu;
	int pid;
	int tgid;
	char comm[16];
	unsigned long cpumask_bits;
	int movable;
};

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 256 * 1024);
} rb SEC(".maps");

SEC("sched/cfs_sched_tick_end")
int BPF_PROG(check_movable, struct rq *rq, u64 now, unsigned int idle_cpus)
{
	struct task_struct *curr = rq->curr;

	if (curr == rq->idle)
		return 0;

	const cpumask_t *cpumask = curr->cpus_ptr;
	unsigned long bits = *(cpumask->bits);

	struct lhp_event *e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e)
		return 0;

	e->cpu = rq->cpu;
	e->pid = curr->pid;
	e->tgid = curr->tgid;
	__builtin_memcpy(e->comm, curr->comm, 16);
	e->cpumask_bits = bits;
	e->movable = __builtin_popcountl(bits) > 1 ? 1 : 0;

	bpf_ringbuf_submit(e, 0);
	return 0;
}
