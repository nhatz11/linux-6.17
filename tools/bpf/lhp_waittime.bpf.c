// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include "../../vmlinux.h"
#include "bpf_helpers.h"

extern const struct rq runqueues __ksym;

char LICENSE[] SEC("license") = "Dual BSD/GPL";

#define CR_COUNTER_OFFSET 28
#define RSEQ_CR_IN_CS_MASK 0xFFFFFFFEu
#define RSEQ_MIN_LEN 32

struct wt_event {
	int cpu;
	int pid;
	int tgid;
	char comm[16];
	u64 cs_duration_ns;
	u64 wait_time_ns;
};

struct wt_entry {
	u64 entry_ts;
	u64 wait_anchor;
};

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 256 * 1024);
} rb SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 10240);
	__type(key, u32);
	__type(value, struct wt_entry);
} user_cs_map SEC(".maps");

static void emit_event(u32 pid, u32 tgid, char *comm, int cpu,
		       u64 cs_duration_ns, u64 wait_time_ns)
{
	struct wt_event *e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e)
		return;
	e->cpu = cpu;
	e->pid = pid;
	e->tgid = tgid;
	__builtin_memcpy(e->comm, comm, 16);
	e->cs_duration_ns = cs_duration_ns;
	e->wait_time_ns = wait_time_ns;
	bpf_ringbuf_submit(e, 0);
}

SEC("sched/cfs_sched_tick_end")
int BPF_PROG(track_waittime, struct rq *rq, u64 now, unsigned int idle_cpus)
{
	struct task_struct *curr = rq->curr;

	if (curr == rq->idle)
		return 0;

	struct rseq *rseq_ptr = curr->rseq;
	u32 rseq_len = curr->rseq_len;

	if (!rseq_ptr || rseq_len < RSEQ_MIN_LEN)
		return 0;

	u32 cr_counter = 0;
	if (bpf_probe_read_user(&cr_counter, sizeof(cr_counter),
				(void *)rseq_ptr + CR_COUNTER_OFFSET))
		return 0;

	int in_cs = (cr_counter & RSEQ_CR_IN_CS_MASK) != 0;
	u32 pid = curr->pid;
	u64 ts = now;  /* now is sched_clock(), same domain as last_preemption/last_idle_tp */
	int cpu = rq->cpu;

	struct wt_entry *ent = bpf_map_lookup_elem(&user_cs_map, &pid);

	if (in_cs && !ent) {
		u64 anchor = rq->last_preemption > rq->last_idle_tp
				     ? rq->last_preemption
				     : rq->last_idle_tp;
		struct wt_entry new_ent = { .entry_ts = ts, .wait_anchor = anchor };
		bpf_map_update_elem(&user_cs_map, &pid, &new_ent, BPF_ANY);
	} else if (!in_cs && ent) {
		u64 cs_duration = ts - ent->entry_ts;
		u64 wait_time = ent->wait_anchor < ent->entry_ts
					? ent->entry_ts - ent->wait_anchor
					: 0;
		emit_event(pid, curr->tgid, curr->comm, cpu, cs_duration, wait_time);
		bpf_map_delete_elem(&user_cs_map, &pid);
	}

	return 0;
}
