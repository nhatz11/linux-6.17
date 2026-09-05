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
 * stock upstream's own check) fire count, mechanism-independent -- this
 * branch in pv_wait_early() is reached and evaluated for every mechanism,
 * including mechanism=0, unlike the tier-2 counters above which only ever
 * increment once is_wait_preempted() is reached (mechanism != 0). Exists
 * to let the tier-1-vs-tier-2 resolution ratio be measured directly,
 * instead of inferred from tier-2's counts alone.
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
DEFINE_PER_CPU(u64, ivh_wake_hypercall);
DEFINE_PER_CPU(u64, ivh_wake_ipi);
DEFINE_PER_CPU(u64, ivh_wait_irqoff_nohalt);

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
 * ivh_adaptive_mode: reject anything above 2 (IVH_MODE_ADAPTIVE). Unlike the
 * pre-rebuild mechanism/kick-knob pile, there is no unsafe combination to
 * guard here: mode 0 always wakes via the hypercall (safe with an IF=0
 * halt), modes 1/2 never reach that halt shape at all (see ivh_pv_wait()),
 * so every value of this one knob is self-consistent by construction.
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
	unsigned long val = READ_ONCE(ivh_adaptive_mode);
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

	WRITE_ONCE(ivh_pv_preempt_src, val);
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
 * EXPLICIT halt (mechanism 0's PV_UNHALT halt()/safe_halt(), mechanism 2's
 * safe_halt()). A vCPU parked in HLT publishes nothing, so on wake its
 * stamp is stale until the next tick -- up to 1ms at HZ=1000, during which
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
 * Kick a cpu by its apicid -- the stock, host-cooperative wake, used only
 * by the sysctl==0 fallback path below when the host actually advertises
 * KVM_FEATURE_PV_UNHALT. Verbatim behavior of the pre-IVH kvm_kick_cpu().
 */
static void ivh_pv_hypercall_kick(int cpu)
{
	u32 apicid = per_cpu(x86_cpu_to_apicid, cpu);

	kvm_hypercall2(KVM_HC_KICK_CPU, 0, apicid);
}

static void ivh_pv_wait(u8 *ptr, u8 val)
{
	if (in_nmi())
		return;

	this_cpu_inc(ivh_pv_wait_calls);

	/*
	 * mode VANILLA (default): behave as close to "no IVH" as this call
	 * site allows.
	 *   - If the host advertises KVM_FEATURE_PV_UNHALT, this is byte-for-
	 *     byte the pre-IVH kvm_wait() body: a real halt/safe_halt, woken by
	 *     ivh_pv_kick()'s hypercall below.
	 *   - If not, a plain cpu_relax() busy loop, matching upstream exactly.
	 */
	if (READ_ONCE(ivh_adaptive_mode) == IVH_MODE_VANILLA) {
		/*
		 * ivh_lock_halt_begin/end: this is a HLT taken OUTSIDE the idle
		 * loop, so tick_nohz's idle accumulators never see it. Measure
		 * it here or it becomes phantom steal -- see struct
		 * ivh_lock_halt in <asm/ivh_tsc_beat.h>.
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
	 * modes PURE_IPI / ADAPTIVE: a real halt/safe_halt (genuine vCPU
	 * yield, a real HLT vmexit the host observes and can reschedule the
	 * descheduled lock holder onto), woken by the real smp_send_reschedule()
	 * IPI in ivh_pv_kick() -- NOT by KVM_HC_KICK_CPU, which these modes
	 * never send, and therefore NOT gated on
	 * kvm_para_has_feature(KVM_FEATURE_PV_UNHALT).
	 *
	 * Why this is correct without PV_UNHALT: HLT in a guest always
	 * vmexits (host-side HLT-passthrough is only enabled for dedicated
	 * pCPUs / KVM_HINTS_REALTIME, and kvm_spinlock_init() below already
	 * routes that case to native qspinlock so this path is never reached
	 * then) -- the yield is real and host-visible regardless of
	 * PV_UNHALT, which only ever optimized the *wake*. A HLT-exited
	 * (host-blocked) vCPU is un-halted by ANY interrupt delivered to its
	 * LAPIC, and smp_send_reschedule() sends a genuine RESCHEDULE_VECTOR
	 * APIC IPI, which is baseline interrupt-driven wake, not a paravirt
	 * feature.
	 *
	 * *** DO NOT halt() here when IRQs are ALREADY disabled. ***
	 *
	 * Per the Intel SDM Vol.2 and the AMD APM: with RFLAGS.IF=0 a
	 * maskable interrupt is recognized and left pending in the IRR but
	 * does NOT un-halt the core -- only NMI/SMI/INIT/RESET do. KVM
	 * faithfully reproduces that (kvm_arch_vcpu_runnable()'s maskable-
	 * interrupt term is gated on the guest's RFLAGS.IF); with IF=0 the
	 * only unconditional wake left is vcpu->arch.pv.pv_unhalted, set by
	 * exactly one thing: the KVM_HC_KICK_CPU hypercall, which these modes
	 * never send by design. A RESCHEDULE_VECTOR IPI is maskable and can
	 * never un-halt an IF=0 HLT. This is precisely the 2026-07-24
	 * hard-freeze root cause: a HLT taken with IF=0, woken only by a
	 * vehicle (a maskable IPI) that provably cannot wake it, producing a
	 * silent, unrecoverable whole-VM freeze with no oops.
	 *
	 * So these modes halt ONLY on the path where IF=1 is guaranteed at
	 * the HLT (local_irq_disable + safe_halt's atomic sti;hlt). When the
	 * caller already had IRQs off, we must not block at all: this is an
	 * IRREDUCIBLE gap versus mode VANILLA (see ivh_wait_irqoff_nohalt),
	 * not a bug -- there is no hypercall to fall back on in these modes.
	 *
	 * For a role-C queued MCS waiter (pv_wait_node()), this branch is
	 * reached only when pv_wait_early() actually fired for the
	 * predecessor, in mode ADAPTIVE (in mode PURE_IPI it is reached
	 * unconditionally on SPIN_THRESHOLD exhaustion, same as VANILLA). A
	 * role-A/B waiter (queue head, pv_wait_head_or_lock()) has no
	 * predecessor to scope against and reaches this same branch
	 * UNCONDITIONALLY after its own SPIN_THRESHOLD in every mode --
	 * intentional, not an oversight: there is no vcpu_is_preempted()
	 * signal for "the current lock holder" from that path, only for an
	 * MCS predecessor.
	 */
	if (!irqs_disabled()) {
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
	 * IRQs already disabled by an outer context: never halt() here (see
	 * above). Behave as an uninstrumented, immediately-rechecking
	 * cpu_relax() loop -- no fixed floor, no PV bookkeeping, no
	 * hypercall. Counted, not silent: a large ivh_wait_irqoff_nohalt
	 * means this workload's irqsave-held-lock population is making
	 * modes 1/2 materially less halt-y than mode 0.
	 */
	this_cpu_inc(ivh_wait_irqoff_nohalt);
	ivh_pv_trace("native-spin (irqs already disabled on entry)");
	while (READ_ONCE(*ptr) == val)
		cpu_relax();
}

/*
 * The single shared wake vehicle for both IVH modes' one remaining wake
 * site (see pv_kick_node()'s comment for why there is only one -- vanilla
 * itself sends nothing at the node-handoff site). Mode VANILLA sends the
 * hypercall; modes PURE_IPI/ADAPTIVE send the IPI. Exactly one branch
 * decides the vehicle, so a future call site can't independently drift the
 * way the old two-knob (kick_pure_ipi/kick_node_ipi) design did.
 */
static void ivh_wake(int cpu)
{
	if (READ_ONCE(ivh_adaptive_mode) == IVH_MODE_VANILLA) {
		this_cpu_inc(ivh_wake_hypercall);
		if (kvm_para_has_feature(KVM_FEATURE_PV_UNHALT))
			ivh_pv_hypercall_kick(cpu);
		return;
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
