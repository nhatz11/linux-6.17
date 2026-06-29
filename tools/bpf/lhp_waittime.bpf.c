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
	u64 cs_corrected_ns;  /* cs_duration_ns minus time task was off-CPU */
	u64 wait_time_ns;
};

struct wt_entry {
	u64 entry_ts;
	u64 wait_anchor;
	u64 offcpu_start;  /* ktime when task last switched out (0 = on CPU) */
	u64 offcpu_total;  /* accumulated off-CPU ns since CS was first detected */
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
		       u64 cs_duration_ns, u64 cs_corrected_ns, u64 wait_time_ns)
{
	struct wt_event *e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e)
		return;
	e->cpu = cpu;
	e->pid = pid;
	e->tgid = tgid;
	__builtin_memcpy(e->comm, comm, 16);
	e->cs_duration_ns = cs_duration_ns;
	e->cs_corrected_ns = cs_corrected_ns;
	e->wait_time_ns = wait_time_ns;
	bpf_ringbuf_submit(e, 0);
}

/*
 * Track every context switch so we can subtract off-CPU time from the
 * tick-sampled CS window.  Fires for all tasks but the map lookup is
 * a fast miss for the vast majority that are not in a CS window.
 */
SEC("tp_btf/sched_switch")
int BPF_PROG(track_sched_switch, bool preempt,
	     struct task_struct *prev, struct task_struct *next)
{
	u64 now = bpf_ktime_get_ns();

	/* prev leaving CPU: start its off-CPU period */
	u32 prev_pid = prev->pid;
	struct wt_entry *pent = bpf_map_lookup_elem(&user_cs_map, &prev_pid);
	if (pent && pent->offcpu_start == 0)
		pent->offcpu_start = now;

	/* next arriving on CPU: close its off-CPU period */
	u32 next_pid = next->pid;
	struct wt_entry *nent = bpf_map_lookup_elem(&user_cs_map, &next_pid);
	if (nent && nent->offcpu_start != 0) {
		nent->offcpu_total += now - nent->offcpu_start;
		nent->offcpu_start = 0;
	}

	return 0;
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
	/*
	 * Use bpf_ktime_get_ns() rather than the 'now' arg so timestamps
	 * are in the same domain as the sched_switch hook above.
	 */
	u64 ts = bpf_ktime_get_ns();
	int cpu = rq->cpu;

	struct wt_entry *ent = bpf_map_lookup_elem(&user_cs_map, &pid);

	if (in_cs && !ent) {
		u64 anchor = rq->last_preemption > rq->last_idle_tp
				     ? rq->last_preemption
				     : rq->last_idle_tp;
		struct wt_entry new_ent = {
			.entry_ts    = ts,
			.wait_anchor = anchor,
			.offcpu_start = 0,
			.offcpu_total = 0,
		};
		bpf_map_update_elem(&user_cs_map, &pid, &new_ent, BPF_ANY);
	} else if (!in_cs && ent) {
		u64 offcpu = ent->offcpu_total;
		/*
		 * If offcpu_start is still set the task switched out and back
		 * between the sched_switch hook and this tick without the
		 * switch-in path clearing it — shouldn't happen but handle it.
		 */
		if (ent->offcpu_start != 0)
			offcpu += ts - ent->offcpu_start;

		u64 cs_raw       = ts - ent->entry_ts;
		u64 cs_corrected = cs_raw > offcpu ? cs_raw - offcpu : 0;
		u64 wait_time    = ent->wait_anchor < ent->entry_ts
					? ent->entry_ts - ent->wait_anchor : 0;

		emit_event(pid, curr->tgid, curr->comm, cpu,
			   cs_raw, cs_corrected, wait_time);
		bpf_map_delete_elem(&user_cs_map, &pid);
	}

	return 0;
}
