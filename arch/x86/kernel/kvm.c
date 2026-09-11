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
#include <linux/smp.h>
#include <trace/events/ipi.h>
#include <linux/slab.h>
#include <linux/kprobes.h>
#include <linux/nmi.h>
#include <linux/delay.h>
#include <linux/swait.h>
#include <linux/syscore_ops.h>
#include <linux/cc_platform.h>
#include <linux/efi.h>
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
 * IVH rebuild Step 4 (tools/bpf/docs/ivh_rebuild_plan.md sec 4): this
 * replaces vanilla's kvm_kick_cpu()/kvm_wait()/kvm_spinlock_init() with
 * IVH's own ivh_pv_kick()/ivh_pv_wait() substitute, mechanism=0 (stock-
 * mimicking) by default. Ported from production's kvm.c per sec 1.4 items
 * 1-5, EXCLUDING two mechanisms production fuses into this same file:
 *
 *   - The CS-preemption-stamp system (ivh_cs_beat, ivh_cs_preempt_src,
 *     ivh_cs_predicate_form, ivh_cs_head_check() in
 *     kernel/locking/qspinlock_paravirt.h) -- sec 1.7's artifact list:
 *     "fully wired, large, but default-OFF, never enabled in production,
 *     predicate has a measured hard ceiling of 78.57% sensitivity."
 *   - Hot Threads' ivh_this_cpu_steal_ns() and the lock-holder-identity
 *     ownership-transfer call sites (A6/A7/A8/R3/R4 in
 *     kernel/locking/qspinlock.c and qspinlock_paravirt.h) -- same sec 1.7
 *     entry bundles these with the CS-stamp predicate as one archived,
 *     do-not-port unit. Step 2 already ported the holder-table STORAGE
 *     (arch/x86/kernel/ivh_lock_holder.c) and the release-side clear call
 *     sites (arch/x86/include/asm/qspinlock.h's R2/R2b) inertly; the
 *     acquire-side stamp sites stay unported, so the table stays
 *     permanently empty -- exactly production's own runtime behavior,
 *     since ivh_lock_holder_enabled is never armed there either.
 *   - Part C (ivh_vact_capacity/ivh_vact_jump_threshold) -- sec 1.7:
 *     "attempted, measured regression, root-caused, not shipped."
 *
 * Project thesis: mitigate lock-holder preemption in a KVM guest WITHOUT
 * hypervisor cooperation. The stock kvm_wait() HLTs the waiting vCPU and
 * relies on the host waking it via KVM_HC_KICK_CPU (KVM_FEATURE_PV_UNHALT).
 * Mode VANILLA below reproduces that byte-for-byte (the safe default);
 * modes PURE_IPI/ADAPTIVE are IVH's own non-hypervisor-cooperative wake,
 * runtime-selectable via the ivh_adaptive_mode sysctl -- see
 * <asm/qspinlock.h>'s comment on that extern for the full mode table.
 */

DEFINE_PER_CPU(u64, ivh_pv_wait_calls);

/*
 * Runtime selector for ivh_pv_wait()/ivh_pv_kick()/pv_wait_early()'s
 * behavior. Declared extern (with the full three-mode comment) in
 * arch/x86/include/asm/qspinlock.h. Default 0: VANILLA.
 */
unsigned long ivh_adaptive_mode = 0UL;

/*
 * Forensic tracing for the non-vanilla wait/kick path. Default OFF (0).
 * Plain printk(KERN_EMERG ...), not trace_printk()/ftrace/pstore: this
 * project had exactly one confirmed hard freeze (2026-07-24, an IF=0 HLT
 * that no maskable IPI could ever un-halt -- see ivh_pv_wait()'s
 * comment for the full SDM/APM-sourced analysis), and once
 * every CPU is IF=0-halted nothing that depends on *something running
 * later* (a kthread, irq_work, a future reschedule) can ever produce
 * output. printk()'s ring-buffer append is synchronous and needs nothing
 * but the calling CPU, so it is the only instrumentation that survives
 * this exact failure mode -- and it already has, in the one real incident.
 * Left OFF by default: this path is hot, and tracing every call at
 * contention rates can itself perturb timing. Turn on only when actively
 * chasing a repeat of 2026-07-24.
 */
unsigned long ivh_pv_wait_trace = 0UL;

/*
 * IVH Idea 4 -- EXPERIMENTAL, deliberately not the shipped Stage 1 design.
 * Default 0: today's safe behavior (busy-spin, never halt, when IRQs are
 * already disabled at ivh_pv_wait() entry). When 1: enable IRQs for the
 * halt, then explicitly restore them to disabled before returning to the
 * caller -- see ivh_pv_wait()'s IF=0 branch.
 *
 * This exists ONLY to run a deliberate, targeted reproduction of the
 * hazard identified in the build-plan doc (§9.4/§9.4.1): every wake_up()
 * variant takes the generic wait-queue lock via spin_lock_irqsave()
 * specifically because it must be callable from hardirq context. If an
 * interrupt lands on this CPU during the open window and its handler
 * wants the SAME lock, that handler enqueues behind this CPU's own
 * already-in-flight MCS node -- a permanent deadlock, not a slowdown,
 * because this CPU can never finish the handler (which needs the lock)
 * to get back to its own earlier queue position (which the handler is
 * now blocking). NOT proven safe against this hazard. NOT for general use.
 * See cvm_setup/ hazard_a_test module for the deliberate reproduction.
 */
unsigned long ivh_pv_irqoff_halt = 0UL;
DEFINE_PER_CPU(u64, ivh_irqoff_halt_used);

/*
 * G-LOCK-21-spin: spinning-threshold study knobs. See <asm/qspinlock.h>
 * for the full comment. Defaults reproduce stock/current behavior exactly:
 * tier 1 on, spin budget equal to the compile-time SPIN_THRESHOLD.
 */
unsigned long ivh_pv_tier1_enable = 1UL;
unsigned long ivh_pv_spin_threshold = 1UL << 15;	/* == SPIN_THRESHOLD, <asm/spinlock.h> */

/*
 * G-LOCK-22-hybrid: cached copy of kvm_para_has_feature(KVM_FEATURE_PV_UNHALT).
 * That call is a live CPUID(0x40000001) -- on a TDX guest, CPUID in that
 * VMM-delegated leaf range is NOT handled in hardware: it takes a #VE, and
 * the #VE handler emulates it via a real TDVMCALL round-trip to the host
 * (arch/x86/coco/tdx/tdx.c's handle_cpuid()). That is ~10^4 cycles, not the
 * ~10-cycle native CPUID cost the old code implicitly assumed. The feature
 * bit is architecturally fixed for the life of a boot (the host does not
 * change what it advertises mid-flight), so there is no correctness reason
 * to ever re-read it live -- only the pre-rebuild code's inherited habit of
 * calling kvm_para_has_feature() at every halt/wake did that. Set ONCE in
 * kvm_spinlock_init(), read everywhere else. This directly un-penalizes
 * mode VANILLA, which was the only mode still calling this live (modes
 * PURE_IPI/ADAPTIVE never reached it) -- every prior VANILLA-vs-other-mode
 * timing in this project paid this cost on one side only.
 */
/*
 * Not static: pv_wait_early() (kernel/locking/qspinlock_paravirt.h) needs
 * ivh_pv_allowed(), declared alongside these in <asm/qspinlock.h> where both
 * this file and that one can reach it.
 */
bool ivh_pv_unhalt_avail __ro_after_init;
/* unsigned long shadow of the bool above, purely so the read-only
 * "ivh_pv_unhalt_avail" sysctl below can use proc_doulongvec_minmax() like
 * every other IVH knob instead of a bespoke bool proc_handler. */
static unsigned long ivh_pv_unhalt_avail_sysctl __ro_after_init;

/*
 * ivh_pv_allow: boot-parameter policy override for whether VANILLA and
 * ADAPTIVE may use the PV-native hypercall halt/wake path when the host
 * actually advertises KVM_FEATURE_PV_UNHALT (PURE_IPI is unconditionally
 * exempt -- see ivh_mode_uses_hypercall()). Deliberately a boot parameter,
 * NOT a live sysctl: ivh_mode_uses_hypercall()'s answer for a given mode
 * must not change while a CPU could be mid-halt on that vehicle, and
 * keeping this fixed for the life of the boot means the ONLY thing that
 * ever moves at runtime is ivh_adaptive_mode itself -- which is exactly
 * what ivh_pv_drain_hypercall_halts()'s transition drain below already
 * handles.
 *
 *   ivh_pv_allow=1 (default) - auto: VANILLA and ADAPTIVE both use the
 *       hypercall whenever ivh_pv_unhalt_avail is true. Under this default,
 *       VANILLA is byte-for-byte the true, unmodified "what stock PV
 *       actually does" baseline -- ivh_pv_allowed() reduces to exactly
 *       ivh_pv_unhalt_avail when this is 1.
 *   ivh_pv_allow=0 - force BOTH VANILLA and ADAPTIVE to behave as if
 *       PV_UNHALT were NOT advertised, even though it is. This is a
 *       simulated-environment override, not an ADAPTIVE-only policy: it has
 *       to move VANILLA too, or "PV not allowed" would have no reachable
 *       baseline arm to compare ADAPTIVE against (see ivh_mode_uses_hypercall()
 *       and its doc comment in <asm/qspinlock.h>). This is the way to reach
 *       and measure both modes' IPI-wake / busy-spin branches on a host
 *       that actually supports the hypercall, without needing host/QEMU-side
 *       control over what is advertised to this guest (unconfirmed whether
 *       that is even honorable for a TDX TD's configured CPUID -- this knob
 *       sidesteps needing to find out).
 *       Caveat: a LIVE ivh_adaptive_mode transition INTO VANILLA while this
 *       is 0 is handled by an explicit reschedule sweep in
 *       ivh_pv_proc_adaptive_mode() (not the hypercall drain, which does not
 *       apply here since neither side of that transition ever hypercalls)
 *       -- see its comment for why a waiter can otherwise be left parked in
 *       an IF=1 safe_halt() with nothing left to wake it until the next
 *       timer tick.
 */
unsigned long ivh_pv_allow __ro_after_init = 1UL;

static int __init ivh_pv_allow_setup(char *str)
{
	unsigned long val;

	if (!str || kstrtoul(str, 0, &val))
		return 0;
	ivh_pv_allow = val ? 1UL : 0UL;
	return 1;
}
__setup("ivh_pv_allow=", ivh_pv_allow_setup);

/*
 * ivh_pv_allowed() and ivh_mode_uses_hypercall() -- the single source of
 * truth for "does THIS mode use the hypercall wake vehicle right now" -- are
 * defined in <asm/qspinlock.h>, not here: pv_wait_early()
 * (kernel/locking/qspinlock_paravirt.h) needs them too, and that file cannot
 * see anything with internal linkage in this one.
 */

/*
 * ivh_pv_tas: boot parameter (NOT a live sysctl -- see kvm_spinlock_init()'s
 * comment for why this MUST be boot-time-only, same reasoning as
 * ivh_pv_allow but sharper: this one decides whether virt_spin_lock_key
 * itself is left enabled, and that key is re-checked on every single
 * contended qspinlock acquisition in the entire kernel -- a live flip could
 * strand a TAS-mode waiter and an MCS-queued waiter on the very same lock at
 * once, a correctness question this project has NOT verified safe (unlike
 * ivh_pv_allow's live-safe boot-fixed knobs, which only ever change which
 * WAIT/WAKE mechanism is used inside the already-permanently-installed PV
 * ops, never whether PV ops or MCS queueing exist at all).
 *
 * Default 0: unchanged behavior -- kvm_spinlock_init() registers IVH's PV
 * spinlock substitute and permanently disables virt_spin_lock_key, exactly
 * as before this existed.
 *
 * ivh_pv_tas=1: kvm_spinlock_init() returns immediately, before any of its
 * other checks or registration, WITHOUT ever registering pv_ops.lock.* and
 * WITHOUT touching virt_spin_lock_key -- which native_pv_lock_init()
 * (arch/x86/kernel/paravirt.c, called earlier in setup_arch() for any
 * X86_FEATURE_HYPERVISOR guest, which this always is) has already enabled
 * by the time kvm_spinlock_init() runs. The result is real, unmodified
 * upstream test-and-set spinlocks (virt_spin_lock() in
 * arch/x86/include/asm/qspinlock.h) for every single lock in the kernel,
 * for the whole boot: no MCS queueing, no pv_wait()/pv_kick() ever called,
 * ivh_adaptive_mode becomes completely inert (nothing consults it). This is
 * the ONLY way to reach a genuine TAS baseline in this tree -- see the
 * mode-4 build-plan discussion for why a live in-boot toggle was rejected.
 *
 * Mutually exclusive with ivh_pv_allow in effect (not enforced, just moot):
 * if this is 1, kvm_spinlock_init() returns before ivh_pv_allow is ever
 * consulted by anything.
 */
unsigned long ivh_pv_tas __ro_after_init = 0UL;

/*
 * MUST be early_param(), NOT __setup(): kvm_spinlock_init() (which reads
 * this) runs from smp_prepare_boot_cpu() inside setup_arch(), at
 * init/main.c:928 in start_kernel() -- but a plain __setup() handler is
 * non-early and isn't invoked until parse_args(..., unknown_bootoption)
 * at init/main.c:935-938, strictly AFTER kvm_spinlock_init() already ran.
 * With __setup(), ivh_pv_tas=1 on the cmdline would silently have NO
 * effect on kvm_spinlock_init()'s decision (it would still register PV
 * ops and disable virt_spin_lock_key) while STILL updating the variable
 * in time for the (much later) late_initcall that registers the
 * ivh_pv_tas sysctl mirror -- i.e. the sysctl would read back 1 while the
 * kernel was actually running mode-VANILLA/ADAPTIVE PV spinlocks the
 * whole boot. Found by round-4 review; caught before any data was
 * collected under it. early_param() (see upstream's own "nopvspin",
 * kernel/locking/qspinlock.c, for precisely this same reason) runs from
 * parse_early_param() inside setup_arch(), before smp_prepare_boot_cpu().
 *
 * Return convention also differs from __setup(): early_param() treats a
 * NONZERO return as "malformed option" (do_early_param() in init/main.c
 * warns on it), the opposite of __setup()'s "1 == handled". Compare
 * parse_nopvspin(), which returns 0 on success.
 */
static int __init ivh_pv_tas_setup(char *str)
{
	unsigned long val;

	if (!str || kstrtoul(str, 0, &val))
		return -EINVAL;
	ivh_pv_tas = val ? 1UL : 0UL;
	return 0;
}
early_param("ivh_pv_tas", ivh_pv_tas_setup);

/*
 * G-LOCK-22-hybrid: default-OFF sysctl gating whether pv_wait_early()'s tier
 * 1/tier 2 early-bail may fire for a mode-ADAPTIVE waiter that has no
 * productive place to bail TO -- i.e. !ivh_pv_allowed() (no hypercall) and
 * irqs_disabled() at the check (so the eventual ivh_pv_wait() call is just
 * going to busy-spin regardless of when it's reached). Bailing early there
 * does not change the FINAL wait behavior (still busy-spin), only whether
 * pv_kick_node() takes the _Q_SLOW_VAL + hash-table path first -- which
 * forces the eventual unlocker onto the slow unlock path and a real,
 * unneeded IPI on a third CPU's critical path. Left default 0 because the
 * sign of this trade is NOT proven: the closely analogous
 * ivh_pv_spin_threshold experiment (bail later instead of earlier) measured
 * ~9% SLOWER. A/B this in isolation before trusting either direction.
 */
unsigned long ivh_adaptive_irqoff_bail_gate = 0UL;

/*
 * G-LOCK-23: mode-agnostic contended-acquisition wait-time accounting,
 * ported from the earlier migration-engine era's ivh_obs_wait_begin()/
 * ivh_obs_wait_end() (kernel/locking/qspinlock.c, commit 194f859759c7,
 * "WIP: Build 1 TSC redesign in progress") with one deliberate change:
 *
 *   Gated on this GLOBAL sysctl, not a per-task current->ivh_observe flag
 *   -- that flag and its PR_SET_IVH_ELIGIBLE plumbing belong to the
 *   migration-engine subsystem this rebuild never carried over (see
 *   ivh_exec.c's /proc/ivh_debug dependency, confirmed absent from this
 *   tree). This project's whole-system, sysctl-selected A/B methodology
 *   (spin_mode 1/2/3/4) has no notion of "the one observed task" to begin
 *   with, so a global gate is the right scope here, not a simplification
 *   of the original.
 *
 * Everything else matches the original on purpose, restored after an
 * initial port dropped it and round-review caught the omission: the
 * in_interrupt() exclusion (a nested hardirq/softirq's own contention is
 * not this waiter's, and counting it would sum overlapping, not disjoint,
 * intervals), and the exact call sites (kernel/locking/qspinlock.c's
 * queued_spin_lock_slowpath()): right after virt_spin_lock()'s
 * TAS-acquired return, right before the pending-bit-acquired return, and
 * once at the release: label (which the original's own comment notes
 * already covers every "goto release" site plus the natural
 * contended-MCS fallthrough -- no separate call needed at each goto).
 *
 * This one instrumentation point is mode-agnostic by construction: the
 * function is compiled twice (native_queued_spin_lock_slowpath() and, via
 * the _GEN_PV_LOCK_SLOWPATH self-include below, __pv_queued_spin_lock_slowpath())
 * so the SAME edit measures raw TAS's virt_spin_lock() retry duration, mode
 * VANILLA/PURE_IPI/ADAPTIVE's whole MCS-queue-plus-halt wait duration, all
 * with one counter pair, directly comparable across every mode this project
 * tests -- exactly the point of building it this way rather than
 * instrumenting each mode's wait path separately.
 *
 * sched_clock() (nanosecond-denominated), not rdtsc(): matches the
 * original's choice, needs no manual cycle->ns conversion, and is already
 * the standard safe-to-call-from-any-context kernel time source.
 *
 * Default 0: zero added cost (one READ_ONCE, no clock read) when not
 * actively measuring, same posture as every other optional IVH knob here.
 */
unsigned long ivh_slowpath_wait_measure = 0UL;
DEFINE_PER_CPU(u64, ivh_slowpath_wait_ns);
DEFINE_PER_CPU(u64, ivh_slowpath_wait_events);

#define ivh_pv_trace(fmt, ...)						\
	do {								\
		if (unlikely(READ_ONCE(ivh_pv_wait_trace)))		\
			printk(KERN_EMERG "ivh_trace: cpu=%d mode=%lu irqs_disabled=%d " \
			       fmt "\n", raw_smp_processor_id(),	\
			       READ_ONCE(ivh_adaptive_mode),		\
			       irqs_disabled(), ##__VA_ARGS__);	\
	} while (0)

/*
 * IVH per-CPU TSC heartbeat -- storage, knobs and validation counters.
 * Declared (with the full design comment) in <asm/ivh_tsc_beat.h>.
 */
DEFINE_PER_CPU_ALIGNED(struct ivh_tsc_beat, ivh_tsc_beat);
EXPORT_PER_CPU_SYMBOL_GPL(ivh_tsc_beat);

unsigned long ivh_pv_preempt_src = 0UL;		/* 0 = KVM bit (default) */
unsigned long ivh_pv_tier1_confirm = 0UL;		/* 0 = upstream tier-1, bit-identical */
/*
 * 3,300,000 cycles = 1.5 ms at 2200 MHz -- is_cpu_preempted()'s existing
 * 1,500,000 ns threshold (kernel/sched/cputime.c) expressed in cycles, so
 * the shadow-comparator mode (src==1) is a controlled reproduction of the
 * signal this tree already has. Recomputed from the live tsc_khz at
 * late_initcall so the knob survives a different host.
 */
unsigned long ivh_pv_beat_threshold = 3300000UL;
#define IVH_BEAT_THRESHOLD_US	1500ULL
unsigned long ivh_pv_beat_publish_mask = 0xfffUL;

DEFINE_PER_CPU(u64, ivh_beat_agree_true);
DEFINE_PER_CPU(u64, ivh_beat_agree_false);
DEFINE_PER_CPU(u64, ivh_beat_false_pos);
DEFINE_PER_CPU(u64, ivh_beat_false_neg);
DEFINE_PER_CPU(u64, ivh_beat_publishes);
/*
 * IVH rebuild diagnostic, 2026-08-30: tier-1 (prev->state != VCPU_RUNNING,
 * stock upstream's own check) fire count, mode-independent -- this branch
 * in pv_wait_early() is reached and evaluated in every ivh_adaptive_mode
 * value, including VANILLA, unlike the tier-2 counters above which only
 * ever increment once is_wait_preempted() is reached (mode == ADAPTIVE
 * only). Exists to let the tier-1-vs-tier-2 resolution ratio be measured
 * directly, instead of inferred from tier-2's counts alone.
 */
DEFINE_PER_CPU(u64, ivh_beat_tier1_fired);
DEFINE_PER_CPU(u64, ivh_halt_from_node);
DEFINE_PER_CPU(u64, ivh_halt_from_head);
DEFINE_PER_CPU(u64, ivh_beat_tier2_checked);
DEFINE_PER_CPU(u64, ivh_beat_tier2_fired);
DEFINE_PER_CPU(u64, ivh_node_spin_iters_sum);
DEFINE_PER_CPU(u64, ivh_node_spin_attempts);
DEFINE_PER_CPU(u64, ivh_head_spin_iters_sum);
DEFINE_PER_CPU(u64, ivh_head_spin_attempts);
DEFINE_PER_CPU(u64, ivh_node_spin_success_iters_sum);
DEFINE_PER_CPU(u64, ivh_node_spin_success_attempts);
DEFINE_PER_CPU(s64, ivh_beat_min_age) = S64_MAX;
DEFINE_PER_CPU(u64, ivh_beat_age_hist_running[IVH_BEAT_AGE_HIST_BUCKETS]);
DEFINE_PER_CPU(u64, ivh_beat_age_hist_preempted[IVH_BEAT_AGE_HIST_BUCKETS]);
DEFINE_PER_CPU(u64, ivh_beat_age_hist_raw[IVH_BEAT_AGE_HIST_BUCKETS]);
DEFINE_PER_CPU(u64, ivh_head_arm);
DEFINE_PER_CPU(u64, ivh_head_yield_try_tier1);
DEFINE_PER_CPU(u64, ivh_head_yield_ok_tier1);
DEFINE_PER_CPU(u64, ivh_head_yield_try_tier2);
DEFINE_PER_CPU(u64, ivh_head_yield_ok_tier2);
DEFINE_PER_CPU(u64, ivh_head_woke_yielded);
DEFINE_PER_CPU(u64, ivh_head_woke_moot);
DEFINE_PER_CPU(u64, ivh_head_spin_enter);
DEFINE_PER_CPU(u64, ivh_head_yield_try_tier2_spinning);
DEFINE_PER_CPU(u64, ivh_head_yield_ok_tier2_spinning);
DEFINE_PER_CPU(u64, ivh_head_spinning_prearm);
DEFINE_PER_CPU(u64, ivh_wake_hypercall);
DEFINE_PER_CPU(u64, ivh_wake_ipi);
/* Pairs with ivh_wait_vanilla_nopv_spin -- mode VANILLA's !ivh_pv_allowed()
 * wake site, which correctly sends nothing (nobody halted). Counted so the
 * "did we ever accidentally send an IPI here" question has a direct answer
 * instead of relying on ivh_wake_ipi staying zero for an unrelated reason. */
DEFINE_PER_CPU(u64, ivh_wake_vanilla_nopv_noop);
DEFINE_PER_CPU(u64, ivh_wait_irqoff_nohalt);
DEFINE_PER_CPU(u32, ivh_pv_halt_inflight);
/*
 * G-LOCK-22-hybrid partition counters: every ivh_pv_wait_calls MUST land in
 * exactly one of {ivh_wait_pv_halt_irqoff, ivh_wait_pv_halt_irqon,
 * ivh_wait_ipi_halt_irqon, ivh_wait_irqoff_nohalt, ivh_wait_vanilla_nopv_spin}.
 * That exhaustive-partition check is the single thing that proves no
 * sub-population is silently uncounted -- the exact failure mode that
 * produced weeks of null tier-2 A/B results earlier in this project.
 */
DEFINE_PER_CPU(u64, ivh_wait_pv_halt_irqoff);
DEFINE_PER_CPU(u64, ivh_wait_pv_halt_irqon);
DEFINE_PER_CPU(u64, ivh_wait_ipi_halt_irqon);
DEFINE_PER_CPU(u64, ivh_earlybail_suppressed);
/*
 * Fifth partition member: mode VANILLA with !ivh_pv_allowed(). Only
 * reachable with ivh_pv_allow=0 on a host that DOES advertise PV_UNHALT
 * (on a genuinely PV_UNHALT-less host this is also where every waiter
 * lands, same as it always was) -- kept as its own counter rather than
 * folded into ivh_wait_irqoff_nohalt because it's a different mechanism
 * (VANILLA never even checks irqs_disabled() here; it busy-spins either
 * way, matching upstream) and because ivh_wake()'s matching no-op needs a
 * name too (ivh_wake_vanilla_nopv_noop, below) to keep wake-side and
 * wait-side counters legible as pairs.
 */
DEFINE_PER_CPU(u64, ivh_wait_vanilla_nopv_spin);

/* G-LOCK-25 scoping: see <asm/ivh_tsc_beat.h> for what these measure. */
DEFINE_PER_CPU(u64, ivh_node_halt_cycles[PV_BAIL_COUNT]);
DEFINE_PER_CPU(u64, ivh_node_halt_events[PV_BAIL_COUNT]);
DEFINE_PER_CPU(u64, ivh_node_halt_hist[PV_BAIL_COUNT][IVH_BEAT_AGE_HIST_BUCKETS]);
DEFINE_PER_CPU(u64, ivh_tier1_confirm_checked);
DEFINE_PER_CPU(u64, ivh_tier1_confirm_agreed);
DEFINE_PER_CPU(u64, ivh_tier1_confirm_disagreed);
DEFINE_PER_CPU(u64, ivh_tier1_suppressed);

/*
 * IVH Idea 4 Stage 0: attribution for the ivh_wait_irqoff_nohalt population.
 * Small fixed per-CPU table, not a real hash table -- the build-plan doc's
 * expectation is a handful of distinct call sites, not thousands, so a
 * linear scan is fine and keeps this easy to audit. Read-only consumer,
 * zero behavior change: this does not alter which waiters halt vs spin, it
 * only records *why* the spin population looks the way it does, so the
 * Idea 4 correctness audit has a concrete, short list of real call sites
 * instead of "audit every spin_lock_irqsave() in the kernel."
 *
 * Race-free by construction: only ever touched from ivh_pv_wait()'s IF=0
 * branch, which by definition runs with this CPU's interrupts already off,
 * on this same CPU -- no other context can preempt in and race the update.
 */
#define IVH_IRQOFF_ATTR_SLOTS 16
struct ivh_irqoff_attr_slot {
	unsigned long ret_ip;
	u64 count;
};
DEFINE_PER_CPU(struct ivh_irqoff_attr_slot, ivh_irqoff_attr[IVH_IRQOFF_ATTR_SLOTS]);
DEFINE_PER_CPU(u64, ivh_irqoff_attr_overflow);

static void ivh_irqoff_attr_record(unsigned long ret_ip)
{
	struct ivh_irqoff_attr_slot *tbl = this_cpu_ptr(ivh_irqoff_attr);
	int i;

	for (i = 0; i < IVH_IRQOFF_ATTR_SLOTS; i++) {
		if (tbl[i].ret_ip == ret_ip) {
			tbl[i].count++;
			return;
		}
		if (tbl[i].ret_ip == 0) {
			tbl[i].ret_ip = ret_ip;
			tbl[i].count = 1;
			return;
		}
	}
	this_cpu_inc(ivh_irqoff_attr_overflow);
}

/*
 * HLT/poll cycle accounting for ivh_pv_wait()'s halt paths. Declared in
 * <asm/ivh_tsc_beat.h>; production defines this in kernel/sched/core.c
 * (shared with the Step 6/8 phantom-steal correction that reads it). That
 * consumer is not ported here, so it is defined locally instead -- see the
 * file-level comment above for why. Counters accumulate unread for now.
 */
DEFINE_PER_CPU_ALIGNED(struct ivh_lock_halt, ivh_lock_halt);
EXPORT_PER_CPU_SYMBOL_GPL(ivh_lock_halt);

static int __init ivh_pv_beat_calibrate(void)
{
	if (tsc_khz)
		ivh_pv_beat_threshold = (unsigned long)((u64)tsc_khz *
					IVH_BEAT_THRESHOLD_US / 1000ULL);

	pr_info("IVH: TSC heartbeat threshold = %lu cycles (%llu us at tsc_khz=%u)\n",
		ivh_pv_beat_threshold, IVH_BEAT_THRESHOLD_US, tsc_khz);

	return 0;
}
late_initcall(ivh_pv_beat_calibrate);

#ifdef CONFIG_SYSCTL
/*
 * Drain every CPU that might already be committed to ivh_pv_wait()'s shared
 * PV-native-halt path (read a mode for which ivh_mode_uses_hypercall() was
 * true before this write landed -- VANILLA always, ADAPTIVE whenever
 * ivh_pv_allowed()) before a live transition to a mode for which it's now
 * false completes. Found by independent review: a non-hypercall mode never
 * sends KVM_HC_KICK_CPU, so a CPU already past that READ_ONCE and headed for
 * the bare, RFLAGS.IF=0 halt() would otherwise permanently lose its only
 * wake vehicle the instant the mode flips -- the exact 2026-07-24
 * hard-freeze class, just reached via a sysctl write instead of a wait/kick
 * race.
 *
 * Correctness relies on KVM_HC_KICK_CPU's pv_unhalted being a LATCHING flag
 * (arch/x86/kvm/lapic.c, kvm_pv_kick_cpu_op()): a kick delivered at any point
 * during a CPU's in-flight window -- including before it has disabled IRQs
 * or reached halt() at all -- makes that CPU's next halt() (if it takes one
 * at all before exiting the branch) return immediately. So this does not
 * need to catch anyone precisely "at" halt(); it only needs to keep kicking
 * every online CPU on every pass until none of them still report being in
 * the branch, which bounds the set of CPUs that could still reach a
 * not-yet-latched halt() to CPUs this loop has not yet observed as
 * in-flight -- and it re-kicks all of them every single pass regardless.
 * Kicking an already-running (or already-woken) vCPU is harmless.
 *
 * Bounded, not indefinite: a real stuck CPU (NMI storm, host-side stall)
 * should surface as a warning, not hang this sysctl write forever.
 */
#define IVH_MODE_TRANSITION_MAX_PASSES 200

static void ivh_pv_hypercall_kick(int cpu);

/*
 * G-LOCK-22-hybrid: renamed from ivh_pv_drain_vanilla_halts() to
 * ivh_pv_drain_hypercall_halts(). The set of CPUs that can be relying on
 * the KVM_HC_KICK_CPU hypercall latch is no longer just mode VANILLA --
 * mode ADAPTIVE joins it whenever ivh_pv_allowed() is true, and (since
 * ivh_pv_allow now applies to VANILLA too, see its comment above) VANILLA
 * only ever joins it when ivh_pv_allowed() as well -- there is no longer any
 * config where VANILLA hypercalls but ADAPTIVE doesn't, or vice versa.
 * ivh_pv_halt_inflight (renamed from ivh_vanilla_inflight) is now set by
 * BOTH branches that take the shared native-halt path, so this drain covers
 * both without change to its own logic.
 *
 * Guarded on ivh_pv_unhalt_avail (the raw hardware fact) rather than
 * ivh_pv_allowed()'s policy purely as defensive belt-and-braces: the caller
 * (ivh_pv_proc_adaptive_mode()) only ever reaches this call when its own
 * trigger condition already implies ivh_mode_uses_hypercall(old) was true,
 * which in turn implies ivh_pv_allowed() and hence ivh_pv_unhalt_avail --
 * so this guard cannot actually be exercised as false here, it just documents
 * the invariant this function relies on rather than silently assuming it.
 *
 * A transition INTO VANILLA while !ivh_pv_allowed() is NOT this function's
 * job and is handled separately (see ivh_pv_proc_adaptive_mode()'s
 * reschedule sweep): neither the old mode (PURE_IPI, or ADAPTIVE without PV)
 * nor the new one (VANILLA without PV) ever hypercalls, so there is no
 * hypercall-latch rescue to perform -- the stranded population there is
 * IPI-wake waiters, a different vehicle entirely.
 */
static void ivh_pv_drain_hypercall_halts(void)
{
	int pass, cpu;
	bool any_inflight;

	if (!ivh_pv_unhalt_avail)
		return;		/* no CPU could have reached the IF=0 halt() either */

	for (pass = 0; pass < IVH_MODE_TRANSITION_MAX_PASSES; pass++) {
		any_inflight = false;
		for_each_online_cpu(cpu) {
			ivh_pv_hypercall_kick(cpu);
			if (READ_ONCE(per_cpu(ivh_pv_halt_inflight, cpu)))
				any_inflight = true;
		}
		if (!any_inflight)
			return;
		msleep(1);
	}

	pr_warn("IVH: ivh_adaptive_mode transition drain did not converge after %d passes -- some CPU may still be relying on the hypercall wake this write just took away. Check ivh_pv_halt_inflight per-CPU.\n",
		IVH_MODE_TRANSITION_MAX_PASSES);
}

/*
 * ivh_adaptive_mode: reject anything above 2 (IVH_MODE_ADAPTIVE).
 *
 * Still worth a warning, not a rejection: mode 2's tier-2 early bail is a
 * dead branch whenever ivh_pv_preempt_src==0 on a host without
 * KVM_FEATURE_STEAL_TIME (vcpu_is_preempted() is then hardwired to return
 * false -- see is_wait_preempted(), kernel/locking/qspinlock_paravirt.h).
 * Silently-dead tier-2 was exactly the trap that produced weeks of null
 * A/B results before this rebuild; surface it at the moment it would bite.
 */
static int ivh_pv_proc_adaptive_mode(const struct ctl_table *table, int write,
				     void *buffer, size_t *lenp, loff_t *ppos)
{
	unsigned long old = READ_ONCE(ivh_adaptive_mode);
	unsigned long val = old;
	struct ctl_table tmp = *table;
	int ret;

	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (val > IVH_MODE_ADAPTIVE) {
		pr_err("IVH: refusing ivh_adaptive_mode=%lu: valid values are 0 (vanilla), 1 (pure IPI) and 2 (IVH adaptive)\n",
		       val);
		return -EINVAL;
	}

	if (val == IVH_MODE_ADAPTIVE && !READ_ONCE(ivh_pv_preempt_src) &&
	    !kvm_para_has_feature(KVM_FEATURE_STEAL_TIME))
		pr_warn("IVH: ivh_adaptive_mode=2 with ivh_pv_preempt_src=0 on a host with no KVM_FEATURE_STEAL_TIME: vcpu_is_preempted() is hardwired false here, so tier 2 can never fire. Set ivh_pv_preempt_src=2 (TSC heartbeat) for mode 2 to do anything.\n");

	WRITE_ONCE(ivh_adaptive_mode, val);

	/*
	 * G-LOCK-22-hybrid: the drain trigger is now "did the OLD mode use the
	 * hypercall and the NEW one doesn't", not "old was VANILLA". With
	 * ivh_pv_allow fixed for the boot, ivh_mode_uses_hypercall()'s answer
	 * for each mode never changes except via this WRITE_ONCE, so this is
	 * exhaustive over all 6 possible transitions: e.g. with PV allowed and
	 * available, VANILLA<->ADAPTIVE now needs NO drain (both hypercall),
	 * while ADAPTIVE->PURE_IPI does (this is the new freeze-class case a
	 * VANILLA-only check would have missed).
	 */
	if (ivh_mode_uses_hypercall(old) && !ivh_mode_uses_hypercall(val))
		ivh_pv_drain_hypercall_halts();

	/*
	 * G-LOCK-22-hybrid, round-3 review finding: a transition INTO VANILLA
	 * while !ivh_pv_allowed() is not a hypercall-latch rescue (neither old
	 * mode PURE_IPI/ADAPTIVE-without-PV, nor new mode VANILLA-without-PV,
	 * ever hypercalls -- see ivh_pv_drain_hypercall_halts()'s comment), but
	 * it CAN otherwise strand a waiter already parked in the IPI-wake
	 * branch's safe_halt() (IF=1) from the OLD mode: ivh_wake() now
	 * correctly sends NOTHING once a concurrent unlocker reads the new
	 * mode==VANILLA (see ivh_wake()'s own comment), but that waiter is
	 * still genuinely halted awaiting the IPI a call already in flight was
	 * about to send under the OLD mode. Not the 2026-07-24 freeze class --
	 * IF=1 means the next timer tick alone releases it -- but real,
	 * avoidable latency rather than an instant wake. One unconditional
	 * sweep closes it: same "kick everyone once, an unneeded kick is
	 * harmless" logic as the drain above, just smp_send_reschedule()
	 * instead of the hypercall since this population's vehicle is the IPI.
	 */
	if (val == IVH_MODE_VANILLA && old != IVH_MODE_VANILLA && !ivh_pv_allowed()) {
		int cpu;

		for_each_online_cpu(cpu)
			smp_send_reschedule(cpu);
	}

	return 0;
}

/*
 * ivh_pv_preempt_src: reject anything above 2, and refuse to make the
 * heartbeat AUTHORITATIVE (2) until every online CPU has actually published
 * at least once -- an unseeded slot reads 0, so ivh_beat_age() returns a
 * full rdtsc() and pv_wait_early() would report "preempted" forever, silently.
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

	if (val != 2 && READ_ONCE(ivh_adaptive_mode) == IVH_MODE_ADAPTIVE &&
	    !kvm_para_has_feature(KVM_FEATURE_STEAL_TIME))
		pr_warn("IVH: ivh_pv_preempt_src=%lu while ivh_adaptive_mode=2 on a host with no KVM_FEATURE_STEAL_TIME: vcpu_is_preempted() is hardwired false here, so tier 2 can no longer fire. Set src back to 2 (TSC heartbeat) for mode 2 to do anything.\n",
			val);

	/*
	 * ivh_pv_tier1_confirm==2 depends on src==2 the same way tier 2 itself
	 * does (is_wait_preempted() at src!=2 either reads the hardwired-false
	 * KVM bit or, at src==1, still returns it) -- dropping src below 2
	 * silently turns confirm==2 into "suppress every tier-1 bail
	 * unconditionally" rather than the intended "confirm against the
	 * heartbeat first."
	 */
	if (val != 2 && READ_ONCE(ivh_pv_tier1_confirm) == 2)
		pr_warn("IVH: ivh_pv_preempt_src=%lu while ivh_pv_tier1_confirm=2: is_wait_preempted() no longer reads the TSC heartbeat, so every tier-1 trip will now be suppressed unconditionally. Set ivh_pv_tier1_confirm back to 0/1 or src back to 2.\n",
			val);

	WRITE_ONCE(ivh_pv_preempt_src, val);
	return 0;
}

/*
 * ivh_pv_tier1_confirm: reject anything above 2, reject making it
 * authoritative (2) unless ivh_pv_preempt_src==2 (see the comment on
 * ivh_pv_preempt_src's own handler above -- same hazard, same reason), and
 * warn if the knob is set to anything nonzero while it's inert
 * (ivh_adaptive_mode != ADAPTIVE never reaches pv_wait_early()'s tier-1
 * block's confirm check at all).
 */
static int ivh_pv_proc_tier1_confirm(const struct ctl_table *table, int write,
				     void *buffer, size_t *lenp, loff_t *ppos)
{
	unsigned long val = READ_ONCE(ivh_pv_tier1_confirm);
	struct ctl_table tmp = *table;
	int ret;

	tmp.data = &val;
	ret = proc_doulongvec_minmax(&tmp, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (val > 2) {
		pr_err("IVH: refusing ivh_pv_tier1_confirm=%lu: valid values are 0 (upstream tier-1, bit-identical), 1 (shadow: measure only, still bail) and 2 (authoritative: an unconfirmed trip does not bail)\n",
		       val);
		return -EINVAL;
	}

	if (val == 2 && READ_ONCE(ivh_pv_preempt_src) != 2) {
		pr_err("IVH: refusing ivh_pv_tier1_confirm=2: requires ivh_pv_preempt_src=2 (TSC heartbeat authoritative) -- at src!=2, is_wait_preempted() would not read the heartbeat at all, so confirm=2 would suppress every tier-1 bail unconditionally instead of confirming it against anything.\n");
		return -EINVAL;
	}

	if (val && READ_ONCE(ivh_adaptive_mode) != IVH_MODE_ADAPTIVE)
		pr_warn("IVH: ivh_pv_tier1_confirm=%lu while ivh_adaptive_mode!=2 (ADAPTIVE): inert until mode is switched to ADAPTIVE.\n",
			val);

	WRITE_ONCE(ivh_pv_tier1_confirm, val);
	return 0;
}

/*
 * The publish mask must stay coarser than or equal to PV_PREV_CHECK_MASK
 * (0xff, kernel/locking/qspinlock_paravirt.h) and must be a (2^n - 1) form,
 * because the spin loops test `(loop & mask) == 0`.
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

	if (val < 0xffUL || (val & (val + 1))) {
		pr_err("IVH: refusing ivh_pv_beat_publish_mask=0x%lx: must be of the form 2^n-1 and >= PV_PREV_CHECK_MASK (0xff)\n",
		       val);
		return -EINVAL;
	}

	WRITE_ONCE(ivh_pv_beat_publish_mask, val);
	return 0;
}

/*
 * Clamp for ivh_pv_spin_threshold: even after widening pv_wait_node()'s and
 * pv_wait_head_or_lock()'s loop counters to unsigned long (so a large value
 * can no longer truncate into a negative/wrapped int), an UNBOUNDED sysctl
 * here is still a live-tuning footgun on a knob meant for hand sweeping: a
 * multi-billion-iteration value spins with IRQs disabled for the length of
 * one attempt, which is indistinguishable from a hang / soft-lockup / RCU
 * stall. 1 << 24 (16.7M) is a generous ceiling -- three orders of magnitude
 * above the 1<<15 default, comfortably enough range for any real sweep,
 * while keeping worst case bounded to a fraction of a second.
 */
static unsigned long ivh_spin_thresh_min = 1UL;
static unsigned long ivh_spin_thresh_max = 1UL << 24;

static const struct ctl_table ivh_pv_sysctls[] = {
	{
		.procname	= "ivh_adaptive_mode",
		.data		= &ivh_adaptive_mode,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= ivh_pv_proc_adaptive_mode,
	},
	{
		.procname	= "ivh_pv_wait_trace",
		.data		= &ivh_pv_wait_trace,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_pv_preempt_src",
		.data		= &ivh_pv_preempt_src,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= ivh_pv_proc_preempt_src,
	},
	{
		.procname	= "ivh_pv_tier1_confirm",
		.data		= &ivh_pv_tier1_confirm,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= ivh_pv_proc_tier1_confirm,
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
	{
		.procname	= "ivh_pv_irqoff_halt",
		.data		= &ivh_pv_irqoff_halt,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_pv_tier1_enable",
		.data		= &ivh_pv_tier1_enable,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_pv_spin_threshold",
		.data		= &ivh_pv_spin_threshold,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
		.extra1		= &ivh_spin_thresh_min,
		.extra2		= &ivh_spin_thresh_max,
	},
	{
		.procname	= "ivh_adaptive_irqoff_bail_gate",
		.data		= &ivh_adaptive_irqoff_bail_gate,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_slowpath_wait_measure",
		.data		= &ivh_slowpath_wait_measure,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0644,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		/*
		 * Read-only mirror of the ivh_pv_allow=0/1 boot parameter (fixed
		 * for the life of the boot, see its definition above) so the
		 * effective policy is checkable at runtime without parsing
		 * /proc/cmdline. mode 0444: the write is refused by procfs's OWN
		 * permission check (sysctl_perm()/test_perm() in
		 * fs/proc/proc_sysctl.c, -EACCES before proc_doulongvec_minmax()
		 * is ever entered), same pattern the kernel uses elsewhere for
		 * read-only proc_doulongvec entries. This is not incidental:
		 * ivh_pv_allow itself is __ro_after_init, so if this mode were
		 * ever "simplified" to 0644 believing the handler alone protects
		 * it, a write would reach proc_doulongvec_minmax() and fault
		 * writing to read-only kernel memory under
		 * CONFIG_STRICT_KERNEL_RWX.
		 */
		.procname	= "ivh_pv_allow",
		.data		= &ivh_pv_allow,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0444,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		.procname	= "ivh_pv_unhalt_avail",
		.data		= &ivh_pv_unhalt_avail_sysctl,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0444,
		.proc_handler	= proc_doulongvec_minmax,
	},
	{
		/*
		 * Read-only mirror of the ivh_pv_tas=0/1 boot parameter, same
		 * rationale and 0444 mechanism as ivh_pv_allow above. Registered
		 * via the unconditional late_initcall below regardless of
		 * whether kvm_spinlock_init() early-returned for ivh_pv_tas=1
		 * -- this is the ONLY way a userspace script can distinguish
		 * "this boot is real TAS" from "this boot has PV registered"
		 * without parsing /proc/cmdline or grepping dmesg.
		 */
		.procname	= "ivh_pv_tas",
		.data		= &ivh_pv_tas,
		.maxlen		= sizeof(unsigned long),
		.mode		= 0444,
		.proc_handler	= proc_doulongvec_minmax,
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
 * Republish this vCPU's TSC heartbeat the instant it comes back from an
 * EXPLICIT halt (mode VANILLA's PV_UNHALT halt()/safe_halt(), modes
 * PURE_IPI/ADAPTIVE's safe_halt()). A vCPU parked in HLT publishes nothing,
 * so on wake its stamp is stale until the next tick -- up to 1ms at
 * HZ=1000, during which
 * every waiter queued behind it reads it as host-preempted when it just
 * woke. Deliberately UNCONDITIONAL: one rdtsc plus one store on a path that
 * already took a HLT vmexit, so the cost is unmeasurable there.
 */
static __always_inline void ivh_beat_halt_exit(void)
{
	ivh_tsc_beat_publish();
	this_cpu_inc(ivh_beat_publishes);
}

/*
 * Kick a cpu by its apicid -- the stock, host-cooperative wake. Used by
 * ivh_wake() for any mode where ivh_mode_uses_hypercall() is true (VANILLA
 * or ADAPTIVE, whenever ivh_pv_allowed()), and by
 * ivh_pv_drain_hypercall_halts()'s rescue sweep. Verbatim behavior of the
 * pre-IVH kvm_kick_cpu().
 */
static void ivh_pv_hypercall_kick(int cpu)
{
	u32 apicid = per_cpu(x86_cpu_to_apicid, cpu);

	kvm_hypercall2(KVM_HC_KICK_CPU, 0, apicid);
}

/*
 * G-LOCK-22-hybrid: the shared PV-native halt/wake body. Taken by modes
 * VANILLA and ADAPTIVE alike, whenever ivh_pv_allowed() -- byte-for-byte
 * identical either way, which is the whole point: ADAPTIVE-with-PV needs no
 * interrupt-handling logic of its own, because the hypercall's latching
 * pv_unhalted already wakes an IF=0 halt() natively (see kvm.c's file-level
 * comment / lapic.c's kvm_pv_kick_cpu_op()) -- there is no reopened-IF
 * window to reason about here at all, unlike the IPI-wake branch below.
 *
 * ivh_pv_halt_inflight is set for the whole call (not just around the
 * actual halt), so the transition drain (ivh_pv_drain_hypercall_halts()) can
 * catch a CPU that already committed to this branch but hasn't reached
 * halt() yet. Cleared before every return. this_cpu_inc(), not a bool
 * store, for the same reason the original VANILLA-only version used it:
 * the drain's guarantee comes from the writer's WRITE_ONCE plus at least
 * one full pass reading every online CPU at 0, not from this flag alone.
 *
 * ivh_lock_halt_begin/end: this is a HLT taken OUTSIDE the idle loop, so
 * tick_nohz's idle accumulators never see it. Measure it here or it
 * becomes phantom steal -- see struct ivh_lock_halt in <asm/ivh_tsc_beat.h>.
 */
static __always_inline void ivh_pv_native_halt(u8 *ptr, u8 val)
{
	this_cpu_inc(ivh_pv_halt_inflight);

	if (irqs_disabled()) {
		this_cpu_inc(ivh_wait_pv_halt_irqoff);
		if (READ_ONCE(*ptr) == val) {
			ivh_lock_halt_begin(false);
			halt();
			ivh_beat_halt_exit();
			ivh_lock_halt_end();
		}
	} else {
		this_cpu_inc(ivh_wait_pv_halt_irqon);
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

	this_cpu_dec(ivh_pv_halt_inflight);
}

static void ivh_pv_wait(u8 *ptr, u8 val)
{
	unsigned long mode;

	if (in_nmi())
		return;

	this_cpu_inc(ivh_pv_wait_calls);
	mode = READ_ONCE(ivh_adaptive_mode);

	/*
	 * PV-native halt/wake: mode VANILLA always takes this when the host
	 * advertises PV_UNHALT (byte-for-byte the pre-IVH kvm_wait() body),
	 * and mode ADAPTIVE joins it whenever ivh_pv_allowed() -- see
	 * <asm/qspinlock.h>'s mode-table comment. Both are woken by the SAME
	 * KVM_HC_KICK_CPU hypercall latch in ivh_wake(), and both are tracked
	 * by the SAME ivh_pv_halt_inflight, so the freeze-hazard drain covers
	 * both without needing to know which mode a given CPU is in.
	 */
	if (ivh_mode_uses_hypercall(mode)) {
		ivh_pv_native_halt(ptr, val);
		return;
	}

	if (mode == IVH_MODE_VANILLA) {
		/* !ivh_pv_allowed(): match upstream's plain cpu_relax() busy
		 * loop exactly -- nothing to wait for a wake vehicle on, since
		 * none will ever arrive (see ivh_wake()'s matching no-op). The
		 * fifth and last member of the ivh_pv_wait_calls partition --
		 * counted, unlike upstream, so this population is visible once
		 * ivh_pv_allow=0 makes it reachable on a host that otherwise
		 * advertises PV_UNHALT. */
		this_cpu_inc(ivh_wait_vanilla_nopv_spin);
		while (READ_ONCE(*ptr) == val)
			cpu_relax();
		return;
	}

	/*
	 * IPI-wake path: mode PURE_IPI always, mode ADAPTIVE whenever
	 * !ivh_pv_allowed(). A real halt/safe_halt (genuine vCPU yield, a
	 * real HLT vmexit the host observes and can reschedule the
	 * descheduled lock holder onto), woken by the real
	 * smp_send_reschedule() IPI in ivh_wake() -- NOT by KVM_HC_KICK_CPU.
	 *
	 * Why this is correct without the hypercall: HLT in a guest always
	 * vmexits (host-side HLT-passthrough is only enabled for dedicated
	 * pCPUs / KVM_HINTS_REALTIME, and kvm_spinlock_init() already routes
	 * that case to native qspinlock so this path is never reached then)
	 * -- the yield is real and host-visible regardless of PV_UNHALT,
	 * which only ever optimized the *wake*. A HLT-exited (host-blocked)
	 * vCPU is un-halted by ANY interrupt delivered to its LAPIC, and
	 * smp_send_reschedule() sends a genuine RESCHEDULE_VECTOR APIC IPI,
	 * which is baseline interrupt-driven wake, not a paravirt feature.
	 *
	 * *** DO NOT halt() here when IRQs are ALREADY disabled. ***
	 *
	 * Per the Intel SDM Vol.2 and the AMD APM: with RFLAGS.IF=0 a
	 * maskable interrupt is recognized and left pending in the IRR but
	 * does NOT un-halt the core -- only NMI/SMI/INIT/RESET do. With IF=0
	 * the only unconditional wake left is vcpu->arch.pv.pv_unhalted, set
	 * by exactly one thing: the KVM_HC_KICK_CPU hypercall, which this
	 * branch never sends by design. A RESCHEDULE_VECTOR IPI is maskable
	 * and can never un-halt an IF=0 HLT. This is precisely the
	 * 2026-07-24 hard-freeze root cause: a HLT taken with IF=0, woken
	 * only by a vehicle that provably cannot wake it, producing a
	 * silent, unrecoverable whole-VM freeze with no oops.
	 *
	 * So this branch halts ONLY on the path where IF=1 is guaranteed at
	 * the HLT (local_irq_disable + safe_halt's atomic sti;hlt). When the
	 * caller already had IRQs off, we must not block at all here: for
	 * mode ADAPTIVE that's a deliberate choice (see below, not a gap --
	 * PV-native halt above already covers the IF=0 case whenever PV is
	 * allowed, so this corner exists only when PV genuinely isn't); for
	 * mode PURE_IPI it remains the irreducible gap it always was.
	 */
	if (!irqs_disabled()) {
		this_cpu_inc(ivh_wait_ipi_halt_irqon);
		local_irq_disable();
		if (READ_ONCE(*ptr) == val) {
			ivh_pv_trace("HALT enter (safe_halt, IF=1 at hlt)");
			ivh_lock_halt_begin(false);
			safe_halt();		/* sti;hlt -- HLT taken with IF=1 */
			ivh_beat_halt_exit();
			ivh_lock_halt_end();
			ivh_pv_trace("HALT exit (woke)");
		} else {
			ivh_pv_trace("no-halt (condition cleared before halt)");
			local_irq_enable();
		}
		return;
	}

	/*
	 * IRQs already disabled by an outer context, and no hypercall
	 * available to this waiter (mode PURE_IPI always; mode ADAPTIVE when
	 * !ivh_pv_allowed()). Behave as an uninstrumented, immediately-
	 * rechecking cpu_relax() loop -- no fixed floor, no PV bookkeeping,
	 * no hypercall. Counted, not silent: a large ivh_wait_irqoff_nohalt
	 * means this workload's irqsave-held-lock population is making this
	 * config materially less halt-y than PV-native.
	 */
	this_cpu_inc(ivh_wait_irqoff_nohalt);
	ivh_irqoff_attr_record(this_cpu_read(qlock_slowpath_caller_ip));
	ivh_pv_trace("native-spin (irqs already disabled on entry)");

	/*
	 * Idea 4 EXPERIMENTAL path -- PURE_IPI only now. Mode ADAPTIVE never
	 * consults ivh_pv_irqoff_halt: whenever PV is allowed it already took
	 * the hypercall-halt branch above with no reopened-IF window needed
	 * at all (the hypercall wakes an IF=0 halt natively -- Idea 4's whole
	 * benefit, for free, with none of Hazard A's risk); whenever PV is
	 * NOT allowed, ADAPTIVE deliberately just busy-spins here rather than
	 * reopening IF, since this corner is judged not worth Hazard A's risk
	 * under the mode meant to be safe-by-default. The knob stays reachable
	 * under PURE_IPI so Hazard-A evidence-gathering can continue there.
	 *
	 * in_nmi()/in_hardirq()/in_serving_softirq() are refused
	 * unconditionally: that's a real but narrower protection (this CPU is
	 * not ALREADY inside an interrupt handler trying to use this path
	 * recursively) -- it does NOT protect against the hazard this path
	 * exists to test (an INDEPENDENT interrupt landing during the window
	 * opened below).
	 */
	if (mode == IVH_MODE_PURE_IPI && READ_ONCE(ivh_pv_irqoff_halt) &&
	    !in_nmi() && !in_hardirq() && !in_serving_softirq()) {
		this_cpu_inc(ivh_irqoff_halt_used);
		if (READ_ONCE(*ptr) == val) {
			ivh_pv_trace("HALT enter (irqoff-halt, IF=1 at hlt)");
			ivh_lock_halt_begin(false);
			safe_halt();		/* sti;hlt -- HLT taken with IF=1 */
			ivh_beat_halt_exit();
			ivh_lock_halt_end();
			local_irq_disable();	/* restore: entry to this function was IRQs-off */
			ivh_pv_trace("HALT exit (woke, irqoff-halt)");
		}
		/* else: condition already cleared; IRQs are still off from entry, nothing to restore */
		WARN_ONCE(!irqs_disabled(),
			  "IVH Idea 4: ivh_pv_wait() returning with IRQs enabled, entry was disabled");
		return;
	}

	while (READ_ONCE(*ptr) == val)
		cpu_relax();
}

/*
 * The single shared wake vehicle for both IVH modes' one remaining wake
 * site (see pv_kick_node()'s comment for why there is only one -- vanilla
 * itself sends nothing at the node-handoff site). ivh_mode_uses_hypercall()
 * is the SAME predicate ivh_pv_wait() used to decide whether to sleep via
 * the hypercall-halt path, so the two can never independently drift on the
 * vehicle a given waiter actually needs -- both VANILLA and ADAPTIVE
 * hypercall whenever ivh_pv_allowed(), PURE_IPI never does.
 *
 * VANILLA gets one further exception IPI-side that ADAPTIVE does NOT:
 * when !ivh_pv_allowed(), VANILLA's own ivh_pv_wait() branch (kvm.c) never
 * halts at all -- it degrades to an uninstrumented busy-spin with nothing
 * to wake, matching upstream's own PV_UNHALT-absent behavior exactly. So
 * mode VANILLA sends NEITHER vehicle in that case; only ADAPTIVE (which
 * DOES halt via safe_halt()+IPI in that case, see ivh_pv_wait()) and
 * PURE_IPI reach the smp_send_reschedule() below.
 */
static void ivh_wake(int cpu)
{
	unsigned long mode = READ_ONCE(ivh_adaptive_mode);

	if (ivh_mode_uses_hypercall(mode)) {
		this_cpu_inc(ivh_wake_hypercall);
		ivh_pv_hypercall_kick(cpu);
		return;
	}

	if (mode == IVH_MODE_VANILLA) {
		this_cpu_inc(ivh_wake_vanilla_nopv_noop);
		return;		/* nobody halted -- see comment above */
	}

	this_cpu_inc(ivh_wake_ipi);
	ivh_pv_trace("KICK target_cpu=%d via smp_send_reschedule (RESCHEDULE_VECTOR IPI)", cpu);
	smp_send_reschedule(cpu);
}

static void ivh_pv_kick(int cpu)
{
	ivh_wake(cpu);
}

/*
 * Setup pv_lock_ops to exploit KVM_FEATURE_PV_UNHALT if present.
 */
void __init kvm_spinlock_init(void)
{
	/*
	 * G-LOCK-22-hybrid: ivh_pv_tas=1 -- return BEFORE anything else in
	 * this function, including the ivh_pv_unhalt_avail probe below:
	 * nothing this function would otherwise do matters once PV
	 * registration is skipped, and skipping it here is what leaves
	 * virt_spin_lock_key exactly as native_pv_lock_init() (called earlier
	 * in setup_arch(), see paravirt.c) already set it -- enabled, for any
	 * X86_FEATURE_HYPERVISOR guest, which this always is. This is the ONE
	 * safe way to get real, unmodified TAS spinlocks in this tree; see
	 * ivh_pv_tas's own comment above for why a live equivalent was
	 * rejected.
	 */
	if (READ_ONCE(ivh_pv_tas)) {
		pr_info("IVH: ivh_pv_tas=1, using real TAS/virt_spin_lock (PV spinlock substitute NOT registered; ivh_adaptive_mode/ivh_pv_allow are both inert this boot)\n");
		return;
	}

	/*
	 * G-LOCK-22-hybrid: cache the PV_UNHALT feature probe exactly once,
	 * here, before any lock contention exists on this CPU -- see
	 * ivh_pv_unhalt_avail's definition above for why this must never be
	 * a live re-read on the hot halt/wake path on a TDX guest.
	 */
	ivh_pv_unhalt_avail = kvm_para_has_feature(KVM_FEATURE_PV_UNHALT);
	ivh_pv_unhalt_avail_sysctl = ivh_pv_unhalt_avail ? 1UL : 0UL;

	/*
	 * IVH: unlike stock KVM we deliberately do NOT bail when the host lacks
	 * KVM_FEATURE_PV_UNHALT. ivh_pv_wait()/ivh_pv_kick() are always safe to
	 * register here regardless of that feature bit: internally they check
	 * it themselves to pick between the stock host-cooperative
	 * halt/hypercall-kick behavior and IVH's own IPI-wake modes,
	 * live-toggleable at runtime via the ivh_adaptive_mode sysctl. What
	 * must NOT be host-feature- or sysctl-dependent is *this*
	 * registration itself: pv_ops.lock.* and virt_spin_lock_key are set up
	 * exactly once here, at boot, before any concurrent lock activity
	 * exists on this CPU. queued_spin_lock_slowpath() re-checks
	 * virt_spin_lock_key on every single contended acquisition of every
	 * lock in the kernel, so flipping it under load could strand TAS-mode
	 * and MCS-queued waiters on the same lock at once.
	 *
	 * When dedicated pCPUs are advertised there is no lock-holder
	 * preemption to mitigate, so plain native fair qspinlock is best.
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

	pr_info("IVH: PV spinlock substitute registered (TAS virt_spin_lock disabled, MCS queueing restored); ivh_adaptive_mode=%lu selects vanilla/pure-IPI/adaptive wake at runtime\n",
		ivh_adaptive_mode);

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
