// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * KVM paravirt_ops implementation
 *
 * Copyright (C) 2007, Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 * Copyright IBM Corporation, 2007
 *   Authors: Anthony Liguori <aliguori@us.ibm.com>
 */

#define pr_fmt(fmt) "kvm-guest: " fmt

#include <linux/context_tracking.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/kvm_para.h>
#include <linux/cpu.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/hardirq.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/hash.h>
#include <linux/sched.h>
#include <linux/bpf_sched.h>
#include <linux/slab.h>
#include <linux/kprobes.h>
#include <linux/nmi.h>
#include <linux/smp.h>
#include <trace/events/ipi.h>
#include <linux/swait.h>
#include <linux/syscore_ops.h>
#include <linux/cc_platform.h>
#include <linux/efi.h>
#include <linux/vmalloc.h>
#include <asm/timer.h>
#include <asm/cpu.h>
#include <asm/traps.h>
#include <asm/desc.h>
#include <asm/tlbflush.h>
#include <asm/apic.h>
#include <asm/apicdef.h>
#include <asm/hypervisor.h>
#include <asm/mtrr.h>
#include <asm/tlb.h>
#include <asm/cpuidle_haltpoll.h>
#include <asm/msr.h>
#include <asm/mwait.h>
#include <linux/sysctl.h>
#include <asm/ptrace.h>
#include <asm/reboot.h>
#include <asm/svm.h>
#include <asm/e820/api.h>

DEFINE_STATIC_KEY_FALSE_RO(kvm_async_pf_enabled);

static int kvmapf = 1;

static int __init parse_no_kvmapf(char *arg)
{
        kvmapf = 0;
        return 0;
}

early_param("no-kvmapf", parse_no_kvmapf);

static int steal_acc = 1;
static int __init parse_no_stealacc(char *arg)
{
        steal_acc = 0;
        return 0;
}

early_param("no-steal-acc", parse_no_stealacc);

static DEFINE_PER_CPU_READ_MOSTLY(bool, async_pf_enabled);
static DEFINE_PER_CPU_DECRYPTED(struct kvm_vcpu_pv_apf_data, apf_reason) __aligned(64);
DEFINE_PER_CPU_DECRYPTED(struct kvm_steal_time, steal_time) __aligned(64) __visible;
static int has_steal_clock = 0;

static int has_guest_poll = 0;
/*
 * No need for any "IO delay" on KVM
 */
static void kvm_io_delay(void)
{
}

#define KVM_TASK_SLEEP_HASHBITS 8
#define KVM_TASK_SLEEP_HASHSIZE (1<<KVM_TASK_SLEEP_HASHBITS)

struct kvm_task_sleep_node {
	struct hlist_node link;
	struct swait_queue_head wq;
	u32 token;
	int cpu;
};

static struct kvm_task_sleep_head {
	raw_spinlock_t lock;
	struct hlist_head list;
} async_pf_sleepers[KVM_TASK_SLEEP_HASHSIZE];

static struct kvm_task_sleep_node *_find_apf_task(struct kvm_task_sleep_head *b,
						  u32 token)
{
	struct hlist_node *p;

	hlist_for_each(p, &b->list) {
		struct kvm_task_sleep_node *n =
			hlist_entry(p, typeof(*n), link);
		if (n->token == token)
			return n;
	}

	return NULL;
}

static bool kvm_async_pf_queue_task(u32 token, struct kvm_task_sleep_node *n)
{
	u32 key = hash_32(token, KVM_TASK_SLEEP_HASHBITS);
	struct kvm_task_sleep_head *b = &async_pf_sleepers[key];
	struct kvm_task_sleep_node *e;

	raw_spin_lock(&b->lock);
	e = _find_apf_task(b, token);
	if (e) {
		/* dummy entry exist -> wake up was delivered ahead of PF */
		hlist_del(&e->link);
		raw_spin_unlock(&b->lock);
		kfree(e);
		return false;
	}

	n->token = token;
	n->cpu = smp_processor_id();
	init_swait_queue_head(&n->wq);
	hlist_add_head(&n->link, &b->list);
	raw_spin_unlock(&b->lock);
	return true;
}

/*
 * kvm_async_pf_task_wait_schedule - Wait for pagefault to be handled
 * @token:	Token to identify the sleep node entry
 *
 * Invoked from the async pagefault handling code or from the VM exit page
 * fault handler. In both cases RCU is watching.
 */
void kvm_async_pf_task_wait_schedule(u32 token)
{
	struct kvm_task_sleep_node n;
	DECLARE_SWAITQUEUE(wait);

	lockdep_assert_irqs_disabled();

	if (!kvm_async_pf_queue_task(token, &n))
		return;

	for (;;) {
		prepare_to_swait_exclusive(&n.wq, &wait, TASK_UNINTERRUPTIBLE);
		if (hlist_unhashed(&n.link))
			break;

		local_irq_enable();
		schedule();
		local_irq_disable();
	}
	finish_swait(&n.wq, &wait);
}
EXPORT_SYMBOL_GPL(kvm_async_pf_task_wait_schedule);

static void apf_task_wake_one(struct kvm_task_sleep_node *n)
{
	hlist_del_init(&n->link);
	if (swq_has_sleeper(&n->wq))
		swake_up_one(&n->wq);
}

static void apf_task_wake_all(void)
{
	int i;

	for (i = 0; i < KVM_TASK_SLEEP_HASHSIZE; i++) {
		struct kvm_task_sleep_head *b = &async_pf_sleepers[i];
		struct kvm_task_sleep_node *n;
		struct hlist_node *p, *next;

		raw_spin_lock(&b->lock);
		hlist_for_each_safe(p, next, &b->list) {
			n = hlist_entry(p, typeof(*n), link);
			if (n->cpu == smp_processor_id())
				apf_task_wake_one(n);
		}
		raw_spin_unlock(&b->lock);
	}
}

void kvm_async_pf_task_wake(u32 token)
{
	u32 key = hash_32(token, KVM_TASK_SLEEP_HASHBITS);
	struct kvm_task_sleep_head *b = &async_pf_sleepers[key];
	struct kvm_task_sleep_node *n, *dummy = NULL;

	if (token == ~0) {
		apf_task_wake_all();
		return;
	}

again:
	raw_spin_lock(&b->lock);
	n = _find_apf_task(b, token);
	if (!n) {
		/*
		 * Async #PF not yet handled, add a dummy entry for the token.
		 * Allocating the token must be down outside of the raw lock
		 * as the allocator is preemptible on PREEMPT_RT kernels.
		 */
		if (!dummy) {
			raw_spin_unlock(&b->lock);
			dummy = kzalloc(sizeof(*dummy), GFP_ATOMIC);

			/*
			 * Continue looping on allocation failure, eventually
			 * the async #PF will be handled and allocating a new
			 * node will be unnecessary.
			 */
			if (!dummy)
				cpu_relax();

			/*
			 * Recheck for async #PF completion before enqueueing
			 * the dummy token to avoid duplicate list entries.
			 */
			goto again;
		}
		dummy->token = token;
		dummy->cpu = smp_processor_id();
		init_swait_queue_head(&dummy->wq);
		hlist_add_head(&dummy->link, &b->list);
		dummy = NULL;
	} else {
		apf_task_wake_one(n);
	}
	raw_spin_unlock(&b->lock);

	/* A dummy token might be allocated and ultimately not used.  */
	kfree(dummy);
}
EXPORT_SYMBOL_GPL(kvm_async_pf_task_wake);

noinstr u32 kvm_read_and_reset_apf_flags(void)
{
	u32 flags = 0;

	if (__this_cpu_read(async_pf_enabled)) {
		flags = __this_cpu_read(apf_reason.flags);
		__this_cpu_write(apf_reason.flags, 0);
	}

	return flags;
}
EXPORT_SYMBOL_GPL(kvm_read_and_reset_apf_flags);

noinstr bool __kvm_handle_async_pf(struct pt_regs *regs, u32 token)
{
	u32 flags = kvm_read_and_reset_apf_flags();
	irqentry_state_t state;

	if (!flags)
		return false;

	state = irqentry_enter(regs);
	instrumentation_begin();

	/*
	 * If the host managed to inject an async #PF into an interrupt
	 * disabled region, then die hard as this is not going to end well
	 * and the host side is seriously broken.
	 */
	if (unlikely(!(regs->flags & X86_EFLAGS_IF)))
		panic("Host injected async #PF in interrupt disabled region\n");

	if (flags & KVM_PV_REASON_PAGE_NOT_PRESENT) {
		if (unlikely(!(user_mode(regs))))
			panic("Host injected async #PF in kernel mode\n");
		/* Page is swapped out by the host. */
		kvm_async_pf_task_wait_schedule(token);
	} else {
		WARN_ONCE(1, "Unexpected async PF flags: %x\n", flags);
	}

	instrumentation_end();
	irqentry_exit(regs, state);
	return true;
}

DEFINE_IDTENTRY_SYSVEC(sysvec_kvm_asyncpf_interrupt)
{
	struct pt_regs *old_regs = set_irq_regs(regs);
	u32 token;

	apic_eoi();

	inc_irq_stat(irq_hv_callback_count);

	if (__this_cpu_read(async_pf_enabled)) {
		token = __this_cpu_read(apf_reason.token);
		kvm_async_pf_task_wake(token);
		__this_cpu_write(apf_reason.token, 0);
		wrmsrq(MSR_KVM_ASYNC_PF_ACK, 1);
	}

	set_irq_regs(old_regs);
}

static void __init paravirt_ops_setup(void)
{
	pv_info.name = "KVM";

	if (kvm_para_has_feature(KVM_FEATURE_NOP_IO_DELAY))
		pv_ops.cpu.io_delay = kvm_io_delay;

#ifdef CONFIG_X86_IO_APIC
	no_timer_check = 1;
#endif
}

static void kvm_register_steal_time(void)
{
	int cpu = smp_processor_id();
	struct kvm_steal_time *st = &per_cpu(steal_time, cpu);

	if (!has_steal_clock)
		return;

	wrmsrq(MSR_KVM_STEAL_TIME, (slow_virt_to_phys(st) | KVM_MSR_ENABLED));
	pr_debug("stealtime: cpu %d, msr %llx\n", cpu,
		(unsigned long long) slow_virt_to_phys(st));
}

static DEFINE_PER_CPU_DECRYPTED(unsigned long, kvm_apic_eoi) = KVM_PV_EOI_DISABLED;

static notrace __maybe_unused void kvm_guest_apic_eoi_write(void)
{
	/**
	 * This relies on __test_and_clear_bit to modify the memory
	 * in a way that is atomic with respect to the local CPU.
	 * The hypervisor only accesses this memory from the local CPU so
	 * there's no need for lock or memory barriers.
	 * An optimization barrier is implied in apic write.
	 */
	if (__test_and_clear_bit(KVM_PV_EOI_BIT, this_cpu_ptr(&kvm_apic_eoi)))
		return;
	apic_native_eoi();
}

static void kvm_guest_cpu_init(void)
{
	if (kvm_para_has_feature(KVM_FEATURE_ASYNC_PF_INT) && kvmapf) {
		u64 pa;

		WARN_ON_ONCE(!static_branch_likely(&kvm_async_pf_enabled));

		pa = slow_virt_to_phys(this_cpu_ptr(&apf_reason));
		pa |= KVM_ASYNC_PF_ENABLED | KVM_ASYNC_PF_DELIVERY_AS_INT;

		if (kvm_para_has_feature(KVM_FEATURE_ASYNC_PF_VMEXIT))
			pa |= KVM_ASYNC_PF_DELIVERY_AS_PF_VMEXIT;

		wrmsrq(MSR_KVM_ASYNC_PF_INT, HYPERVISOR_CALLBACK_VECTOR);

		wrmsrq(MSR_KVM_ASYNC_PF_EN, pa);
		__this_cpu_write(async_pf_enabled, true);
		pr_debug("setup async PF for cpu %d\n", smp_processor_id());
	}

	if (kvm_para_has_feature(KVM_FEATURE_PV_EOI)) {
		unsigned long pa;

		/* Size alignment is implied but just to make it explicit. */
		BUILD_BUG_ON(__alignof__(kvm_apic_eoi) < 4);
		__this_cpu_write(kvm_apic_eoi, 0);
		pa = slow_virt_to_phys(this_cpu_ptr(&kvm_apic_eoi))
			| KVM_MSR_ENABLED;
		wrmsrq(MSR_KVM_PV_EOI_EN, pa);
	}

	if (has_steal_clock)
		kvm_register_steal_time();
}

static void kvm_pv_disable_apf(void)
{
	if (!__this_cpu_read(async_pf_enabled))
		return;

	wrmsrq(MSR_KVM_ASYNC_PF_EN, 0);
	__this_cpu_write(async_pf_enabled, false);

	pr_debug("disable async PF for cpu %d\n", smp_processor_id());
}

static void kvm_disable_steal_time(void)
{
	if (!has_steal_clock)
		return;

	wrmsrq(MSR_KVM_STEAL_TIME, 0);
}

static u64 kvm_steal_clock(int cpu)
{
	u64 steal;
	struct kvm_steal_time *src;
	int version;

	src = &per_cpu(steal_time, cpu);
	do {
		version = src->version;
		virt_rmb();
		steal = src->steal;
		virt_rmb();
	} while ((version & 1) || (version != src->version));

	return steal;
}

/*
 * Hot Threads: cheap cumulative steal-ns read for THIS vCPU. Callers
 * (kernel/locking/spinlock.c cs_enter()/cs_exit()) hold a raw spinlock, so
 * preemption is off and this_cpu is stable across the read. Reuses
 * kvm_steal_clock()'s seqlock read rather than duplicating it.
 */
u64 ivh_this_cpu_steal_ns(void)
{
	if (!has_steal_clock)
		return 0;
	return kvm_steal_clock(smp_processor_id());
}
EXPORT_SYMBOL_GPL(ivh_this_cpu_steal_ns);

static inline __init void __set_percpu_decrypted(void *ptr, unsigned long size)
{
	early_set_memory_decrypted((unsigned long) ptr, size);
}

/*
 * Iterate through all possible CPUs and map the memory region pointed
 * by apf_reason, steal_time and kvm_apic_eoi as decrypted at once.
 *
 * Note: we iterate through all possible CPUs to ensure that CPUs
 * hotplugged will have their per-cpu variable already mapped as
 * decrypted.
 */
static void __init sev_map_percpu_data(void)
{
	int cpu;

	if (cc_vendor != CC_VENDOR_AMD ||
	    !cc_platform_has(CC_ATTR_GUEST_MEM_ENCRYPT))
		return;

	for_each_possible_cpu(cpu) {
		__set_percpu_decrypted(&per_cpu(apf_reason, cpu), sizeof(apf_reason));
		__set_percpu_decrypted(&per_cpu(steal_time, cpu), sizeof(steal_time));
		__set_percpu_decrypted(&per_cpu(kvm_apic_eoi, cpu), sizeof(kvm_apic_eoi));
	}
}

static void kvm_guest_cpu_offline(bool shutdown)
{
	kvm_disable_steal_time();
	if (kvm_para_has_feature(KVM_FEATURE_PV_EOI))
		wrmsrq(MSR_KVM_PV_EOI_EN, 0);
	if (kvm_para_has_feature(KVM_FEATURE_MIGRATION_CONTROL))
		wrmsrq(MSR_KVM_MIGRATION_CONTROL, 0);
	kvm_pv_disable_apf();
	if (!shutdown)
		apf_task_wake_all();
	kvmclock_disable();
}

static int kvm_cpu_online(unsigned int cpu)
{
	unsigned long flags;

	local_irq_save(flags);
	kvm_guest_cpu_init();
	local_irq_restore(flags);
	return 0;
}

#ifdef CONFIG_SMP

static DEFINE_PER_CPU(cpumask_var_t, __pv_cpu_mask);

static bool pv_tlb_flush_supported(void)
{
	return (kvm_para_has_feature(KVM_FEATURE_PV_TLB_FLUSH) &&
		!kvm_para_has_hint(KVM_HINTS_REALTIME) &&
		kvm_para_has_feature(KVM_FEATURE_STEAL_TIME) &&
		!boot_cpu_has(X86_FEATURE_MWAIT) &&
		(num_possible_cpus() != 1));
}

static bool pv_ipi_supported(void)
{
	return (kvm_para_has_feature(KVM_FEATURE_PV_SEND_IPI) &&
	       (num_possible_cpus() != 1));
}

static bool pv_sched_yield_supported(void)
{
	return (kvm_para_has_feature(KVM_FEATURE_PV_SCHED_YIELD) &&
		!kvm_para_has_hint(KVM_HINTS_REALTIME) &&
	    kvm_para_has_feature(KVM_FEATURE_STEAL_TIME) &&
	    !boot_cpu_has(X86_FEATURE_MWAIT) &&
	    (num_possible_cpus() != 1));
}

#define KVM_IPI_CLUSTER_SIZE	(2 * BITS_PER_LONG)

static void __send_ipi_mask(const struct cpumask *mask, int vector)
{
	unsigned long flags;
	int cpu, min = 0, max = 0;
#ifdef CONFIG_X86_64
	__uint128_t ipi_bitmap = 0;
#else
	u64 ipi_bitmap = 0;
#endif
	u32 apic_id, icr;
	long ret;

	if (cpumask_empty(mask))
		return;

	local_irq_save(flags);

	switch (vector) {
	default:
		icr = APIC_DM_FIXED | vector;
		break;
	case NMI_VECTOR:
		icr = APIC_DM_NMI;
		break;
	}

	for_each_cpu(cpu, mask) {
		apic_id = per_cpu(x86_cpu_to_apicid, cpu);
		if (!ipi_bitmap) {
			min = max = apic_id;
		} else if (apic_id < min && max - apic_id < KVM_IPI_CLUSTER_SIZE) {
			ipi_bitmap <<= min - apic_id;
			min = apic_id;
		} else if (apic_id > min && apic_id < min + KVM_IPI_CLUSTER_SIZE) {
			max = apic_id < max ? max : apic_id;
		} else {
			ret = kvm_hypercall4(KVM_HC_SEND_IPI, (unsigned long)ipi_bitmap,
				(unsigned long)(ipi_bitmap >> BITS_PER_LONG), min, icr);
			WARN_ONCE(ret < 0, "kvm-guest: failed to send PV IPI: %ld",
				  ret);
			min = max = apic_id;
			ipi_bitmap = 0;
		}
		__set_bit(apic_id - min, (unsigned long *)&ipi_bitmap);
	}

	if (ipi_bitmap) {
		ret = kvm_hypercall4(KVM_HC_SEND_IPI, (unsigned long)ipi_bitmap,
			(unsigned long)(ipi_bitmap >> BITS_PER_LONG), min, icr);
		WARN_ONCE(ret < 0, "kvm-guest: failed to send PV IPI: %ld",
			  ret);
	}

	local_irq_restore(flags);
}

static void kvm_send_ipi_mask(const struct cpumask *mask, int vector)
{
	__send_ipi_mask(mask, vector);
}

static void kvm_send_ipi_mask_allbutself(const struct cpumask *mask, int vector)
{
	unsigned int this_cpu = smp_processor_id();
	struct cpumask *new_mask = this_cpu_cpumask_var_ptr(__pv_cpu_mask);
	const struct cpumask *local_mask;

	cpumask_copy(new_mask, mask);
	cpumask_clear_cpu(this_cpu, new_mask);
	local_mask = new_mask;
	__send_ipi_mask(local_mask, vector);
}

static int __init setup_efi_kvm_sev_migration(void)
{
	efi_char16_t efi_sev_live_migration_enabled[] = L"SevLiveMigrationEnabled";
	efi_guid_t efi_variable_guid = AMD_SEV_MEM_ENCRYPT_GUID;
	efi_status_t status;
	unsigned long size;
	bool enabled;

	if (!cc_platform_has(CC_ATTR_GUEST_MEM_ENCRYPT) ||
	    !kvm_para_has_feature(KVM_FEATURE_MIGRATION_CONTROL))
		return 0;

	if (!efi_enabled(EFI_BOOT))
		return 0;

	if (!efi_enabled(EFI_RUNTIME_SERVICES)) {
		pr_info("%s : EFI runtime services are not enabled\n", __func__);
		return 0;
	}

	size = sizeof(enabled);

	/* Get variable contents into buffer */
	status = efi.get_variable(efi_sev_live_migration_enabled,
				  &efi_variable_guid, NULL, &size, &enabled);

	if (status == EFI_NOT_FOUND) {
		pr_info("%s : EFI live migration variable not found\n", __func__);
		return 0;
	}

	if (status != EFI_SUCCESS) {
		pr_info("%s : EFI variable retrieval failed\n", __func__);
		return 0;
	}

	if (enabled == 0) {
		pr_info("%s: live migration disabled in EFI\n", __func__);
		return 0;
	}

	pr_info("%s : live migration enabled in EFI\n", __func__);
	wrmsrq(MSR_KVM_MIGRATION_CONTROL, KVM_MIGRATION_READY);

	return 1;
}

late_initcall(setup_efi_kvm_sev_migration);

/*
 * Set the IPI entry points
 */
static __init void kvm_setup_pv_ipi(void)
{
	apic_update_callback(send_IPI_mask, kvm_send_ipi_mask);
	apic_update_callback(send_IPI_mask_allbutself, kvm_send_ipi_mask_allbutself);
	pr_info("setup PV IPIs\n");
}

static void kvm_smp_send_call_func_ipi(const struct cpumask *mask)
{
	int cpu;

	native_send_call_func_ipi(mask);

	/* Make sure other vCPUs get a chance to run if they need to. */
	for_each_cpu(cpu, mask) {
		if (!idle_cpu(cpu) && vcpu_is_preempted(cpu)) {
			kvm_hypercall1(KVM_HC_SCHED_YIELD, per_cpu(x86_cpu_to_apicid, cpu));
			break;
		}
	}
}

static void kvm_flush_tlb_multi(const struct cpumask *cpumask,
			const struct flush_tlb_info *info)
{
	u8 state;
	int cpu;
	struct kvm_steal_time *src;
	struct cpumask *flushmask = this_cpu_cpumask_var_ptr(__pv_cpu_mask);

	cpumask_copy(flushmask, cpumask);
	/*
	 * We have to call flush only on online vCPUs. And
	 * queue flush_on_enter for pre-empted vCPUs
	 */
	for_each_cpu(cpu, flushmask) {
		/*
		 * The local vCPU is never preempted, so we do not explicitly
		 * skip check for local vCPU - it will never be cleared from
		 * flushmask.
		 */
		src = &per_cpu(steal_time, cpu);
		state = READ_ONCE(src->preempted);
		if ((state & KVM_VCPU_PREEMPTED)) {
			if (try_cmpxchg(&src->preempted, &state,
					state | KVM_VCPU_FLUSH_TLB))
				__cpumask_clear_cpu(cpu, flushmask);
		}
	}

	native_flush_tlb_multi(flushmask, info);
}

static __init int kvm_alloc_cpumask(void)
{
	int cpu;

	if (!kvm_para_available() || nopv)
		return 0;

	if (pv_tlb_flush_supported() || pv_ipi_supported())
		for_each_possible_cpu(cpu) {
			zalloc_cpumask_var_node(per_cpu_ptr(&__pv_cpu_mask, cpu),
				GFP_KERNEL, cpu_to_node(cpu));
		}

	return 0;
}
arch_initcall(kvm_alloc_cpumask);

static void __init kvm_smp_prepare_boot_cpu(void)
{
	/*
	 * Map the per-cpu variables as decrypted before kvm_guest_cpu_init()
	 * shares the guest physical address with the hypervisor.
	 */
	sev_map_percpu_data();

	kvm_guest_cpu_init();
	native_smp_prepare_boot_cpu();
	kvm_spinlock_init();
}

static int kvm_cpu_down_prepare(unsigned int cpu)
{
	unsigned long flags;

	local_irq_save(flags);
	kvm_guest_cpu_offline(false);
	local_irq_restore(flags);
	return 0;
}

#endif

static int kvm_suspend(void)
{
	u64 val = 0;

	kvm_guest_cpu_offline(false);

#ifdef CONFIG_ARCH_CPUIDLE_HALTPOLL
	if (kvm_para_has_feature(KVM_FEATURE_POLL_CONTROL))
		rdmsrq(MSR_KVM_POLL_CONTROL, val);
	has_guest_poll = !(val & 1);
#endif
	return 0;
}

static void kvm_resume(void)
{
	kvm_cpu_online(raw_smp_processor_id());

#ifdef CONFIG_ARCH_CPUIDLE_HALTPOLL
	if (kvm_para_has_feature(KVM_FEATURE_POLL_CONTROL) && has_guest_poll)
		wrmsrq(MSR_KVM_POLL_CONTROL, 0);
#endif
}

static struct syscore_ops kvm_syscore_ops = {
	.suspend	= kvm_suspend,
	.resume		= kvm_resume,
};

static void kvm_pv_guest_cpu_reboot(void *unused)
{
	kvm_guest_cpu_offline(true);
}

static int kvm_pv_reboot_notify(struct notifier_block *nb,
				unsigned long code, void *unused)
{
	if (code == SYS_RESTART)
		on_each_cpu(kvm_pv_guest_cpu_reboot, NULL, 1);
	return NOTIFY_DONE;
}

static struct notifier_block kvm_pv_reboot_nb = {
	.notifier_call = kvm_pv_reboot_notify,
};

/*
 * After a PV feature is registered, the host will keep writing to the
 * registered memory location. If the guest happens to shutdown, this memory
 * won't be valid. In cases like kexec, in which you install a new kernel, this
 * means a random memory location will be kept being written.
 */
#ifdef CONFIG_CRASH_DUMP
static void kvm_crash_shutdown(struct pt_regs *regs)
{
	kvm_guest_cpu_offline(true);
	native_machine_crash_shutdown(regs);
}
#endif

#if defined(CONFIG_X86_32) || !defined(CONFIG_SMP)
bool __kvm_vcpu_is_preempted(long cpu);

__visible bool __kvm_vcpu_is_preempted(long cpu)
{
	struct kvm_steal_time *src = &per_cpu(steal_time, cpu);

	return !!(src->preempted & KVM_VCPU_PREEMPTED);
}
PV_CALLEE_SAVE_REGS_THUNK(__kvm_vcpu_is_preempted);

#else

#include <asm/asm-offsets.h>

extern bool __raw_callee_save___kvm_vcpu_is_preempted(long);

/*
 * Hand-optimize version for x86-64 to avoid 8 64-bit register saving and
 * restoring to/from the stack.
 */
#define PV_VCPU_PREEMPTED_ASM						     \
 "movq   __per_cpu_offset(,%rdi,8), %rax\n\t"				     \
 "cmpb   $0, " __stringify(KVM_STEAL_TIME_preempted) "+steal_time(%rax)\n\t" \
 "setne  %al\n\t"

DEFINE_ASM_FUNC(__raw_callee_save___kvm_vcpu_is_preempted,
		PV_VCPU_PREEMPTED_ASM, .text);
#endif

static void __init kvm_guest_init(void)
{
	int i;

	paravirt_ops_setup();
	register_reboot_notifier(&kvm_pv_reboot_nb);
	for (i = 0; i < KVM_TASK_SLEEP_HASHSIZE; i++)
		raw_spin_lock_init(&async_pf_sleepers[i].lock);

	if (kvm_para_has_feature(KVM_FEATURE_STEAL_TIME)) {
		has_steal_clock = 1;
		static_call_update(pv_steal_clock, kvm_steal_clock);

		pv_ops.lock.vcpu_is_preempted =
			PV_CALLEE_SAVE(__kvm_vcpu_is_preempted);
	}

	if (kvm_para_has_feature(KVM_FEATURE_PV_EOI))
		apic_update_callback(eoi, kvm_guest_apic_eoi_write);

	if (kvm_para_has_feature(KVM_FEATURE_ASYNC_PF_INT) && kvmapf) {
		static_branch_enable(&kvm_async_pf_enabled);
		sysvec_install(HYPERVISOR_CALLBACK_VECTOR, sysvec_kvm_asyncpf_interrupt);
	}

#ifdef CONFIG_SMP
	if (pv_tlb_flush_supported()) {
		pv_ops.mmu.flush_tlb_multi = kvm_flush_tlb_multi;
		pr_info("KVM setup pv remote TLB flush\n");
	}

	smp_ops.smp_prepare_boot_cpu = kvm_smp_prepare_boot_cpu;
	if (pv_sched_yield_supported()) {
		smp_ops.send_call_func_ipi = kvm_smp_send_call_func_ipi;
		pr_info("setup PV sched yield\n");
	}
	if (cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN, "x86/kvm:online",
				      kvm_cpu_online, kvm_cpu_down_prepare) < 0)
		pr_err("failed to install cpu hotplug callbacks\n");
#else
	sev_map_percpu_data();
	kvm_guest_cpu_init();
#endif

#ifdef CONFIG_CRASH_DUMP
	machine_ops.crash_shutdown = kvm_crash_shutdown;
#endif

	register_syscore_ops(&kvm_syscore_ops);

	/*
	 * Hard lockup detection is enabled by default. Disable it, as guests
	 * can get false positives too easily, for example if the host is
	 * overcommitted.
	 */
	hardlockup_detector_disable();
}

static noinline uint32_t __kvm_cpuid_base(void)
{
	if (boot_cpu_data.cpuid_level < 0)
		return 0;	/* So we don't blow up on old processors */

	if (boot_cpu_has(X86_FEATURE_HYPERVISOR))
		return cpuid_base_hypervisor(KVM_SIGNATURE, 0);

	return 0;
}

static inline uint32_t kvm_cpuid_base(void)
{
	static int kvm_cpuid_base = -1;

	if (kvm_cpuid_base == -1)
		kvm_cpuid_base = __kvm_cpuid_base();

	return kvm_cpuid_base;
}

bool kvm_para_available(void)
{
	return kvm_cpuid_base() != 0;
}
EXPORT_SYMBOL_GPL(kvm_para_available);

unsigned int kvm_arch_para_features(void)
{
	return cpuid_eax(kvm_cpuid_base() | KVM_CPUID_FEATURES);
}

unsigned int kvm_arch_para_hints(void)
{
	return cpuid_edx(kvm_cpuid_base() | KVM_CPUID_FEATURES);
}
EXPORT_SYMBOL_GPL(kvm_arch_para_hints);

static uint32_t __init kvm_detect(void)
{
	return kvm_cpuid_base();
}

static void __init kvm_apic_init(void)
{
#ifdef CONFIG_SMP
	if (pv_ipi_supported())
		kvm_setup_pv_ipi();
#endif
}

static bool __init kvm_msi_ext_dest_id(void)
{
	return kvm_para_has_feature(KVM_FEATURE_MSI_EXT_DEST_ID);
}

static void kvm_sev_hc_page_enc_status(unsigned long pfn, int npages, bool enc)
{
	kvm_sev_hypercall3(KVM_HC_MAP_GPA_RANGE, pfn << PAGE_SHIFT, npages,
			   KVM_MAP_GPA_RANGE_ENC_STAT(enc) | KVM_MAP_GPA_RANGE_PAGE_SZ_4K);
}

static void __init kvm_init_platform(void)
{
	if (cc_platform_has(CC_ATTR_GUEST_MEM_ENCRYPT) &&
	    kvm_para_has_feature(KVM_FEATURE_MIGRATION_CONTROL)) {
		unsigned long nr_pages;
		int i;

		pv_ops.mmu.notify_page_enc_status_changed =
			kvm_sev_hc_page_enc_status;

		/*
		 * Reset the host's shared pages list related to kernel
		 * specific page encryption status settings before we load a
		 * new kernel by kexec. Reset the page encryption status
		 * during early boot instead of just before kexec to avoid SMP
		 * races during kvm_pv_guest_cpu_reboot().
		 * NOTE: We cannot reset the complete shared pages list
		 * here as we need to retain the UEFI/OVMF firmware
		 * specific settings.
		 */

		for (i = 0; i < e820_table->nr_entries; i++) {
			struct e820_entry *entry = &e820_table->entries[i];

			if (entry->type != E820_TYPE_RAM)
				continue;

			nr_pages = DIV_ROUND_UP(entry->size, PAGE_SIZE);

			kvm_sev_hypercall3(KVM_HC_MAP_GPA_RANGE, entry->addr,
				       nr_pages,
				       KVM_MAP_GPA_RANGE_ENCRYPTED | KVM_MAP_GPA_RANGE_PAGE_SZ_4K);
		}

		/*
		 * Ensure that _bss_decrypted section is marked as decrypted in the
		 * shared pages list.
		 */
		early_set_mem_enc_dec_hypercall((unsigned long)__start_bss_decrypted,
						__end_bss_decrypted - __start_bss_decrypted, 0);

		/*
		 * If not booted using EFI, enable Live migration support.
		 */
		if (!efi_enabled(EFI_BOOT))
			wrmsrq(MSR_KVM_MIGRATION_CONTROL,
			       KVM_MIGRATION_READY);
	}
	kvmclock_init();
	x86_platform.apic_post_init = kvm_apic_init;

	/* Set WB as the default cache mode for SEV-SNP and TDX */
	guest_force_mtrr_state(NULL, 0, MTRR_TYPE_WRBACK);
}

#if defined(CONFIG_AMD_MEM_ENCRYPT)
static void kvm_sev_es_hcall_prepare(struct ghcb *ghcb, struct pt_regs *regs)
{
	/* RAX and CPL are already in the GHCB */
	ghcb_set_rbx(ghcb, regs->bx);
	ghcb_set_rcx(ghcb, regs->cx);
	ghcb_set_rdx(ghcb, regs->dx);
	ghcb_set_rsi(ghcb, regs->si);
}

static bool kvm_sev_es_hcall_finish(struct ghcb *ghcb, struct pt_regs *regs)
{
	/* No checking of the return state needed */
	return true;
}
#endif

const __initconst struct hypervisor_x86 x86_hyper_kvm = {
	.name				= "KVM",
	.detect				= kvm_detect,
	.type				= X86_HYPER_KVM,
	.init.guest_late_init		= kvm_guest_init,
	.init.x2apic_available		= kvm_para_available,
	.init.msi_ext_dest_id		= kvm_msi_ext_dest_id,
	.init.init_platform		= kvm_init_platform,
#if defined(CONFIG_AMD_MEM_ENCRYPT)
	.runtime.sev_es_hcall_prepare	= kvm_sev_es_hcall_prepare,
	.runtime.sev_es_hcall_finish	= kvm_sev_es_hcall_finish,
#endif
};

static __init int activate_jump_labels(void)
{
	if (has_steal_clock) {
		static_key_slow_inc(&paravirt_steal_enabled);
		if (steal_acc)
			static_key_slow_inc(&paravirt_steal_rq_enabled);
	}

	return 0;
}
arch_initcall(activate_jump_labels);

#ifdef CONFIG_PARAVIRT_SPINLOCKS

#include <asm/qspinlock.h>
#include <asm/ivh_tsc_beat.h>

/*
 * IVH non-halting paravirt-spinlock wait/kick substitute.
 *
 * Project thesis: mitigate lock-holder preemption in a KVM guest WITHOUT
 * hypervisor cooperation.  The stock kvm_wait() HLTs the waiting vCPU and
 * relies on the host waking it via a KVM_HC_KICK_CPU hypercall
 * (KVM_FEATURE_PV_UNHALT); kvm_kick_cpu() issues that hypercall.  That is
 * precisely the host-cooperative halt/kick this project rejects, and the HLT
 * also opens a steal window at the wake boundary (the host may re-place the
 * vCPU on an idle pCPU right as the lock frees).
 *
 * Instead we keep the vCPU in TASK_RUNNING and busy-wait with a bounded,
 * interrupt/TSC-terminated backoff, re-checking the exact condition word the
 * stock code waited on.  Correctness: both PV callers in
 * kernel/locking/qspinlock_paravirt.h (pv_wait_node() and
 * pv_wait_head_or_lock()) already wrap pv_wait() in a for(;;) loop that
 * re-checks node->locked / lock->locked afterwards and tolerates spurious
 * wakeups, so a bounded, always-"spurious" return is safe — no MCS or pv_hash
 * invariant depends on the wait actually blocking.  Because nobody is ever
 * truly blocked, the paired kick has nothing to wake and ivh_pv_kick() is a
 * no-op (the busy-waiter self-wakes when the lock word changes).
 *
 * The window constants below are conservative and tunable; they bound how
 * long a single ivh_pv_wait() naps before returning to let the PV slowpath
 * re-spin / re-hash and call again.
 */
#define IVH_PV_WAIT_TSC		65536ULL	/* max cycles per wait call */
#define IVH_PV_TPAUSE_CYCLES	512ULL		/* per-iteration C0.2 nap */

/*
 * IVH_PV_ADAPTIVE_TSC (~1 ms @ 3 GHz) is the deadline used instead of
 * IVH_PV_WAIT_TSC by ivh_pv_wait()'s mechanism==1 branch, paired with the
 * real pv_kick_node()/ivh_pv_kick() IPI wake added below (kernel/locking/
 * qspinlock_paravirt.h's pv_kick_node(), and ivh_pv_kick() further down this
 * file). The two are a package deal, not independent tunables:
 *
 *   - With no wake vehicle, IVH_PV_WAIT_TSC (22 us) is the right size: the
 *     deadline IS the wake, so it must be short.
 *   - With a real smp_send_reschedule() wake, the IPI is the common-case
 *     wake and the deadline becomes a worst-case backstop for a lost/
 *     misdelivered IPI, not the primary mechanism, so it can (and should,
 *     to avoid needless re-polling on every nap) be long.
 *
 * Do not widen this without the IPI wake in place, and do not add the IPI
 * wake while leaving the deadline at IVH_PV_WAIT_TSC — see
 * tools/bpf/docs/ivh_adaptive_tpause_ipi_plan_2026-07-22.md secs 4.1/5.1.
 */
#define IVH_PV_ADAPTIVE_TSC	3000000ULL	/* max cycles/wait, mechanism==1 */

DEFINE_PER_CPU(u64, ivh_pv_wait_calls);		/* surfaced in /proc/ivh_debug */

/*
 * Runtime toggle for which pv_wait()/pv_kick()/pv_wait_early() behavior
 * ivh_pv_wait(), ivh_pv_kick() and kernel/locking/qspinlock_paravirt.h's
 * pv_wait_early() use.  Declared extern in arch/x86/include/asm/qspinlock.h
 * (see there for the full comment on the two states and why this — and not
 * virt_spin_lock_key / pv_ops.lock.* registration below — is the runtime-
 * toggleable knob).  Default 0: OFF, safest, closest to pre-IVH behavior.
 *
 * Deliberately *not* placed in kernel/sched/bpf_sched.c's ivh_sysctls table:
 * this mechanism has nothing to do with CONFIG_BPF_SYSCALL and must remain
 * usable (and this sysctl must remain registered) independent of it.
 */
unsigned long ivh_pv_wait_mechanism = 0UL;

/*
 * Forensic tracing for the mechanism==2 wait/kick path. Default OFF (0);
 * enable live with `sysctl kernel.ivh_pv_wait_trace=1`, disable the same way.
 * Never touched by the mechanism logic itself, so leaving it 0 costs one
 * READ_ONCE + branch per call site -- safe to leave compiled in permanently.
 *
 * --- Why this exists, and why it is plain printk(KERN_EMERG ...) and not
 * trace_printk()/ftrace, a custom per-CPU ring buffer, or pstore ---
 *
 * This project had exactly one confirmed hard freeze (2026-07-24): mechanism
 * 2's old code called raw halt() on a CPU that already had RFLAGS.IF=0. Per
 * the Intel SDM / AMD APM (see the long comment on the mechanism==2 branch
 * below) a maskable interrupt cannot un-halt a HLT taken with IF=0 -- only
 * NMI/SMI/INIT/RESET can. That CPU was gone permanently: no more ticks, no
 * more IPIs serviced, and (because it was holding up an MCS queue) every
 * other CPU waiting behind it eventually wedged too. journalctl -k -b -1 for
 * that boot shows total silence from the moment of the freeze -- not even
 * BUG()/WARN()/soft-lockup/RCU-stall output, because producing any of that
 * also requires a CPU that can still run code, which by then none could.
 *
 * That failure mode is the design constraint: any instrumentation that
 * depends on *something running later* -- a kthread flushing a buffer, an
 * irq_work callback, ftrace's deferred drain, a future reschedule -- is
 * worthless here, because "later" never comes once every CPU is IF=0-halted.
 * The only work that survives is whatever the instruction stream already
 * completed *before* the fatal halt() executes.
 *
 * printk()'s ring-buffer append (kernel/printk/printk_ringbuffer.c, the
 * lockless printk_ringbuffer merged in 5.10) is exactly that: a synchronous,
 * non-blocking reservation+commit into an in-memory buffer done entirely by
 * the calling CPU, with no dependency on a console driver, another CPU, or
 * sleeping -- callable from any context including IRQs-disabled and NMI.
 * Placed on the line immediately before a halt()/safe_halt(), the record is
 * durably in the ring buffer before that halt executes; there is nothing
 * left to lose after the store retires, halted-forever CPU or not.
 *
 * Getting it OFF the guest then needs exactly one thing: some CPU, at some
 * point before or during the freeze, still alive long enough for journald to
 * drain /dev/kmsg (which is fed directly off this same ring buffer) to disk.
 * This is not a theoretical hope -- it is what already happened in our one
 * real incident: vcap's own unrelated per-CPU latency dump (plain pr_info,
 * no special handling at all) shows up cleanly in `journalctl -k -b -1` for
 * every CPU right up through the exact instant of the freeze (CPU 14's line
 * printed; CPU 15's never did). printk already survives this precise freeze
 * mode today, on this kernel/qemu/journald stack, with zero extra effort.
 * These trace points ride the same, already-proven path.
 *
 * Two alternatives were checked and rejected for this specific failure mode:
 *   - pstore (CONFIG_PSTORE=y) has no backend compiled in on this build
 *     (CONFIG_PSTORE_RAM and CONFIG_PSTORE_BLK are both unset; runtime
 *     /sys/module/pstore/parameters/backend reads "(null)") -- it persists
 *     nothing across the hypervisor's forced reset today. Wiring up a
 *     ramoops region needs a reserved-memory boot param / QEMU change, out
 *     of scope for a code-only fix, and unnecessary given the above.
 *   - The NMI-driven hardlockup detector is compiled in
 *     (CONFIG_HARDLOCKUP_DETECTOR_PERF=y) but reads 0 at runtime
 *     (/proc/sys/kernel/nmi_watchdog), almost certainly because this nested/
 *     cloud KVM guest has no usable vPMU to drive the perf event it needs.
 *     It is not a safety net here; do not assume it will fire.
 *
 * KERN_EMERG, not _INFO/_DEBUG: journald reads /dev/kmsg, which is fed off
 * the ring buffer regardless of level, so level does not gate whether next
 * boot's `journalctl -k -b -1` captures the line -- that part is already
 * satisfied at any level. EMERG is used anyway so these lines (a) are
 * impossible to miss/grep past among IVH's/vcap's routine _INFO chatter, and
 * (b) get console_verbose()-style priority for anyone watching a live serial
 * console at the moment it happens.
 *
 * Left OFF by default because mechanism==2's wait path is hot (every
 * contended lock acquisition that reaches it): tracing every call at high
 * lock-contention rates can itself perturb timing and flood the log/journal
 * faster than journald can drain it. Turn on only when actively chasing a
 * suspected repeat of this failure, not for routine use.
 */
unsigned long ivh_pv_wait_trace = 0UL;

/*
 * Make ivh_pv_kick()'s wake a *pure* IPI: skip the KVM_HC_KICK_CPU hypercall
 * entirely for nonzero mechanisms and send only smp_send_reschedule().
 * Default 0 (OFF) = current behavior, hypercall + IPI both.
 *
 * Why this knob exists: mechanism 2's claim is "a real HLT yield woken by a
 * plain APIC IPI, no paravirt wake vehicle needed". As shipped, ivh_pv_kick()
 * issues the hypercall *in addition to* the IPI for every nonzero mechanism
 * (see its comment), which is correct for safety but makes any mechanism-0 vs
 * mechanism-2 A/B unable to claim mechanism 2 avoids paravirtualization — the
 * hypercall is still on the wire. Setting this to 1 gives a genuinely
 * hypercall-free kick to measure against.
 *
 * MUTUALLY EXCLUSIVE WITH ivh_pv_wait_mechanism == 0, enforced by the proc
 * handlers below (not by documentation): mechanism 0 is the only path that
 * still reaches a bare halt() with RFLAGS.IF=0 (ivh_pv_wait()'s PV_UNHALT
 * branch), and per the Intel SDM / AMD APM a maskable RESCHEDULE_VECTOR IPI
 * cannot un-halt that -- only KVM_HC_KICK_CPU's pv_unhalted can. Dropping the
 * hypercall while any waiter can be parked there is the exact hard-freeze that
 * was diagnosed and fixed on 2026-07-24. Mechanism 2 (post-fix) never halts
 * with IRQs already disabled -- it falls through to the bounded poll instead
 * (see ivh_pv_wait()) -- so mechanism 2 plus a pure-IPI kick does NOT reopen
 * that hole, and mechanism 1 never halts at all.
 *
 * Mechanism 3 never halts either, and its kick sends no IPI at all, so here
 * this knob means "suppress the belt-and-braces hypercall too", i.e. make the
 * kick a complete no-op. Same caveat as mechanism 2: it is safe with respect
 * to anything mechanism 3 itself parked (nothing), not with respect to a
 * waiter left over from a live toggle out of mechanism 0. See ivh_pv_kick().
 */
unsigned long ivh_pv_kick_pure_ipi = 0UL;

/*
 * ---------------------------------------------------------------------------
 * IVH per-CPU TSC heartbeat -- storage, knobs and validation counters.
 * Declared (with the full design comment) in arch/x86/include/asm/qspinlock.h.
 * ---------------------------------------------------------------------------
 */
DEFINE_PER_CPU_ALIGNED(struct ivh_tsc_beat, ivh_tsc_beat);
EXPORT_PER_CPU_SYMBOL_GPL(ivh_tsc_beat);

unsigned long ivh_pv_preempt_src = 0UL;		/* 0 = KVM bit (default) */
/*
 * 3,300,000 cycles = 1.5 ms at 2200 MHz.  This is NOT a guess and it is NOT
 * the final value: it is is_cpu_preempted()'s existing 1,500,000 ns threshold
 * (kernel/sched/cputime.c) expressed in cycles, which makes Phase 1 a
 * controlled reproduction of the signal this tree already has.  Recomputed
 * from the live tsc_khz at late_initcall so the knob survives a different
 * host; the literal below is only the pre-calibration fallback.  For Phase 2,
 * do NOT pick a number up front -- read it off the separation point of the
 * two ivh_beat_age_hist_* distributions.
 */
unsigned long ivh_pv_beat_threshold = 3300000UL;
#define IVH_BEAT_THRESHOLD_US	1500ULL
unsigned long ivh_pv_beat_publish_mask = 0xfffUL;

DEFINE_PER_CPU(u64, ivh_beat_agree_true);
DEFINE_PER_CPU(u64, ivh_beat_agree_false);
DEFINE_PER_CPU(u64, ivh_beat_false_pos);
DEFINE_PER_CPU(u64, ivh_beat_false_neg);
DEFINE_PER_CPU(u64, ivh_beat_publishes);
DEFINE_PER_CPU(s64, ivh_beat_min_age) = S64_MAX;
DEFINE_PER_CPU(u64, ivh_beat_age_hist_running[IVH_BEAT_AGE_HIST_BUCKETS]);
DEFINE_PER_CPU(u64, ivh_beat_age_hist_preempted[IVH_BEAT_AGE_HIST_BUCKETS]);

/*
 * ---------------------------------------------------------------------------
 * IVH critical-section stamp -- storage, knobs and counters.
 * Declared, with the full "what this predicate actually measures" writeup, in
 * <asm/ivh_tsc_beat.h>.  Design: tools/bpf/docs/
 * ivh_tsc_full_redesign_build_plan_2026-07-29.md sec 3.2 and sec 1.2.
 * ---------------------------------------------------------------------------
 */
DEFINE_PER_CPU_ALIGNED(struct ivh_cs_beat, ivh_cs_beat);
EXPORT_PER_CPU_SYMBOL_GPL(ivh_cs_beat);

unsigned long ivh_cs_preempt_src = 0UL;		/* 0 = off (default) */
EXPORT_SYMBOL_GPL(ivh_cs_preempt_src);
/*
 * Form 1 by default -- "in a CS AND the liveness heartbeat is stale" rather
 * than "in a CS for a long time".  See the two-form table in
 * <asm/ivh_tsc_beat.h>: form 1 is strictly better founded, and defaulting to
 * it means that if nobody ever gets round to running the form-0-vs-form-1 A/B
 * the shipped behaviour is still the defensible one.  Form 0 remains
 * reachable by sysctl precisely so that A/B costs an echo rather than a boot.
 */
unsigned long ivh_cs_predicate_form = 1UL;
EXPORT_SYMBOL_GPL(ivh_cs_predicate_form);

/*
 * Form 0's threshold, in raw TSC cycles.  220,000 cycles = 100 us at
 * 2200 MHz; recomputed from the live tsc_khz at late_initcall so the knob
 * survives a different host, with the literal below as the pre-calibration
 * fallback -- identical treatment to ivh_pv_beat_threshold above.
 *
 * WHY 100 us AND NOT THE HEARTBEAT'S 1500 us.  The two thresholds answer
 * different questions and sharing a number would be a category error.  The
 * wait stamp's 1500 us is deliberately is_cpu_preempted()'s existing
 * threshold, because that makes the heartbeat a controlled reproduction of a
 * signal this tree already has.  The CS stamp has no such counterpart: its
 * age is a HOLD DURATION, and the relevant population is the kernel's real
 * spinlock hold-time distribution, whose bulk on this tree's measured
 * workloads sits at 120-295 ns.  100 us is roughly nine doublings above that
 * bulk and is also exactly IVH_HOT_STEAL_FLOOR_NS (kernel/locking/spinlock.c),
 * which is the number this project already uses for "this hold was long
 * enough that host steal is the plausible explanation".  Reusing it keeps the
 * two hold-length judgements in the tree consistent with each other.
 *
 * It is a SWEEP SEED, not a committed value.  The authoritative calibration
 * is the separation point of ivh_cs_age_hist_running[] against
 * ivh_cs_age_hist_preempted[], cross-checked against ivh_cs_hold_hist[]'s
 * p99.9 -- which is why all three histograms ship in this same build.
 */
unsigned long ivh_cs_beat_threshold = 220000UL;
#define IVH_CS_BEAT_THRESHOLD_US	100ULL
EXPORT_SYMBOL_GPL(ivh_cs_beat_threshold);

DEFINE_PER_CPU(u64, ivh_cs_checks);
DEFINE_PER_CPU(u64, ivh_cs_publishes);
DEFINE_PER_CPU(u64, ivh_cs_agree_true);
DEFINE_PER_CPU(u64, ivh_cs_agree_false);
DEFINE_PER_CPU(u64, ivh_cs_false_pos);
DEFINE_PER_CPU(u64, ivh_cs_false_neg);
DEFINE_PER_CPU(u64, ivh_cs_clear_mismatch);
DEFINE_PER_CPU(u64, ivh_cs_age_hist_running[IVH_CS_AGE_HIST_BUCKETS]);
DEFINE_PER_CPU(u64, ivh_cs_age_hist_preempted[IVH_CS_AGE_HIST_BUCKETS]);
DEFINE_PER_CPU(u64, ivh_cs_hold_hist[IVH_CS_HOLD_HIST_BUCKETS]);
EXPORT_PER_CPU_SYMBOL_GPL(ivh_cs_publishes);
EXPORT_PER_CPU_SYMBOL_GPL(ivh_cs_clear_mismatch);
EXPORT_PER_CPU_SYMBOL_GPL(ivh_cs_hold_hist);

DEFINE_PER_CPU(u64, ivh_holder_stamps);
DEFINE_PER_CPU(u64, ivh_holder_clears);
DEFINE_PER_CPU(u64, ivh_holder_unknown_empty);
DEFINE_PER_CPU(u64, ivh_holder_unknown_collision);
DEFINE_PER_CPU(u64, ivh_holder_raced);
DEFINE_PER_CPU(u64, ivh_holder_self);

DEFINE_PER_CPU(u64, ivh_head_bail_early);
DEFINE_PER_CPU(u64, ivh_head_bail_loop_hist[IVH_CS_LOOP_HIST_BUCKETS]);
DEFINE_PER_CPU(u64, ivh_lock_steals);

/*
 * ---------------------------------------------------------------------------
 * IVH lock-holder identity -- Option B, the direct-mapped side table.
 * API and the full "why not cs_enter()/cs_exit()" argument in
 * <linux/ivh_lock_holder.h>.  Design: build plan sec 3.3.
 * ---------------------------------------------------------------------------
 *
 * DIRECT-MAPPED AND LOSSY BY DESIGN.  One slot per hash bucket, no collision
 * resolution, no locking, no BUG().  On tag mismatch the answer is "unknown",
 * and "unknown" is the safe direction everywhere it is consumed.  A chained
 * or locked table would have to be correct under contention on the qspinlock
 * fast path, which is precisely the cost this whole exercise is trying to
 * measure rather than pay.
 *
 * Allocated ONCE at IVH_HOLDER_MAX_BITS and indexed at the width of the
 * ivh_holder_bits sysctl -- see that header for why making the geometry
 * runtime-sweepable is what turns "how big must the table be" from a
 * one-rebuild-per-data-point question into a single-boot measurement.
 *
 * vzalloc() rather than kzalloc(): 65536 x 64 B is 4 MB, which is at the very
 * top of the buddy allocator's reach and would be a pointless
 * physically-contiguous demand for a structure that is only ever touched
 * through ordinary loads and stores.  vmalloc memory is fully populated at
 * allocation, so there is no fault risk from the lock path.
 */
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

	/*
	 * The table is published at late_initcall, but ivh_lock_holder_enabled
	 * can only be written through a sysctl that is itself registered at
	 * late_initcall and that refuses to arm without a table -- so this NULL
	 * check is belt-and-braces against a future caller, not a live case.
	 * It costs one predicted branch on a path that is already behind the
	 * enabled-gate, i.e. nothing at all in the default configuration.
	 */
	if (unlikely(!table))
		return NULL;

	return &table[hash_ptr(lock, READ_ONCE(ivh_holder_bits))];
}

/*
 * Stamp: this CPU now owns @lock.  Called STRICTLY AFTER the acquiring
 * operation at every one of the nine ownership-transfer sites (build plan
 * sec 3.3.4).
 *
 * MEMORY ORDERING, and the reason must be stated correctly because an earlier
 * planning round stated it wrongly.  Stamping before the acquire would
 * publish a holder for a lock not yet held -- a reader would then attribute a
 * critical section to a CPU that never entered one, which is the dangerous
 * direction.  Placing the plain WRITE_ONCE after the acquire needs no extra
 * barrier on x86, but the reason is STORE->STORE ORDERING UNDER x86-TSO, NOT
 * "the locked RMW fences it": three of the live transfer sites are not RMWs
 * at all (set_locked() and clear_pending_set_locked() are plain WRITE_ONCEs,
 * and the uncontended queue-head acquisition is atomic_try_cmpxchg_RELAXED).
 * Do not "simplify" this comment back to the RMW claim; it is false at those
 * sites and would mislead the next reader.
 *
 * holder_cpu is written BEFORE tag for the same TSO reason: a reader checks
 * the tag first, so publishing the tag last guarantees that any reader which
 * sees a matching tag also sees the matching holder_cpu.
 */
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

/*
 * Clear: called STRICTLY BEFORE the releasing store at all four release
 * sites.  All four are smp_store_release() or try_cmpxchg_release(), both of
 * which order every prior store ahead of the release, so a plain WRITE_ONCE
 * immediately above cannot be moved past it and needs no extra barrier.
 *
 * THE TAG IS CLEARED TOO, AND THAT IS NOT TIDINESS.  Option B adds a case
 * that a pure "zero the holder" clear would miss: the same lock ADDRESS can
 * be freed and reallocated, so a slot whose tag still matches can hand back a
 * long-departed holder.  A slot with a cleared tag fails the tag check and
 * reads "unknown", which is the safe direction.  Tag first, then holder_cpu,
 * mirroring the publish order.
 *
 * The tag check before clearing is what stops us wiping a DIFFERENT lock's
 * live holder after a hash collision.  It also means ivh_holder_clears can
 * legitimately trail ivh_holder_stamps: the gap is (collided stamps) plus
 * (holds currently in flight).  A gap that grows without bound under a steady
 * workload is the signal that an ownership-transfer site was missed.
 */
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

/*
 * Look up @lock's recorded holder, or -1 for unknown.
 *
 * The two unknown causes are counted SEPARATELY and that separation is the
 * whole point of the counter pair.  An empty slot is the genuine handoff
 * window -- the old holder cleared and released, the new holder has acquired
 * but has not stamped yet -- and it is irreducible physics that no table size
 * can remove.  A slot occupied by a different tag is table geometry, and it
 * is exactly what the ivh_holder_bits sweep drives toward zero.  A single
 * lumped "unknown" counter could not tell those apart, and "is the table big
 * enough" would then be unanswerable without a rebuild per data point.
 */
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
 * Wipe the table.  Called from the ivh_holder_bits proc handler on every
 * geometry change, because slots written under the old index width are
 * indistinguishable from collisions under the new one -- without this the
 * first measurement window after every sweep step would report the previous
 * geometry's residue as ivh_holder_unknown_collision and the whole curve
 * would be garbage.
 *
 * Deliberately racy against concurrent stampers: the table is lossy by
 * construction, so the worst outcome is a handful of extra "unknown" answers
 * during the memset, which is exactly the direction that is already safe.
 * Taking a lock here would put a lock on the lock path's own state, which is
 * not a trade this design is willing to make for a sysctl write.
 */
static void ivh_holder_table_clear(void)
{
	struct ivh_holder_slot *table = READ_ONCE(ivh_holder_table);

	if (table)
		memset(table, 0, ivh_holder_table_slots * sizeof(*table));
}

/*
 * Calibrate the staleness threshold against the machine's real TSC rate.
 * late_initcall so tsc_khz is long since established (tsc_init() runs from
 * setup_arch()); if it somehow is not, keep the 2200 MHz literal rather than
 * publishing a zero threshold, which would make every heartbeat read stale
 * and -- at ivh_pv_preempt_src == 2 -- turn pv_wait_early() into "always
 * bail out early", the worst possible failure direction.
 */
static int __init ivh_pv_beat_calibrate(void)
{
	if (tsc_khz)
		ivh_pv_beat_threshold = (unsigned long)((u64)tsc_khz *
					IVH_BEAT_THRESHOLD_US / 1000ULL);

	pr_info("IVH: TSC heartbeat threshold = %lu cycles (%llu us at tsc_khz=%u)\n",
		ivh_pv_beat_threshold, IVH_BEAT_THRESHOLD_US, tsc_khz);

	/*
	 * Same calibration, same fallback discipline, different question -- see
	 * the long comment on ivh_cs_beat_threshold above for why the two
	 * numbers are deliberately not the same.  Keeping the fallback literal
	 * rather than publishing a zero matters for the identical reason: a zero
	 * threshold makes every hold read as preempted, and at
	 * ivh_cs_preempt_src == 2 that is "the queue head always bails", the
	 * worst possible failure direction.
	 */
	if (tsc_khz)
		ivh_cs_beat_threshold = (unsigned long)((u64)tsc_khz *
					IVH_CS_BEAT_THRESHOLD_US / 1000ULL);

	pr_info("IVH: CS stamp threshold = %lu cycles (%llu us at tsc_khz=%u), predicate form %lu\n",
		ivh_cs_beat_threshold, IVH_CS_BEAT_THRESHOLD_US, tsc_khz,
		ivh_cs_predicate_form);

	/*
	 * Part C's jump threshold shares this calibration hook even though the
	 * variable itself lives in kernel/sched/bpf_sched.c beside the other
	 * scheduler-side IVH knobs.  It is calibrated HERE rather than there
	 * because tsc_khz is an x86 concept and this is already the one
	 * late_initcall in the tree whose job is "turn microseconds into this
	 * host's cycles"; duplicating that in generic scheduler code would put
	 * an #ifdef CONFIG_X86 initcall in kernel/sched/ for no gain.
	 *
	 * 1500 us deliberately equals IVH_BEAT_THRESHOLD_US and equals
	 * is_cpu_preempted()'s `> 1500000` ns, so the tick stamp's jump
	 * detector is a controlled comparison against the preemption signal
	 * this tree already has rather than a new signal with a new tuning
	 * surface.
	 */
	if (tsc_khz)
		ivh_vact_jump_threshold = (unsigned long)((u64)tsc_khz *
					  IVH_BEAT_THRESHOLD_US / 1000ULL);

	pr_info("IVH: Part C jump threshold = %lu cycles (%llu us at tsc_khz=%u), window %lu ns\n",
		ivh_vact_jump_threshold, IVH_BEAT_THRESHOLD_US, tsc_khz,
		ivh_vact_window_ns);

	/*
	 * Allocate the holder side table at its MAXIMUM geometry, once, here.
	 * Everything about the sizing experiment depends on this being a
	 * runtime sweep rather than a build-time constant -- see
	 * <linux/ivh_lock_holder.h>.  A failure is not fatal: without a table
	 * ivh_holder_slot_of() returns NULL, every lookup answers "unknown",
	 * the proc handler refuses to arm ivh_lock_holder_enabled, and the rest
	 * of the kernel is entirely unaffected.  Log it and carry on rather
	 * than failing an initcall over an observability feature.
	 */
	ivh_holder_table_slots = 1UL << IVH_HOLDER_MAX_BITS;
	ivh_holder_table = vzalloc(ivh_holder_table_slots *
				   sizeof(struct ivh_holder_slot));
	if (!ivh_holder_table) {
		ivh_holder_table_slots = 0;
		pr_err("IVH: lock-holder side table allocation failed (%lu slots x %zu B); ivh_lock_holder_enabled cannot be armed this boot\n",
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
late_initcall(ivh_pv_beat_calibrate);

#ifdef CONFIG_SYSCTL
/*
 * Shared rejection notice for the {mechanism==0, pure_ipi==1} combination.
 * pr_err (not pr_warn): a write that returns -EINVAL did nothing, and the
 * caller deserves to see why in dmesg next to their failed sysctl.
 */
static void ivh_pv_reject_unsafe_combo(const char *what)
{
	pr_err("IVH: refusing %s: ivh_pv_wait_mechanism=0 halts with RFLAGS.IF=0 and can ONLY be woken by the KVM_HC_KICK_CPU hypercall, which ivh_pv_kick_pure_ipi=1 suppresses -- that combination strands halted waiters and freezes the VM. Change the other knob first.\n",
	       what);
}

/*
 * Both handlers parse into a local and only publish to the global once the
 * combination has been validated, so the pair is never transiently in the
 * unsafe {0,1} state that a concurrent ivh_pv_kick() could observe. This is
 * what makes the guard order-independent: whichever knob is written second is
 * the one that gets rejected, and a live toggle of the mechanism back to 0
 * while pure_ipi is already 1 is rejected by exactly the same check.
 */
static int ivh_pv_proc_wait_mechanism(const struct ctl_table *table, int write,
				      void *buffer, size_t *lenp, loff_t *ppos)
{
	unsigned long val = READ_ONCE(ivh_pv_wait_mechanism);
	struct ctl_table tmp = *table;
	int ret;

	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (val == 0 && READ_ONCE(ivh_pv_kick_pure_ipi)) {
		ivh_pv_reject_unsafe_combo("ivh_pv_wait_mechanism=0 while ivh_pv_kick_pure_ipi=1");
		return -EINVAL;
	}

	WRITE_ONCE(ivh_pv_wait_mechanism, val);
	return 0;
}

static int ivh_pv_proc_kick_pure_ipi(const struct ctl_table *table, int write,
				     void *buffer, size_t *lenp, loff_t *ppos)
{
	unsigned long val = READ_ONCE(ivh_pv_kick_pure_ipi);
	struct ctl_table tmp = *table;
	int ret;

	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (val && !READ_ONCE(ivh_pv_wait_mechanism)) {
		ivh_pv_reject_unsafe_combo("ivh_pv_kick_pure_ipi=1 while ivh_pv_wait_mechanism=0");
		return -EINVAL;
	}

	WRITE_ONCE(ivh_pv_kick_pure_ipi, val);
	return 0;
}

/*
 * ivh_pv_preempt_src: reject anything above 2, and refuse to make the
 * heartbeat AUTHORITATIVE (2) until every online CPU has actually published
 * at least once.
 *
 * The second guard is not paperwork.  An unseeded slot reads 0, so
 * ivh_beat_age() returns a full rdtsc() -- astronomically past any threshold
 * -- and pv_wait_early() would report "preempted" for that vCPU forever.  At
 * src == 2 that is not a counted disagreement, it is the live signal, and the
 * failure is silent: no crash, no warning, just every waiter behind that vCPU
 * bailing out of its spin immediately, every time.  The tick publish in
 * account_process_tick() is unconditional, so one tick after boot this
 * condition is satisfied on every ticking CPU; if it is NOT satisfied, that
 * itself is the finding and the write should fail rather than paper over it.
 *
 * Same cross-knob-validation shape as ivh_pv_proc_wait_mechanism() above:
 * parse into a local, validate, and only then publish to the global, so no
 * concurrent reader ever observes a transiently-invalid value.
 */
static int ivh_pv_proc_preempt_src(const struct ctl_table *table, int write,
				   void *buffer, size_t *lenp, loff_t *ppos)
{
	unsigned long val = READ_ONCE(ivh_pv_preempt_src);
	struct ctl_table tmp = *table;
	int ret, cpu;

	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (val > 2) {
		pr_err("IVH: refusing ivh_pv_preempt_src=%lu: valid values are 0 (KVM steal bit), 1 (shadow compare, still returns the KVM bit) and 2 (TSC heartbeat authoritative)\n",
		       val);
		return -EINVAL;
	}

	if (val == 2) {
		for_each_online_cpu(cpu) {
			if (!READ_ONCE(per_cpu(ivh_tsc_beat, cpu).stamp)) {
				pr_err("IVH: refusing ivh_pv_preempt_src=2: CPU %d has never published a TSC heartbeat, so it would read as permanently preempted. Leave src at 0/1 and check that account_process_tick() is running there.\n",
				       cpu);
				return -EINVAL;
			}
		}
	}

	WRITE_ONCE(ivh_pv_preempt_src, val);
	return 0;
}

/*
 * The publish mask must stay coarser than or equal to PV_PREV_CHECK_MASK
 * (0xff, kernel/locking/qspinlock_paravirt.h) and must be a (2^n - 1) form,
 * because the spin loops test `(loop & mask) == 0`.  A non-contiguous mask
 * would still "work" but its publish cadence would be unreadable, and a mask
 * finer than the read cadence is pure interconnect waste by construction.
 */
static int ivh_pv_proc_beat_publish_mask(const struct ctl_table *table, int write,
					 void *buffer, size_t *lenp, loff_t *ppos)
{
	unsigned long val = READ_ONCE(ivh_pv_beat_publish_mask);
	struct ctl_table tmp = *table;
	int ret;

	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	/* 0xff is PV_PREV_CHECK_MASK; that macro is private to
	 * kernel/locking/qspinlock_paravirt.h, which is not includable here. */
	if (val < 0xffUL || (val & (val + 1))) {
		pr_err("IVH: refusing ivh_pv_beat_publish_mask=0x%lx: must be of the form 2^n-1 and >= PV_PREV_CHECK_MASK (0xff)\n",
		       val);
		return -EINVAL;
	}

	WRITE_ONCE(ivh_pv_beat_publish_mask, val);
	return 0;
}

/*
 * ivh_cs_preempt_src: reject anything above 2, and refuse to make the CS
 * predicate AUTHORITATIVE (2) unless holder stamping is already armed.
 *
 * The second guard is the CS-side analogue of ivh_pv_proc_preempt_src()'s
 * "has every CPU published a heartbeat" check, and it is here for the same
 * class of reason -- to refuse a configuration that would be a SILENT no-op
 * rather than a visible failure.  At src == 2 with
 * ivh_lock_holder_enabled == 0, ivh_lock_holder_cpu() answers "unknown" for
 * every lock forever, so ivh_cs_head_check() returns false on every call:
 * every counter would read zero, every histogram would stay empty, and the
 * natural conclusion from that data ("the predicate never fires, so it is
 * useless") would be entirely wrong and would have cost a full measurement
 * window to reach.
 *
 * Same parse-into-a-local, validate, publish-only-when-valid shape as the two
 * handlers above, so a concurrent reader never observes a transiently
 * invalid value.  Note that src == 1 (shadow) is deliberately NOT gated on
 * the holder table: running the shadow path with holder identity off is a
 * legitimate configuration whose entire output is
 * "ivh_holder_unknown_empty == ivh_cs_checks", which is a perfectly good
 * control measurement.
 */
static int ivh_pv_proc_cs_preempt_src(const struct ctl_table *table, int write,
				      void *buffer, size_t *lenp, loff_t *ppos)
{
	unsigned long val = READ_ONCE(ivh_cs_preempt_src);
	struct ctl_table tmp = *table;
	int ret;

	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (val > 2) {
		pr_err("IVH: refusing ivh_cs_preempt_src=%lu: valid values are 0 (off), 1 (shadow compare, queue head never bails) and 2 (authoritative)\n",
		       val);
		return -EINVAL;
	}

	if (val == 2 && !READ_ONCE(ivh_lock_holder_enabled)) {
		pr_err("IVH: refusing ivh_cs_preempt_src=2 while ivh_lock_holder_enabled=0: with no holder identity every lookup answers \"unknown\", so the predicate would never fire and every counter would read zero -- a silent no-op, not a measurement. Set ivh_lock_holder_enabled=1 first.\n");
		return -EINVAL;
	}

	WRITE_ONCE(ivh_cs_preempt_src, val);
	return 0;
}

/*
 * ivh_cs_predicate_form: 0 or 1 only.  See the two-form table in
 * <asm/ivh_tsc_beat.h>.  A third value would silently select form 0 (the
 * expression is a plain truth test), so reject rather than let a typo change
 * which predicate a measurement window was actually running.
 */
static int ivh_pv_proc_cs_predicate_form(const struct ctl_table *table, int write,
					 void *buffer, size_t *lenp, loff_t *ppos)
{
	unsigned long val = READ_ONCE(ivh_cs_predicate_form);
	struct ctl_table tmp = *table;
	int ret;

	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (val > 1) {
		pr_err("IVH: refusing ivh_cs_predicate_form=%lu: valid values are 0 (CS-stamp age > ivh_cs_beat_threshold) and 1 (in a CS AND liveness heartbeat stale)\n",
		       val);
		return -EINVAL;
	}

	WRITE_ONCE(ivh_cs_predicate_form, val);
	return 0;
}

/*
 * ivh_lock_holder_enabled: 0 or 1, refuse 1 without a table, and refuse 0
 * while ivh_cs_preempt_src is authoritative.
 *
 * The last guard is the exact mirror of ivh_cs_preempt_src's, and having both
 * is what makes the pair order-independent in the same way
 * ivh_pv_proc_wait_mechanism()/ivh_pv_proc_kick_pure_ipi() are: whichever
 * knob is written into the incoherent combination is the one that gets
 * rejected, so there is no write order that can sneak
 * {holder off, predicate authoritative} past the check.
 *
 * This knob is deliberately SEPARATE from ivh_cs_preempt_src rather than
 * folded into it, because the two questions it makes answerable are
 * independent: enabled=1 with src=0 measures nothing but the COST of the
 * store on the qspinlock ownership path, which is the one thing that decides
 * whether any of this can ship at all (build plan sec 1.1) and which is
 * unanswerable if the predicate's own work is mixed into the same A/B.
 */
static int ivh_pv_proc_lock_holder_enabled(const struct ctl_table *table, int write,
					   void *buffer, size_t *lenp, loff_t *ppos)
{
	unsigned long val = READ_ONCE(ivh_lock_holder_enabled);
	struct ctl_table tmp = *table;
	int ret;

	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (val > 1) {
		pr_err("IVH: refusing ivh_lock_holder_enabled=%lu: valid values are 0 (no stamping, one predicted branch per acquire/release) and 1 (stamp and clear)\n",
		       val);
		return -EINVAL;
	}

	if (val && !READ_ONCE(ivh_holder_table)) {
		pr_err("IVH: refusing ivh_lock_holder_enabled=1: the holder side table failed to allocate at late_initcall, so every stamp would be discarded and every lookup would answer \"unknown\". Check the earlier IVH allocation error in dmesg.\n");
		return -EINVAL;
	}

	if (!val && READ_ONCE(ivh_cs_preempt_src) == 2) {
		pr_err("IVH: refusing ivh_lock_holder_enabled=0 while ivh_cs_preempt_src=2: that combination leaves the predicate authoritative with no holder identity behind it, i.e. a permanent silent no-op. Lower ivh_cs_preempt_src first.\n");
		return -EINVAL;
	}

	WRITE_ONCE(ivh_lock_holder_enabled, val);
	return 0;
}

/*
 * ivh_holder_bits: clamp into [IVH_HOLDER_MIN_BITS, IVH_HOLDER_MAX_BITS] and
 * WIPE THE TABLE on every change.
 *
 * Clamped rather than rejected, deliberately and unlike the knobs above: this
 * one exists to be swept in a loop from userspace, and a sweep script that
 * has to know the exact legal range in order not to abort halfway is a worse
 * interface than one that can walk 4..20 and be told what it actually got.
 * The bounds themselves are logged so the sweep's own record shows the
 * clamping rather than hiding it.
 *
 * The wipe is not optional.  Slots written under the previous index width sit
 * at addresses that the new width hashes differently, so without it every
 * one of them reads as a tag mismatch and the first measurement window after
 * each sweep step would report the PREVIOUS geometry's residue as this
 * geometry's collision rate -- which would make the resulting curve not
 * merely noisy but systematically wrong in the direction that matters.
 */
static int ivh_pv_proc_holder_bits(const struct ctl_table *table, int write,
				   void *buffer, size_t *lenp, loff_t *ppos)
{
	unsigned long val = READ_ONCE(ivh_holder_bits);
	unsigned long clamped;
	struct ctl_table tmp = *table;
	int ret;

	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	clamped = clamp(val, (unsigned long)IVH_HOLDER_MIN_BITS,
			(unsigned long)IVH_HOLDER_MAX_BITS);
	if (clamped != val)
		pr_info("IVH: ivh_holder_bits=%lu clamped to %lu (legal range %d..%d)\n",
			val, clamped, IVH_HOLDER_MIN_BITS, IVH_HOLDER_MAX_BITS);

	if (clamped != READ_ONCE(ivh_holder_bits)) {
		WRITE_ONCE(ivh_holder_bits, clamped);
		ivh_holder_table_clear();
		pr_info("IVH: ivh_holder_bits now %lu (%lu effective slots), side table wiped\n",
			clamped, 1UL << clamped);
	}

	return 0;
}

static const struct ctl_table ivh_pv_sysctls[] = {
	{
		.procname	= "ivh_pv_wait_mechanism",
		.data		= &ivh_pv_wait_mechanism,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= ivh_pv_proc_wait_mechanism,
	},
	{
		.procname	= "ivh_pv_wait_trace",
		.data		= &ivh_pv_wait_trace,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_pv_kick_pure_ipi",
		.data		= &ivh_pv_kick_pure_ipi,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= ivh_pv_proc_kick_pure_ipi,
	},
	{
		.procname	= "ivh_pv_preempt_src",
		.data		= &ivh_pv_preempt_src,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= ivh_pv_proc_preempt_src,
	},
	{
		.procname	= "ivh_pv_beat_threshold",
		.data		= &ivh_pv_beat_threshold,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_pv_beat_publish_mask",
		.data		= &ivh_pv_beat_publish_mask,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= ivh_pv_proc_beat_publish_mask,
	},
	/*
	 * Build 1 (build plan sec 3.6), locking-side knobs.  Every one of them
	 * defaults to a value that changes nothing: ivh_cs_preempt_src=0 means
	 * the CS stamp is never written and never read, and
	 * ivh_lock_holder_enabled=0 means the qspinlock ownership path pays one
	 * predicted branch and no store.  ivh_cs_predicate_form and
	 * ivh_holder_bits have non-zero defaults, but neither is reachable
	 * while the two gates above are 0, so they are inert as well.
	 */
	{
		.procname	= "ivh_cs_preempt_src",
		.data		= &ivh_cs_preempt_src,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= ivh_pv_proc_cs_preempt_src,
	},
	{
		.procname	= "ivh_cs_predicate_form",
		.data		= &ivh_cs_predicate_form,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= ivh_pv_proc_cs_predicate_form,
	},
	{
		.procname	= "ivh_cs_beat_threshold",
		.data		= &ivh_cs_beat_threshold,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_lock_holder_enabled",
		.data		= &ivh_lock_holder_enabled,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= ivh_pv_proc_lock_holder_enabled,
	},
	{
		.procname	= "ivh_holder_bits",
		.data		= &ivh_holder_bits,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= ivh_pv_proc_holder_bits,
	},
};

static int __init ivh_pv_sysctl_init(void)
{
	register_sysctl_init("kernel", ivh_pv_sysctls);
	return 0;
}
late_initcall(ivh_pv_sysctl_init);
#endif /* CONFIG_SYSCTL */

/*
 * Trace point for the mechanism==2 wait/kick path -- see the long comment on
 * ivh_pv_wait_trace above for why this is plain printk(KERN_EMERG ...) and
 * not trace_printk()/ftrace/pstore. No-op (one READ_ONCE + branch) unless
 * `sysctl kernel.ivh_pv_wait_trace` is nonzero. raw_smp_processor_id() (not
 * smp_processor_id()) because this fires from contexts -- IRQs disabled,
 * inside the halt sequence itself -- where the preemption-enabled debug
 * check smp_processor_id() performs would be a spurious warning, not a bug.
 */
#define ivh_pv_trace(fmt, ...)						\
	do {								\
		if (unlikely(READ_ONCE(ivh_pv_wait_trace)))		\
			printk(KERN_EMERG "ivh_trace: cpu=%d mech=%lu irqs_disabled=%d " \
			       fmt "\n", raw_smp_processor_id(),	\
			       READ_ONCE(ivh_pv_wait_mechanism),	\
			       irqs_disabled(), ##__VA_ARGS__);	\
	} while (0)

/*
 * Republish this vCPU's TSC heartbeat the instant it comes back from an
 * EXPLICIT halt (mechanism 0's PV_UNHALT halt()/safe_halt(), mechanism 2's
 * safe_halt()).
 *
 * Why this site is needed on top of the tick publish: a vCPU parked in HLT is
 * not executing, so it publishes nothing, and the host may leave it blocked
 * for far longer than one tick.  On wake it IS running again -- but its stamp
 * is still whatever it was before the halt, and stays that way until the next
 * account_process_tick(), i.e. for up to a full 1 ms at CONFIG_HZ=1000.  For
 * that entire window every waiter queued behind it reads it as host-preempted
 * when it is demonstrably not.  That is a false positive manufactured by our
 * own mechanism, on precisely the CPUs the mechanism just woke, and it would
 * be indistinguishable in the histogram from real host preemption.  One store
 * here closes the window to zero.
 *
 * Deliberately UNCONDITIONAL (not gated on ivh_pv_preempt_src): it is one
 * rdtsc plus one store on a path that has just taken a HLT vmexit and a host
 * reschedule, so the cost is unmeasurable there, and gating it would leave the
 * heartbeat's meaning dependent on a knob that can be flipped live underneath
 * an already-halted waiter.
 *
 * Deliberately placed AFTER the halt returns and BEFORE any ivh_pv_trace():
 * a printk on the wake path can take microseconds, and stamping after it would
 * bake that into the heartbeat.  Never publish BEFORE halting -- that would
 * make a genuinely halted vCPU read "running" for a full threshold window,
 * which is the exact false negative this whole signal exists to avoid.
 */
static __always_inline void ivh_beat_halt_exit(void)
{
	ivh_tsc_beat_publish();
	this_cpu_inc(ivh_beat_publishes);
}

static __always_inline void ivh_pv_backoff(void)
{
	/*
	 * TPAUSE (WAITPKG) parks this logical CPU in a light C0.2 state until
	 * the TSC deadline or an interrupt, WITHOUT a HLT vmexit — so it never
	 * descheduls the vCPU or opens a steal window.  If the guest was not
	 * offered WAITPKG, fall back to PAUSE (cpu_relax), which is always
	 * valid and is exactly what the TAS/native spin paths already use.
	 */
	if (cpu_feature_enabled(X86_FEATURE_WAITPKG)) {
		u64 until = rdtsc() + IVH_PV_TPAUSE_CYCLES;

		__tpause(TPAUSE_C02_STATE, upper_32_bits(until),
			 lower_32_bits(until));
	} else {
		cpu_relax();
	}
}

/*
 * Kick a cpu by its apicid — the stock, host-cooperative wake, used only
 * by the sysctl==0 fallback path below when the host actually advertises
 * KVM_FEATURE_PV_UNHALT.  Verbatim behavior of the pre-IVH kvm_kick_cpu().
 */
static void ivh_pv_hypercall_kick(int cpu)
{
	u32 apicid = per_cpu(x86_cpu_to_apicid, cpu);

	kvm_hypercall2(KVM_HC_KICK_CPU, 0, apicid);
}

static void ivh_pv_wait(u8 *ptr, u8 val)
{
	u64 deadline;

	if (in_nmi())
		return;

	this_cpu_inc(ivh_pv_wait_calls);

	/*
	 * sysctl OFF (default, ivh_pv_wait_mechanism == 0): behave as close to
	 * "no IVH-specific mechanism" as this call site allows.
	 *   - If the host advertises KVM_FEATURE_PV_UNHALT, this is byte-for-
	 *     byte the pre-IVH kvm_wait() body: a real halt/safe_halt, woken by
	 *     ivh_pv_kick()'s hypercall below.  Fully hypervisor-cooperative,
	 *     exactly upstream's own long-tested mechanism.
	 *   - If not, a plain cpu_relax() busy loop with no TPAUSE and no
	 *     steal-bit logic: the least surprising degenerate case, and the
	 *     same "just spin" character every other non-PV queued_spin_lock
	 *     waiter already relies on elsewhere in this file. Bounded by the
	 *     lock's own liveness guarantee, same as any other spin-wait.
	 */
	if (!READ_ONCE(ivh_pv_wait_mechanism)) {
		/*
		 * ivh_lock_halt_begin/end: this is a HLT taken OUTSIDE the idle
		 * loop, so tick_nohz's idle accumulators never see it and
		 * ivh_ref_accumulate()'s idle subtraction cannot account for it.
		 * Measure it here or it becomes phantom steal -- see the long
		 * comment on struct ivh_lock_halt in <asm/ivh_tsc_beat.h>.
		 * Mechanism 0 is instrumented alongside 1 and 2 on purpose: the
		 * defect is a property of halting in the lock path, not of any
		 * one mechanism, and an A/B that only corrected the mechanisms
		 * under test would build the conclusion into the measurement.
		 */
		if (kvm_para_has_feature(KVM_FEATURE_PV_UNHALT)) {
			if (irqs_disabled()) {
				if (READ_ONCE(*ptr) == val) {
					ivh_lock_halt_begin(false);
					halt();
					ivh_beat_halt_exit();
					ivh_lock_halt_end();
				}
			} else {
				local_irq_disable();
				if (READ_ONCE(*ptr) == val) {
					ivh_lock_halt_begin(false);
					safe_halt();
					ivh_beat_halt_exit();
					ivh_lock_halt_end();
				} else {
					local_irq_enable();
				}
			}
			return;
		}

		while (READ_ONCE(*ptr) == val)
			cpu_relax();
		return;
	}

	/*
	 * ivh_pv_wait_mechanism == 3: pure busy-spin.  Never halts, never naps,
	 * never yields the pCPU -- the runtime-selectable stand-in for the
	 * `nopvspin` boot parameter, which cannot be toggled live because
	 * pv_ops.lock.* and virt_spin_lock_key are decided exactly once in
	 * kvm_spinlock_init() below (deliberately -- see its comment).  This makes
	 * an A/B against "no adaptive spinning at all" possible without rebooting
	 * between runs.  It is a close approximation, not an identity: the PV
	 * slowpath's own bookkeeping (pn->state transitions, _Q_SLOW_VAL, pv_hash)
	 * still runs, because that lives in the caller, not here.  Keep the real
	 * `nopvspin` boot as the reference baseline for any claim that needs one.
	 *
	 * The loop below is character-for-character the mechanism==0 fallback a
	 * few lines above (its "host does not advertise PV_UNHALT" case): a plain
	 * cpu_relax() poll on the condition the caller passed, bounded by nothing
	 * but the lock's own liveness guarantee, exactly like every other non-PV
	 * queued_spin_lock waiter.  That is the point -- this branch invents no
	 * control flow, it re-uses an already-shipping, already-proven shape under
	 * a new sysctl value.
	 *
	 * Forward progress here needs no kick at all, which is what lets
	 * ivh_pv_kick() skip the IPI for this mechanism: pv_wait_node() waits on
	 * pn->state != VCPU_HALTED, which pv_kick_node()'s
	 * cmpxchg(HALTED->HASHED) publishes (with a full barrier) strictly before
	 * it would kick; pv_wait_head_or_lock() waits on lock->locked !=
	 * _Q_SLOW_VAL, which the unlock's smp_store_release() already cleared
	 * before __pv_queued_spin_unlock_slowpath() reaches pv_kick().  In both
	 * cases the store a spinner is watching for happens before the wake in
	 * program order, so the spinner observes it with no wake ever arriving.
	 *
	 * Deliberately NOT traced (ivh_pv_wait_trace): that instrumentation exists
	 * to capture the instruction stream immediately before a halt that might
	 * never return.  Nothing here can strand a CPU -- no halt, no TPAUSE, no
	 * IRQ manipulation, no state to lose -- so there is no post-mortem
	 * question for a trace line to answer, while this loop is as hot as the
	 * mechanism==2 path whose tracing already has to be left off by default.
	 */
	if (READ_ONCE(ivh_pv_wait_mechanism) == 3) {
		while (READ_ONCE(*ptr) == val)
			cpu_relax();
		return;
	}

	/*
	 * ivh_pv_wait_mechanism == 2: a real halt/safe_halt (genuine vCPU yield,
	 * a real HLT vmexit that the host observes and can reschedule the
	 * descheduled lock holder onto), woken by the real smp_send_reschedule()
	 * IPI in ivh_pv_kick()/pv_kick_node() -- NOT by KVM_HC_KICK_CPU, and
	 * therefore NOT gated on kvm_para_has_feature(KVM_FEATURE_PV_UNHALT).
	 *
	 * Why this is correct without PV_UNHALT (verified against x86/KVM
	 * semantics, not assumed):
	 *   - HLT in a guest always vmexits (host-side HLT-passthrough is only
	 *     enabled for dedicated pCPUs / KVM_HINTS_REALTIME, and
	 *     kvm_spinlock_init() below already routes that case to native
	 *     qspinlock so this path is never reached then). So the yield is
	 *     real and host-visible regardless of PV_UNHALT -- PV_UNHALT only
	 *     ever optimized the *wake* (a directed hypercall vs a generic IPI),
	 *     never whether the halt traps.
	 *   - A HLT-exited (host-blocked) vCPU is un-halted by ANY interrupt
	 *     delivered to its LAPIC. smp_send_reschedule() sends a genuine
	 *     RESCHEDULE_VECTOR APIC IPI (native_smp_send_reschedule ->
	 *     __apic_send_IPI), which KVM injects into the target and thereby
	 *     unblocks it. This is baseline interrupt-driven wake, not a
	 *     paravirt feature.
	 *
	 * The halt sequence below deliberately mirrors the mechanism==0
	 * PV_UNHALT halt sequence above byte-for-byte (same lost-wakeup guard,
	 * same irqs_disabled() branch structure) -- only the PV_UNHALT gate and
	 * the wake vehicle differ. Do not "simplify" one without the other.
	 *
	 * Lost-wakeup race: with IRQs disabled we re-check READ_ONCE(*ptr)==val
	 * and only then halt; safe_halt()'s sti;hlt is atomic (one-instruction
	 * sti interrupt shadow), so a kick landing between the re-check and the
	 * halt is delivered right after the hlt begins and un-halts us -- it
	 * cannot be lost.
	 *
	 * *** DO NOT halt() here when IRQs are ALREADY disabled. ***
	 *
	 * An earlier revision of this branch mirrored the mechanism==0 sequence
	 * byte-for-byte, including its bare halt() for the irqs_disabled() case,
	 * on the (WRONG) premise that "HLT resumes on any pending interrupt
	 * regardless of the IF flag".  It does not.  Both Intel SDM Vol.2 ("An
	 * *enabled* interrupt (including NMI and SMI), a debug exception, the
	 * BINIT# signal, the INIT# signal, or the RESET# signal will resume
	 * execution") and the AMD APM ("...or an *unmasked* external interrupt")
	 * are explicit: with RFLAGS.IF=0 a maskable interrupt is recognized and
	 * left pending in the IRR but does NOT un-halt the core.  Only
	 * NMI/SMI/INIT/RESET do.
	 *
	 * KVM faithfully reproduces that: a vCPU blocked on a HLT exit is only
	 * made runnable by kvm_arch_vcpu_runnable() (arch/x86/kvm/x86.c), whose
	 * maskable-interrupt term is gated on kvm_arch_interrupt_allowed() ->
	 * vmx_interrupt_blocked()/svm_interrupt_blocked(), i.e. on the *guest's*
	 * RFLAGS.IF.  With IF=0 the only unconditional wake left in that
	 * function is vcpu->arch.pv.pv_unhalted -- which is set by exactly one
	 * thing: the KVM_HC_KICK_CPU hypercall (KVM_FEATURE_PV_UNHALT).
	 *
	 * That is precisely the wake vehicle mechanism==2 drops in favour of
	 * smp_send_reschedule().  A RESCHEDULE_VECTOR IPI is maskable, so it can
	 * never un-halt an IF=0 HLT.  Neither can the LAPIC timer tick, so the
	 * "next tick is a backstop" reasoning fails for the same reason.  The
	 * result was an unrecoverable dead vCPU: it stops answering reschedule,
	 * TLB-shootdown and smp_call_function IPIs, stops taking the tick (so no
	 * soft-lockup/RCU-stall report ever prints), keeps its qspinlock queue
	 * position forever, and drags every CPU that waits on it down with it --
	 * a silent whole-VM freeze with no oops.  This is exactly why upstream's
	 * kvm_wait(), which this sequence was copied from, is only ever wired up
	 * when KVM_FEATURE_PV_UNHALT is present (see kvm_spinlock_init()).
	 *
	 * So mechanism 2 halts ONLY on the path where it can guarantee IF=1 at
	 * the HLT (local_irq_disable + safe_halt's atomic sti;hlt).  When the
	 * caller already had IRQs off -- which is the common case, since every
	 * spin_lock_irqsave()/spin_lock_irq()/hardirq-context lock reaches
	 * queued_spin_lock_slowpath() -> pv_wait() that way -- we must not
	 * block at all, and instead fall through to the bounded non-halting
	 * poll below.  TPAUSE *does* wake on a masked pending interrupt (unlike
	 * HLT), and the deadline bounds it even with zero interrupts, so that
	 * path is always self-recovering.  Yielding the pCPU is a nice-to-have;
	 * an unwakeable vCPU is fatal.
	 *
	 * Backstop for the safe_halt() path if the IPI is ever lost or
	 * misdelivered: there IF really is 1, so the next timer tick un-halts
	 * us, whereupon the PV slowpath's own for(;;) re-checks node->locked /
	 * lock->locked and retries.
	 *
	 * "Scoped halt" (2026-07-23): this branch itself is unchanged, but for
	 * a role-C queued MCS waiter (kernel/locking/qspinlock_paravirt.h's
	 * pv_wait_node()) it is now reached on a NARROWER trigger than before
	 * -- only when pv_wait_early() actually fired for the predecessor
	 * (prev host-preempted or itself MCS-halted), never merely because the
	 * SPIN_THRESHOLD spin exhausted while prev looked healthy. See the
	 * wait_early-gated `continue` there. A role-A/B waiter (queue head /
	 * pending-bit, pv_wait_head_or_lock()) has no predecessor to scope
	 * against and reaches this same branch UNCONDITIONALLY after its own
	 * SPIN_THRESHOLD, exactly as mechanisms 0/1 do -- this asymmetry is
	 * intentional, not an oversight: there is no vcpu_is_preempted()
	 * signal available for "the current lock holder" from that path, only
	 * for an MCS predecessor. See
	 * tools/bpf/docs/ivh_scoped_halt_ipi_mechanism2_plan_2026-07-23.md for
	 * the full reasoning, including the honest caveat that this means
	 * mechanism 2 never yields the pCPU under pure in-guest (non-preempted)
	 * contention for a role-C waiter -- it degrades to native qspinlock
	 * spin in that case, not to mechanism 0's unconditional halt.
	 */
	if (READ_ONCE(ivh_pv_wait_mechanism) == 2 && !irqs_disabled()) {
		local_irq_disable();
		if (READ_ONCE(*ptr) == val) {
			ivh_pv_trace("mech2 HALT enter (safe_halt, IF=1 at hlt)");
			/*
			 * THE site the 2026-07-27 root cause is about: a real
			 * HLT, taken from the qspinlock slowpath rather than
			 * from the idle loop.  REF_TSC stops; tick_nohz's idle
			 * accumulators do not move; ivh_ref_accumulate() books
			 * the difference as steal unless this pair measures it.
			 * ivh_lock_halt_flush() at the tick handles the (very
			 * common) case where the timer interrupt that un-halts
			 * us runs account_process_tick() before end() below.
			 */
			ivh_lock_halt_begin(false);
			safe_halt();		/* sti;hlt -- HLT taken with IF=1 */
			ivh_beat_halt_exit();
			ivh_lock_halt_end();
			ivh_pv_trace("mech2 HALT exit (woke)");
		} else {
			ivh_pv_trace("mech2 no-halt (condition cleared before halt)");
			local_irq_enable();
		}
		return;
	}
	/*
	 * mechanism==2 with IRQs already disabled by an outer context falls
	 * through to the bounded poll below on purpose: see the IF=0 HLT
	 * discussion above.  Never halt() here.
	 */
	if (unlikely(READ_ONCE(ivh_pv_wait_mechanism) == 2))
		ivh_pv_trace("mech2 FALLTHROUGH to bounded poll (irqs already disabled on entry)");

	/*
	 * sysctl ON: IVH's own non-hypervisor-cooperative substitute. Bounded,
	 * non-halting poll.  Return as soon as the condition clears, or
	 * (possibly "spuriously") when the window elapses so the PV slowpath
	 * loop re-checks lock/node state and retries.  Never blocks, never
	 * leaves TASK_RUNNING.
	 *
	 * Deadline is IVH_PV_ADAPTIVE_TSC (~1 ms), not IVH_PV_WAIT_TSC: the
	 * real IPI wake now wired into pv_kick_node() (kernel/locking/
	 * qspinlock_paravirt.h) and ivh_pv_kick() below is the expected common
	 * wake, so ivh_pv_backoff()'s __tpause naps are cut short by that IPI
	 * (an "external interrupt occurs" is one of TPAUSE's own exit
	 * conditions, see arch/x86/lib/delay.c's delay_halt_tpause() comment;
	 * this holds regardless of the waiter's local IRQ-enabled state) well
	 * before this loop's own deadline check would fire. The deadline
	 * itself is only the fallback for a lost/misdelivered IPI — see the
	 * IVH_PV_ADAPTIVE_TSC comment above for why the two are paired.
	 */
	/*
	 * Accounted as "poll", separately from HLT, because whether these cycles
	 * are lost to REF_TSC is an open question rather than a known: with
	 * WAITPKG present (it is, here) ivh_pv_backoff() naps in
	 * __tpause(TPAUSE_C02_STATE), and C0.1/C0.2 park the logical processor,
	 * which is what CPU_CLK_UNHALTED.REF stops counting for.  Without
	 * WAITPKG it degrades to cpu_relax(), which is ordinary execution and
	 * costs REF_TSC nothing.  Keeping the two buckets separate is what makes
	 * `ivh_ref_halt_correct=1` vs `=2` a real experiment: if 2 is what
	 * restores agreement with host steal time in /proc/vcap_steal_compare,
	 * TPAUSE does stop the counter; if 1 already suffices, it does not.
	 *
	 * The whole loop body is charged, not just the __tpause: at 512 nap
	 * cycles against ~30 for the rdtsc/compare around it that over-charges
	 * by a few percent, which biases inferred steal DOWN -- the same
	 * direction every other clamp in ivh_ref_accumulate() deliberately
	 * fails, and the safe one for a signal that gates migrations.
	 */
	ivh_lock_halt_begin(true);
	deadline = rdtsc() + IVH_PV_ADAPTIVE_TSC;
	do {
		if (READ_ONCE(*ptr) != val)
			break;
		ivh_pv_backoff();
	} while ((s64)(rdtsc() - deadline) < 0);
	ivh_lock_halt_end();
}

static void ivh_pv_kick(int cpu)
{
	/*
	 * mechanism==0: only the host-PV_UNHALT-supported fallback path above
	 * ever actually halts, so only it needs a real wake; mirror its
	 * condition exactly so wait/kick stay consistent with each other even
	 * if the sysctl changes between one thread's wait and another's kick.
	 * If the host doesn't offer PV_UNHALT, nobody is truly halted (the
	 * cpu_relax() busy loop self-corrects), so this is a deliberate no-op.
	 */
	if (!READ_ONCE(ivh_pv_wait_mechanism)) {
		if (kvm_para_has_feature(KVM_FEATURE_PV_UNHALT))
			ivh_pv_hypercall_kick(cpu);
		return;
	}

	/*
	 * mechanism==3 (pure busy-spin, see ivh_pv_wait()): nothing this mechanism
	 * parks is ever asleep, so there is nothing for a wake to do -- NO IPI is
	 * sent.  Its waiters make forward progress on the unlock/cmpxchg store
	 * alone; the "Forward progress here needs no kick at all" paragraph in
	 * ivh_pv_wait()'s mechanism==3 branch spells out both call sites.
	 *
	 * The KVM_HC_KICK_CPU hypercall IS still issued, and it is *not* for
	 * anything mechanism 3 parked.  The sysctl is live-toggleable, and the
	 * default is 0: a waiter that entered ivh_pv_wait() a moment before the
	 * write of 3 landed is sitting in the mechanism==0 branch's bare halt()
	 * with RFLAGS.IF=0, where -- per the Intel SDM / AMD APM analysis in
	 * ivh_pv_wait()'s mechanism==2 comment -- ONLY pv_unhalted, i.e. only this
	 * hypercall, can ever wake it (a maskable RESCHEDULE_VECTOR IPI provably
	 * cannot, so "send an IPI instead" is not a substitute).  This host does
	 * advertise KVM_FEATURE_PV_UNHALT, so that halt is reachable in practice:
	 * making this kick a bare no-op would turn
	 * `sysctl kernel.ivh_pv_wait_mechanism=3` on a contended system into the
	 * exact stranded-waiter whole-VM freeze the nonzero-mechanism path below
	 * documents at length.  Identical belt-and-braces, identical reason.
	 *
	 * ivh_pv_kick_pure_ipi=1 suppresses even that, giving a genuinely
	 * hypercall-free (and, here, entirely silent) kick for measurement -- with
	 * the same caveat it already carries for mechanism 2: it removes the only
	 * wake a straggler parked under an earlier mechanism 0 could still get.
	 * Set it only once the system has been running at a nonzero mechanism for
	 * long enough that no such straggler can remain.  The proc handlers' guard
	 * needs no change for any of this: it rejects {mechanism==0, pure_ipi==1},
	 * and 3 is not 0.
	 */
	if (READ_ONCE(ivh_pv_wait_mechanism) == 3) {
		if (kvm_para_has_feature(KVM_FEATURE_PV_UNHALT) &&
		    !READ_ONCE(ivh_pv_kick_pure_ipi))
			ivh_pv_hypercall_kick(cpu);
		return;
	}

	/*
	 * mechanism==1 or ==2 (any nonzero mechanism except 3 falls here): this is
	 * __pv_queued_spin_unlock_slowpath()'s pv_kick(node->cpu) for the
	 * queue-head waiter (role B, waiting in
	 * ivh_pv_wait(&lock->locked, _Q_SLOW_VAL) inside
	 * pv_wait_head_or_lock()). By the time pv_kick() runs there,
	 * lock->locked has already been smp_store_release()'d to 0 (the
	 * unlock happened first), so the condition ivh_pv_wait() is polling
	 * for has already changed before this IPI is sent — the ordering the
	 * waiter's own READ_ONCE(*ptr) re-check depends on is already
	 * satisfied by the unlock path itself, this call only needs to wake
	 * the waiter (cut mechanism 1's current TPAUSE nap short, or un-halt
	 * mechanism 2's real HLT). `cpu` is node->cpu recovered from
	 * pv_unhash(lock); upstream's own comment right above this call site
	 * (in __pv_queued_spin_unlock_slowpath()) already establishes that
	 * read is valid this late and that kicking an active/non-halted vCPU
	 * is harmless — we rely on that same guarantee.
	 *
	 * The SAME smp_send_reschedule() serves both mechanisms because the
	 * two want a genuinely identical kick: a real, targeted APIC IPI to
	 * `cpu`. For mechanism 2 that IPI is also what un-halts a real HLT
	 * (see ivh_pv_wait()'s mechanism==2 branch), which is the whole point
	 * of mechanism 2 being able to drop the KVM_HC_KICK_CPU hypercall.
	 *
	 * A lost or spurious IPI is not a hang: pv_wait_head_or_lock()'s
	 * outer for(;;) always re-attempts the trylock/hash dance on return
	 * from pv_wait(). Mechanism 1's ivh_pv_wait() always returns by
	 * IVH_PV_ADAPTIVE_TSC; mechanism 2's IF=1 safe_halt() un-halts on the
	 * next timer tick at the latest — either way, no wake is required for
	 * progress.
	 *
	 * The hypercall kick is issued IN ADDITION to the IPI (never instead of
	 * it) because the sysctl is live-toggleable: a waiter that went to sleep
	 * while the mechanism was 0 is parked in a bare halt() with RFLAGS.IF=0
	 * (upstream's sequence, above), and *only* KVM_HC_KICK_CPU's pv_unhalted
	 * can wake that — a maskable RESCHEDULE_VECTOR IPI provably cannot (see
	 * ivh_pv_wait()'s mechanism==2 comment). Without this, merely writing a
	 * new value to ivh_pv_wait_mechanism on a busy system strands every
	 * currently-halted waiter and freezes the VM. Kicking an already-running
	 * vCPU is harmless (upstream relies on the same property), and this is
	 * the PV slow path, not the fast path.
	 *
	 * ivh_pv_kick_pure_ipi=1 opts out of that belt-and-braces hypercall to
	 * get a measurably 100%-IPI kick (see the sysctl's comment). It is only
	 * reachable when the mechanism is already nonzero: the proc handlers
	 * above refuse the {mechanism==0, pure_ipi==1} pair in either write
	 * order and publish each new value only after validating the pair, so
	 * the "waiter parked in mechanism 0's IF=0 halt() gets stranded" case
	 * this hypercall exists to prevent cannot arise while it is suppressed.
	 */
	if (kvm_para_has_feature(KVM_FEATURE_PV_UNHALT) &&
	    !READ_ONCE(ivh_pv_kick_pure_ipi)) {
		ivh_pv_trace("KICK target_cpu=%d via hypercall (PV_UNHALT)", cpu);
		ivh_pv_hypercall_kick(cpu);
	} else if (READ_ONCE(ivh_pv_kick_pure_ipi)) {
		ivh_pv_trace("KICK target_cpu=%d hypercall SKIPPED (ivh_pv_kick_pure_ipi=1)", cpu);
	}

	ivh_pv_trace("KICK target_cpu=%d via smp_send_reschedule (RESCHEDULE_VECTOR IPI)", cpu);
	smp_send_reschedule(cpu);
}

/*
 * Setup pv_lock_ops to exploit KVM_FEATURE_PV_UNHALT if present.
 */
void __init kvm_spinlock_init(void)
{
	/*
	 * IVH: unlike stock KVM we deliberately do NOT bail when the host lacks
	 * KVM_FEATURE_PV_UNHALT.  ivh_pv_wait()/ivh_pv_kick() are always safe to
	 * register here regardless of that feature bit: internally they check
	 * it themselves (see their comments) to pick between the stock
	 * host-cooperative halt/hypercall-kick behavior and IVH's own
	 * non-cooperative TPAUSE substitute, live-toggleable at runtime via the
	 * ivh_pv_wait_mechanism sysctl. What must NOT be host-feature- or
	 * sysctl-dependent is *this* registration itself: pv_ops.lock.* and
	 * virt_spin_lock_key are set up exactly once here, at boot, before any
	 * concurrent lock activity exists on this CPU. See
	 * arch/x86/include/asm/qspinlock.h's comment on ivh_pv_wait_mechanism
	 * for why that boundary — not this one — is where the live toggle
	 * belongs: queued_spin_lock_slowpath() re-checks virt_spin_lock_key on
	 * every single contended acquisition of every lock in the kernel, so
	 * flipping it under load could strand TAS-mode and MCS-queued waiters
	 * on the same lock at once, a real fairness/starvation hazard even
	 * though the qspinlock word format happens to make outright corruption
	 * unlikely. No sysctl anywhere in this feature touches it.
	 *
	 * When dedicated pCPUs are advertised there is no lock-holder
	 * preemption to mitigate, so plain native fair qspinlock is best: skip
	 * PV-op registration (leaving pv_ops.lock.* at the native defaults) and
	 * only disable the TAS fallback.  Same for a single CPU or an explicit
	 * "nopvspin".
	 */
	if (kvm_para_has_hint(KVM_HINTS_REALTIME)) {
		pr_info("IVH: dedicated pCPUs (KVM_HINTS_REALTIME), using native qspinlock\n");
		goto out;
	}

	if (num_possible_cpus() == 1) {
		pr_info("IVH: single CPU, using native qspinlock\n");
		goto out;
	}

	if (nopvspin) {
		pr_info("IVH: PV spinlocks disabled by \"nopvspin\", using native qspinlock\n");
		goto out;
	}

	pr_info("IVH: PV spinlock substitute registered (TAS virt_spin_lock disabled, MCS queueing restored); ivh_pv_wait_mechanism=%lu selects host-cooperative halt/kick vs IVH's own TPAUSE substitute at runtime\n",
		ivh_pv_wait_mechanism);

	__pv_init_lock_hash();
	pv_ops.lock.queued_spin_lock_slowpath = __pv_queued_spin_lock_slowpath;
	pv_ops.lock.queued_spin_unlock =
		PV_CALLEE_SAVE(__pv_queued_spin_unlock);
	pv_ops.lock.wait = ivh_pv_wait;
	pv_ops.lock.kick = ivh_pv_kick;

	/*
	 * With PV ops registered (or in the native-qspinlock cases above),
	 * virt_spin_lock()'s TAS hijack must be off so real MCS queueing runs.
	 */
out:
	static_branch_disable(&virt_spin_lock_key);
}

#endif	/* CONFIG_PARAVIRT_SPINLOCKS */

#ifdef CONFIG_ARCH_CPUIDLE_HALTPOLL

static void kvm_disable_host_haltpoll(void *i)
{
	wrmsrq(MSR_KVM_POLL_CONTROL, 0);
}

static void kvm_enable_host_haltpoll(void *i)
{
	wrmsrq(MSR_KVM_POLL_CONTROL, 1);
}

void arch_haltpoll_enable(unsigned int cpu)
{
	if (!kvm_para_has_feature(KVM_FEATURE_POLL_CONTROL)) {
		pr_err_once("host does not support poll control\n");
		pr_err_once("host upgrade recommended\n");
		return;
	}

	/* Enable guest halt poll disables host halt poll */
	smp_call_function_single(cpu, kvm_disable_host_haltpoll, NULL, 1);
}
EXPORT_SYMBOL_GPL(arch_haltpoll_enable);

void arch_haltpoll_disable(unsigned int cpu)
{
	if (!kvm_para_has_feature(KVM_FEATURE_POLL_CONTROL))
		return;

	/* Disable guest halt poll enables host halt poll */
	smp_call_function_single(cpu, kvm_enable_host_haltpoll, NULL, 1);
}
EXPORT_SYMBOL_GPL(arch_haltpoll_disable);
#endif
