// SPDX-License-Identifier: GPL-2.0
/*
 * IVH lock-holder identity -- storage + the 3 out-of-line functions declared
 * in <linux/ivh_lock_holder.h>.
 *
 * Ported from production kernel/x86/kernel/kvm.c (verbatim body-for-body,
 * see that file's block starting "IVH lock-holder identity -- Option B") into
 * its own file for Step 2 of the incremental rebuild
 * (tools/bpf/docs/ivh_rebuild_plan.md sec 4): production fuses this table's
 * allocation into ivh_pv_beat_calibrate(), a single late_initcall that ALSO
 * calibrates the PV heartbeat / CS-stamp / Part-C jump thresholds -- all
 * Step 3/4 material with no dependency on this table. Splitting the
 * allocation out here keeps Step 2 to exactly what it claims to test ("does
 * the compiled-in-but-off holder-identity table cost anything"), with zero
 * of the calibration logic pulled in ahead of the steps that actually need
 * it. The two counters ivh_holder_raced/ivh_holder_self (also DEFINE_PER_CPU
 * next to these four in production) are unused by the 3 functions below --
 * they belong to a later step -- and are deliberately not ported here.
 */
#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/export.h>
#include <linux/vmalloc.h>
#include <linux/hash.h>
#include <linux/init.h>
#include <linux/ivh_lock_holder.h>
#include <linux/log2.h>
#include <linux/irqflags.h>	/* irqs_disabled(), G-LOCK-41 */
/*
 * <asm/ivh_tsc_beat.h> is not self-contained: its ivh_tsc_cycles_to_ns()/
 * ivh_tsc_ns_to_cycles() helpers use USEC_PER_SEC without including it (every
 * existing includer gets it transitively). This file has no such transitive
 * path, so pull it in explicitly rather than editing that header's includes.
 */
#include <linux/time64.h>
#include <asm/ivh_tsc_beat.h>

DEFINE_PER_CPU(u64, ivh_holder_stamps);
DEFINE_PER_CPU(u64, ivh_holder_clears);
DEFINE_PER_CPU(u64, ivh_holder_unknown_empty);
DEFINE_PER_CPU(u64, ivh_holder_unknown_collision);

struct ivh_holder_slot {
	void *tag;		/* the qspinlock pointer this slot describes */
	u32   holder_cpu;	/* CPU + 1; 0 == empty */
} ____cacheline_aligned_in_smp;

static struct ivh_holder_slot *ivh_holder_table __read_mostly;
static unsigned long ivh_holder_table_slots;	/* == 1 << IVH_HOLDER_MAX_BITS */

unsigned long ivh_lock_holder_enabled = 0UL;	/* 0 = no stamping at all */
EXPORT_SYMBOL_GPL(ivh_lock_holder_enabled);
unsigned long ivh_holder_bits = IVH_HOLDER_MAX_BITS;
EXPORT_SYMBOL_GPL(ivh_holder_bits);

static __always_inline struct ivh_holder_slot *ivh_holder_slot_of(struct qspinlock *lock)
{
	struct ivh_holder_slot *table = READ_ONCE(ivh_holder_table);

	if (unlikely(!table))
		return NULL;

	return &table[hash_ptr(lock, READ_ONCE(ivh_holder_bits))];
}

void __ivh_lock_set_holder(struct qspinlock *lock)
{
	unsigned long en = READ_ONCE(ivh_lock_holder_enabled);
	struct ivh_holder_slot *slot;

	/* G-LOCK-30: the fast-path owner stamp rides this gate; see the bitmask
	 * comment in <linux/ivh_lock_holder.h>. */
	if (en & IVH_HOLDER_EN_CS_FAST)
		__ivh_cs_owner_stamp(lock);
	if (!(en & IVH_HOLDER_EN_TABLE))
		return;
	slot = ivh_holder_slot_of(lock);

	if (unlikely(!slot))
		return;

	WRITE_ONCE(slot->holder_cpu, (u32)raw_smp_processor_id() + 1);
	WRITE_ONCE(slot->tag, lock);
	this_cpu_inc(ivh_holder_stamps);
}
EXPORT_SYMBOL_GPL(__ivh_lock_set_holder);

void __ivh_lock_clear_holder(struct qspinlock *lock)
{
	struct ivh_holder_slot *slot;

	/* The owner-stamp clear is ivh_cs_owner_release(), on its own gate. */
	if (!(READ_ONCE(ivh_lock_holder_enabled) & IVH_HOLDER_EN_TABLE))
		return;
	slot = ivh_holder_slot_of(lock);

	if (unlikely(!slot))
		return;

	if (READ_ONCE(slot->tag) != (void *)lock)
		return;			/* another lock owns this slot */

	WRITE_ONCE(slot->tag, NULL);
	WRITE_ONCE(slot->holder_cpu, 0);
	this_cpu_inc(ivh_holder_clears);
}
EXPORT_SYMBOL_GPL(__ivh_lock_clear_holder);

int ivh_lock_holder_cpu(struct qspinlock *lock)
{
	struct ivh_holder_slot *slot = ivh_holder_slot_of(lock);
	u32 holder;

	if (unlikely(!slot))
		return -1;

	if (READ_ONCE(slot->tag) != (void *)lock) {
		if (READ_ONCE(slot->holder_cpu))
			this_cpu_inc(ivh_holder_unknown_collision);
		else
			this_cpu_inc(ivh_holder_unknown_empty);
		return -1;
	}

	holder = READ_ONCE(slot->holder_cpu);
	if (!holder) {
		this_cpu_inc(ivh_holder_unknown_empty);
		return -1;
	}

	return (int)holder - 1;
}
EXPORT_SYMBOL_GPL(ivh_lock_holder_cpu);

/*
 * Allocate the holder side table at its MAXIMUM geometry, once, here.
 * A failure is not fatal: without a table ivh_holder_slot_of() returns NULL,
 * every lookup answers "unknown", and the rest of the kernel is entirely
 * unaffected (there is no sysctl to arm ivh_lock_holder_enabled yet in this
 * step, so this table is inert regardless).
 */
static int __init ivh_lock_holder_table_init(void)
{
	ivh_holder_table_slots = 1UL << IVH_HOLDER_MAX_BITS;
	ivh_holder_table = vzalloc(ivh_holder_table_slots *
				   sizeof(struct ivh_holder_slot));
	if (!ivh_holder_table) {
		ivh_holder_table_slots = 0;
		pr_err("IVH: lock-holder side table allocation failed (%lu slots x %zu B)\n",
		       1UL << IVH_HOLDER_MAX_BITS,
		       sizeof(struct ivh_holder_slot));
	} else {
		pr_info("IVH: lock-holder side table = %lu slots x %zu B (%lu KB), effective index width %lu bits\n",
			ivh_holder_table_slots, sizeof(struct ivh_holder_slot),
			(ivh_holder_table_slots * sizeof(struct ivh_holder_slot)) >> 10,
			ivh_holder_bits);
	}

	return 0;
}
late_initcall(ivh_lock_holder_table_init);

/*
 * is_cs_preempted()'s owner stamp. See <linux/ivh_lock_holder.h> for why the
 * gate is inlined and the body is not, and the 2026-09-14 build plan sec 1 for
 * why one site suffices.
 *
 * ->tsc must become visible before ->lock: ->lock is the validity flag the
 * remote reader tests, and without this ordering a reader can pair a matching
 * ->lock with a stale ->tsc from a previous hold and compute a wildly wrong --
 * or negative -- held_for. Identical rule and identical reason to
 * ivh_rot_stamp_release() (kernel/locking/qspinlock_paravirt.h:1375-1383). On
 * x86-TSO smp_wmb() is a compiler barrier, so this is free; it is written
 * because the rule is real, not because the instruction is.
 */
void __ivh_cs_owner_stamp(struct qspinlock *lock)
{
	if (unlikely(this_cpu_read(ivh_cs_owner.lock)))
		this_cpu_inc(ivh_cs_stamp_overwrote);

	this_cpu_write(ivh_cs_owner.tsc, rdtsc());
	smp_wmb();
	this_cpu_write(ivh_cs_owner.lock, lock);
	this_cpu_inc(ivh_cs_stamps);
}
EXPORT_SYMBOL_GPL(__ivh_cs_owner_stamp);

/*
 * Tag-checked and therefore idempotent: a release of a lock this CPU never
 * stamped, or of an outer lock whose slot an inner one has since overwritten,
 * finds a mismatch and does nothing. That is what makes partial stamp coverage
 * safe -- the stamps/clears accounting identity is
 *   ivh_cs_stamps == ivh_cs_clears + ivh_cs_stamp_overwrote + (in flight)
 * rather than a raw equality, which is the lesson of the 680000:1
 * stamps:clears ratio recorded at <asm/qspinlock.h>:100-140.
 */
void __ivh_cs_owner_clear(struct qspinlock *lock)
{
	s64 held;

	u64 tsc;

	if (this_cpu_read(ivh_cs_owner.lock) != (void *)lock)
		return;
	/*
	 * G-LOCK-31: load the acquisition TSC BEFORE the NULL store, and re-check
	 * the tag after loading it. An IRQ that stamps another lock between the
	 * tag check and this read replaces tsc; the re-check catches that and
	 * skips the last_cs sample rather than recording a corrupt hold.
	 */
	tsc = this_cpu_read(ivh_cs_owner.tsc);
	if (this_cpu_read(ivh_cs_owner.lock) != (void *)lock)
		return;
	this_cpu_write(ivh_cs_owner.lock, NULL);
	this_cpu_inc(ivh_cs_clears);

	/*
	 * Holder-side hold-duration sample. Under clear==1 the observer-side
	 * sample at gotlock: can never fire (this store has already NULLed
	 * the tag it tests), so the population-correct contended-hold
	 * histogram is taken here instead. Only stamped holds reach this
	 * line, and only contended acquisitions are stamped, so this is the
	 * same population either way.
	 */
	held = (s64)(rdtsc() - tsc);
	if (held > 0) {
		this_cpu_inc(ivh_cs_prev_hold_hist[held >= (1LL << 31) ?
				IVH_BEAT_AGE_HIST_BUCKETS - 1 : ilog2((u64)held)]);
		/* G-LOCK-31: written after the NULL store, so a reader that sees
		 * the tag still naming a lock never pairs it with this value. */
		this_cpu_write(ivh_cs_owner.last_cs, (u64)held);
	}

	/*
	 * G-LOCK-41 -- the LH detector's 2x2, scored HERE because this is the
	 * one site every stamped hold passes through, so both the flagged and
	 * the unflagged row exist. (At the fire site only the flagged row does,
	 * which is why a hook there could never give more than precision.)
	 *
	 *   (a) the queue head's remote inference, from a heartbeat stamp;
	 *   (b) this vCPU's OWN tick-driven raw-TSC evidence.
	 * Different mechanisms, so agreement is information. (b) was validated
	 * per-event against host `perf sched` -- evaluation.md section 12.
	 *
	 * Placed after the last_cs write so nothing above is perturbed, and
	 * after the NULL store so no remote reader can pair a live tag with it.
	 *
	 * The irqoff split is NOT optional: ivh_vact_tick() runs only from
	 * account_process_tick(), so with interrupts disabled the tick that
	 * would have recorded the gap has not run yet and (b) reads 0 for a
	 * hold that WAS preempted. [1][0] is a blind spot, not a true negative.
	 */
	if (unlikely(READ_ONCE(ivh_cs_verdict))) {
		int irqoff = !!irqs_disabled();
		u64 rel = rdtsc();
		u64 dep = this_cpu_read(ivh_cs_flagged_acq);
		int v;

		/*
		 * A hold shorter than the detection lag cannot be judged: a
		 * preemption starting inside it is not detected until after it
		 * closed. Scoring those "not preempted" buried 17.8M
		 * unjudgeable holds in the denominator on the first run.
		 */
		if (!ivh_vact_judgeable(rel - tsc))
			v = IVH_CS_V_UNKNOWABLE;
		else
			v = ivh_vact_preempt_since(tsc);

		/*
		 * WINDOW match, not equality. 47% of stamps overwrite a
		 * previous one (ivh_cs_stamp_overwrote) because nested
		 * acquires re-stamp ivh_cs_owner.tsc, so the head's deposit
		 * may name an inner hold rather than this one. Exact equality
		 * matched 63 of 11796 fires. Any deposit landing inside
		 * [tsc, rel] was made while this CPU held this lock, which is
		 * what the audit is asking.
		 */
		if (dep && (s64)(dep - tsc) >= 0 && (s64)(rel - dep) >= 0) {
			this_cpu_inc(ivh_cs_v_flagged[irqoff][v]);
			this_cpu_write(ivh_cs_flagged_acq, 0);
		} else {
			this_cpu_inc(ivh_cs_v_unflagged[irqoff][v]);
		}
	}
}
EXPORT_SYMBOL_GPL(__ivh_cs_owner_clear);
