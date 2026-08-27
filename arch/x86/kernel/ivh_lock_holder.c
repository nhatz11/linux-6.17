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
	struct ivh_holder_slot *slot = ivh_holder_slot_of(lock);

	if (unlikely(!slot))
		return;

	WRITE_ONCE(slot->holder_cpu, (u32)raw_smp_processor_id() + 1);
	WRITE_ONCE(slot->tag, lock);
	this_cpu_inc(ivh_holder_stamps);
}
EXPORT_SYMBOL_GPL(__ivh_lock_set_holder);

void __ivh_lock_clear_holder(struct qspinlock *lock)
{
	struct ivh_holder_slot *slot = ivh_holder_slot_of(lock);

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
