/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BPF_SCHED_H
#define _BPF_SCHED_H

#include <linux/bpf.h>
#include <linux/sched/clock.h>

#ifdef CONFIG_BPF_SYSCALL

#include <linux/jump_label.h>
#include <linux/percpu.h>

#define BPF_SCHED_HOOK(RET, DEFAULT, NAME, ...) \
	RET bpf_sched_##NAME(__VA_ARGS__);
#include <linux/sched_hook_defs.h>
#undef BPF_SCHED_HOOK

int bpf_sched_verify_prog(struct bpf_verifier_log *vlog,
			  const struct bpf_prog *prog);

DECLARE_STATIC_KEY_FALSE(bpf_sched_enabled_key);

static inline bool bpf_sched_enabled(void)
{
	return static_branch_unlikely(&bpf_sched_enabled_key);
}

static inline void bpf_sched_inc(void)
{
	static_branch_inc(&bpf_sched_enabled_key);
}

static inline void bpf_sched_dec(void)
{
	static_branch_dec(&bpf_sched_enabled_key);
}

/*
 * Diagnostic-only gate around cs_enter()/cs_exit()'s and
 * finish_task_switch()'s CS-timing sched_clock() reads (this session's own
 * addition, not upstream IVH). Defined in kernel/sched/bpf_sched.c.
 *   0 - off entirely: cs_start_ts never gets set, so cs_enter()/cs_exit()'s
 *       whole bodies (and the reopen in finish_task_switch()) are no-ops.
 *   1 - default, bit-for-bit original behavior: real sched_clock() TSC
 *       reads plus the full lock_depth/task_struct bookkeeping.
 *   2 - bookkeeping only: every call site still runs (lock_depth
 *       transitions, cs_start_ts/cs_wall_start_ts writes, last_cs_ns, ...)
 *       but the underlying clock read is a cheap per-CPU counter instead of
 *       sched_clock() -- see ivh_cs_clock(). Isolates "cost of the
 *       bookkeeping" from "cost of the TSC read" itself: (1) vs (0) is the
 *       whole tax, (2) vs (0) is the bookkeeping's share, (1) vs (2) is the
 *       TSC read's share. DIAGNOSTIC ONLY -- last_cs_ns becomes meaningless
 *       at this setting.
 *   3 - TSC-only isolation: pay the real sched_clock() cost and discard it,
 *       no cs_start_ts write, no bookkeeping at all. Pairs with mode 2 to
 *       separate the TSC read's share of the tax from the bookkeeping's
 *       share. Diagnostic only.
 *   echo 0|1|2|3 > /proc/sys/kernel/ivh_cs_track_enabled
 */
extern unsigned long ivh_cs_track_enabled;
DECLARE_PER_CPU(u64, ivh_cs_fake_clock_ctr);

/*
 * ivh_cs_clock() - sched_clock(), or (ivh_cs_track_enabled==2 only) a cheap
 * monotonic per-CPU counter standing in for it. See ivh_cs_track_enabled's
 * comment above for why this exists and its diagnostic-only caveat.
 */
static __always_inline u64 ivh_cs_clock(void)
{
	if (unlikely(READ_ONCE(ivh_cs_track_enabled) == 2))
		return __this_cpu_inc_return(ivh_cs_fake_clock_ctr);
	return sched_clock();
}

#else /* CONFIG_BPF_SYSCALL */

#define BPF_SCHED_HOOK(RET, DEFAULT, NAME, ...)	\
static inline RET bpf_sched_##NAME(__VA_ARGS__)	\
{						\
	return DEFAULT;				\
}
#include <linux/sched_hook_defs.h>
#undef BPF_SCHED_HOOK

static inline bool bpf_sched_enabled(void)
{
	return false;
}

#endif /* CONFIG_BPF_SYSCALL */

/*
 * Advisory (non-authoritative) Gate 1+2 re-check for task @t's current CPU,
 * called from kernel/rseq.c's rseq_update_cpu_node_id() to publish
 * RSEQ_SCHED_STATE_FLAG_IVH_DANGER (include/uapi/linux/rseq.h) on every
 * return-to-userspace. Defined in kernel/sched/fair.c.
 *
 * IVH rebuild Step 5 (tools/bpf/docs/ivh_rebuild_plan.md sec 4): production's
 * real implementation depends entirely on the capacity/migration engine
 * (ivh_universal_eligible, ivh_cap_source, ivh_gate_capacity(),
 * ivh_gate_time_left_reject()) -- Step 6/8 material not yet ported. Since
 * ivh_universal_eligible is always 0 until Step 8 turns it on, production's
 * own function would always return false anyway at this point in the
 * rebuild; kernel/sched/fair.c defines a minimal stub that does the same
 * thing directly, without pulling in Step 6's plumbing early. Same posture
 * as Step 3's ivh_cs_track_enabled and Step 4's out-of-scope exclusions.
 */
struct task_struct;
bool ivh_task_rq_in_danger(struct task_struct *t);

#endif /* _BPF_CGROUP_H */
