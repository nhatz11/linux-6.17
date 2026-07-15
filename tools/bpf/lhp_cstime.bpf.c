// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include "../../vmlinux.h"
#include "bpf_helpers.h"

extern const struct rq runqueues __ksym;
/* arch/x86/kernel/kvm.c:72 -- DEFINE_PER_CPU_DECRYPTED(...), __visible, not
 * static, so it's a real global symbol resolvable via BTF like runqueues
 * above. ->steal is the raw cumulative ns the host hypervisor writes into
 * this page directly (kvm_steal_clock() just reads it back under a seqcount
 * loop for tear protection, which we skip here -- fine for a diagnostic
 * delta, not fine for anything safety-critical). Continuously updated by the
 * host, NOT batched to the guest's scheduler tick -- unlike rq->preemptions
 * (cputime.c:277), which only updates once per tick and was confirmed too
 * coarse for hackbench's ~100-300ns critical sections (flat 0.24-0.25%
 * whether sysbench was on or off, when it should have moved a lot).
 */
extern struct kvm_steal_time steal_time __ksym;
#define HOST_STEAL_THRESHOLD_NS 100000 /* matches NHextend's floor, for comparability */

char LICENSE[] SEC("license") = "Dual BSD/GPL";

#define CR_COUNTER_OFFSET 28
#define RSEQ_CR_IN_CS_MASK 0xFFFFFFFEu
#define RSEQ_MIN_LEN 32

/* include/linux/sched.h:1830 -- "opted in to IVH pre-lock self-migration",
 * set by ivh_exec via prctl(PR_SET_IVH_ELIGIBLE). Filtering on this lets this
 * tool transparently scope to whatever ivh_exec wraps (hackbench included),
 * with zero changes to the target binary -- unlike NHextend's userspace CS,
 * which had to be instrumented by hand because it isn't a kernel spin_lock().
 */
#define PF_IVH_ELIGIBLE 0x00800000

struct cs_event {
	int cpu;
	int pid;
	int tgid;
	char comm[16];
	u64 duration_ns;
	u32 cs_type;
	u32 host_preempted; /* raw steal_time.steal advanced >100us during this
			      * hold -- real hypervisor steal, nanosecond-
			      * resolution, not tick-batched. */
};

struct cs_entry {
	u64 entry_ts;
	u32 cpu;
	u32 depth;
	u64 steal_at_entry;
};

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 256 * 1024);
} rb SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 10240);
	__type(key, u32);
	__type(value, struct cs_entry);
} user_cs_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 10240);
	__type(key, u32);
	__type(value, struct cs_entry);
} kern_cs_map SEC(".maps");

static void emit_event(u32 pid, u32 tgid, char *comm, int cpu,
		       u64 duration_ns, u32 cs_type, u32 host_preempted)
{
	struct cs_event *e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
	if (!e)
		return;
	e->cpu = cpu;
	e->pid = pid;
	e->tgid = tgid;
	__builtin_memcpy(e->comm, comm, 16);
	e->duration_ns = duration_ns;
	e->cs_type = cs_type;
	e->host_preempted = host_preempted;
	bpf_ringbuf_submit(e, 0);
}

SEC("sched/cfs_sched_tick_end")
int BPF_PROG(track_user_cs, struct rq *rq, u64 now, unsigned int idle_cpus)
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
	u64 ts = bpf_ktime_get_ns();
	int cpu = rq->cpu;

	struct cs_entry *ent = bpf_map_lookup_elem(&user_cs_map, &pid);

	if (in_cs && !ent) {
		struct cs_entry new_ent = { .entry_ts = ts, .cpu = cpu };
		bpf_map_update_elem(&user_cs_map, &pid, &new_ent, BPF_ANY);
	} else if (!in_cs && ent) {
		u64 duration = ts - ent->entry_ts;
		emit_event(pid, curr->tgid, curr->comm, cpu, duration, 0, 0);
		bpf_map_delete_elem(&user_cs_map, &pid);
	}

	return 0;
}

SEC("fexit/_raw_spin_lock")
int BPF_PROG(track_kern_cs_entry)
{
	struct task_struct *curr = (struct task_struct *)bpf_get_current_task_btf();

	if (!(curr->flags & PF_IVH_ELIGIBLE))
		return 0;

	u64 pidtgid = bpf_get_current_pid_tgid();
	u32 pid = (u32)pidtgid;

	/* Nested lock: bump depth so the unlock side knows not to emit yet. */
	struct cs_entry *existing = bpf_map_lookup_elem(&kern_cs_map, &pid);
	if (existing) {
		existing->depth++;
		return 0;
	}
	/* Outermost lock: record entry timestamp + this CPU's raw steal clock
	 * (steal_time.steal) for this TID. */
	u32 cpu = bpf_get_smp_processor_id();
	struct kvm_steal_time *st = bpf_per_cpu_ptr(&steal_time, cpu);
	u64 steal_at_entry = st ? st->steal : 0;
	u64 ts = bpf_ktime_get_ns();
	struct cs_entry ent = { .entry_ts = ts, .cpu = cpu, .depth = 1,
				.steal_at_entry = steal_at_entry };
	bpf_map_update_elem(&kern_cs_map, &pid, &ent, BPF_NOEXIST);
	return 0;
}

SEC("fentry/_raw_spin_unlock")
int BPF_PROG(track_kern_cs_exit)
{
	u64 pidtgid = bpf_get_current_pid_tgid();
	u32 pid = (u32)pidtgid;
	u32 tgid = (u32)(pidtgid >> 32);

	struct cs_entry *ent = bpf_map_lookup_elem(&kern_cs_map, &pid);
	if (!ent)
		return 0;

	/* Nested unlock: decrement depth and wait for the outermost unlock. */
	if (ent->depth > 1) {
		ent->depth--;
		return 0;
	}

	/* Outermost unlock: measure from the first lock acquisition to now.
	 * cpu can't have changed since entry -- _raw_spin_lock holds
	 * preempt_disable() for the whole hold, so this task is pinned to
	 * ent->cpu until this exact unlock (unlike NHextend's userspace lock,
	 * which has no such guarantee and has to handle mid-CS migration). */
	u64 duration = bpf_ktime_get_ns() - ent->entry_ts;
	u32 cpu = ent->cpu;
	u64 steal_at_entry = ent->steal_at_entry;
	bpf_map_delete_elem(&kern_cs_map, &pid);

	struct kvm_steal_time *st = bpf_per_cpu_ptr(&steal_time, cpu);
	u64 steal_now = st ? st->steal : steal_at_entry;
	u32 host_preempted = (steal_now - steal_at_entry) > HOST_STEAL_THRESHOLD_NS;

	char comm[16];
	bpf_get_current_comm(comm, sizeof(comm));
	emit_event(pid, tgid, comm, cpu, duration, 1, host_preempted);
	return 0;
}
