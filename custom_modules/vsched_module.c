#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/sched/topology.h>
#include <linux/cpumask.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/math64.h>   /* mul_u64_u64_div_u64 -- see steal_compare_read below */
#include <asm/paravirt.h>   /* pv_ops.lock.vcpu_is_preempted -- see vcap_preempted below */
#define MAX_TOPOLOGY_LEVEL 3

#define BUFSIZE 6000

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Liran B.H");

static struct proc_dir_entry *get_info_ent;
static struct proc_dir_entry *capacity_ent;
static struct proc_dir_entry *av_capacity_ent;
static struct proc_dir_entry *topo_ent;
static struct proc_dir_entry *latency_ent;
static struct proc_dir_entry *act_ent;
static struct proc_dir_entry *preempted_ent;
static struct proc_dir_entry *steal_compare_ent;

/* External function declarations */
extern void get_steal_and_preemptions(int cpunum, u64* preempt, u64* steal_time);
extern void reset_max_latency(u64 max_latency);
extern void get_max_latency(int cpunum, u64* max_latency);
extern void set_custom_capacity(unsigned long capacity, int cpu);
/* 6.17: arg order is (cpunum, avg_latency) — matches the call in set_latencies() */
extern void set_avg_latency(int cpunum, u64 avg_latency);
extern void set_live_topology(struct sched_domain_topology_level *topology);
extern void set_average_capacity_all(int value);
/* 6.17: get_average_capacity_all is a function, not a plain variable */
extern int get_average_capacity_all(void);
extern struct sched_domain_topology_level *get_sched_topology(void);
extern void set_ewma_act_ns(int cpu, u64 ewma_act_ns);
/*
 * Heartbeat-staleness check -- already EXPORT_SYMBOL'd (kernel/sched/
 * cputime.c), already used by IVH's own kernel-side gate chain. Live in the
 * sense that it is computable at any instant (including while the vCPU is
 * currently stolen), BUT tick-granular: rq->clock_preempt is refreshed only
 * by account_process_tick() (once per 4ms at CONFIG_HZ=250) and
 * rq->last_idle_tp only on idle transitions, so on a busy-but-healthy vCPU
 * this reads "preempted" for the last 2.5ms of every 4ms tick window (62.5%
 * false-positive duty cycle) -- confirmed 2026-07-14 as the cause of the
 * 154,160-backoff explosion when adaptive spinning polled it at high
 * frequency. Kept ONLY as the preempted_src=1 fallback/A-B path of
 * /proc/vcap_preempted below; the default path uses the KVM
 * steal_time.preempted bit instead, which has no tick dependence at all.
 */
extern int is_cpu_preempted(int cpunum);
/*
 * IVH inferred steal time (Plan 2, kernel/sched/core.c). get_real_steal() is
 * host ground truth ALWAYS -- unlike get_steal_and_preemptions(), which the
 * kernel.ivh_steal_source sysctl can switch over to the inferred value, and
 * which therefore cannot be used to validate the inferred value against.
 */
extern void get_inferred_steal(int cpunum, u64 *inferred, u64 *samples, u64 *skipped);
extern void get_real_steal(int cpunum, u64 *steal);

/* Global variables */
struct cpumask cpuset_array[NR_CPUS];
EXPORT_SYMBOL(cpuset_array);

/* Function declarations */
static ssize_t blank_write(struct file *file, const char __user *ubuf, size_t count, loff_t *ppos);
static ssize_t blank_read(struct file *file, char __user *ubuf, size_t count, loff_t *ppos);
static ssize_t get_info_read(struct file *file, char __user *ubuf, size_t count, loff_t *ppos);
static ssize_t capacity_write(struct file *file, const char __user *buffer, size_t count, loff_t *offset);
static ssize_t latency_write(struct file *file, const char __user *buffer, size_t count, loff_t *offset);
static ssize_t topology_write(struct file *file, const char __user *buffer, size_t count, loff_t *offset);
static ssize_t av_capacity_write(struct file *file, const char __user *buffer, size_t count, loff_t *offset);
static ssize_t act_write(struct file *file, const char __user *buffer, size_t count, loff_t *offset);
static void set_capacities(char *data);
static void set_av_capacity(char *data);
static void set_latencies(char *data);
static void set_topology(const char *data, size_t count);
static void set_act(char *data);

/* Function implementations */
static ssize_t blank_write(struct file *file, const char __user *ubuf, size_t count, loff_t *ppos)
{
    printk(KERN_DEBUG "write handler\n");
    return -1;
}

static ssize_t blank_read(struct file *file, char __user *ubuf, size_t count, loff_t *ppos)
{
    printk(KERN_DEBUG "read handler\n");
    return -1;
}

static void set_capacities(char *data)
{
    char *token, *cur;
    char *data_copy;
    int cpu_index = 0;
    long capacity_value;

    data_copy = kstrdup(data, GFP_KERNEL);
    if (!data_copy)
        return;

    cur = data_copy;
    while ((token = strsep(&cur, ";")) != NULL) {
        if (*token == '\0')
            continue;
        if (kstrtol(token, 10, &capacity_value) == 0) {
            printk("Capacity:%lu Cpu%d:", (unsigned long)capacity_value, cpu_index);
            set_custom_capacity((unsigned long)capacity_value, cpu_index);
            cpu_index++;
        }
    }
    kfree(data_copy);
}

static ssize_t capacity_write(struct file *file, const char __user *buffer, size_t count, loff_t *offset)
{
    char *input_buffer;
    ssize_t status = count;

    if (count > BUFSIZE) {
        count = BUFSIZE;
    }

    input_buffer = kmalloc(count + 1, GFP_KERNEL);
    if (!input_buffer)
        return -ENOMEM;

    if (copy_from_user(input_buffer, buffer, count)) {
        kfree(input_buffer);
        return -EFAULT;
    }

    input_buffer[count] = '\0';
    set_capacities(input_buffer);
    kfree(input_buffer);

    return status;
}

static void set_av_capacity(char *data)
{
    long capacity_value;

    if (kstrtol(data, 10, &capacity_value) == 0) {
        printk(KERN_INFO "Setting average capacity to: %ld\n", capacity_value);
        set_average_capacity_all((unsigned int)capacity_value);
    } else {
        printk(KERN_ERR "Invalid input for average capacity\n");
    }
}

static ssize_t av_capacity_write(struct file *file, const char __user *buffer, size_t count, loff_t *offset)
{
    char *input_buffer;
    ssize_t status = count;

    if (count > BUFSIZE) {
        count = BUFSIZE;
    }

    input_buffer = kmalloc(count + 1, GFP_KERNEL);
    if (!input_buffer)
        return -ENOMEM;

    if (copy_from_user(input_buffer, buffer, count)) {
        kfree(input_buffer);
        return -EFAULT;
    }

    input_buffer[count] = '\0';
    set_av_capacity(input_buffer);
    kfree(input_buffer);

    return status;
}

static void set_latencies(char *data)
{
    char *token, *cur;
    char *data_copy;
    int cpu_index = 0;
    long latency_value;

    data_copy = kstrdup(data, GFP_KERNEL);
    if (!data_copy)
        return;

    reset_max_latency(0);
    cur = data_copy;

    while ((token = strsep(&cur, ";")) != NULL) {
        if (*token == '\0')
            continue;

        if (kstrtol(token, 10, &latency_value) == 0) {
            printk("cpu at %d has latency: %llu", cpu_index, (unsigned long long)latency_value);
            set_avg_latency(cpu_index, (unsigned long long)latency_value);
            cpu_index++;
        }
    }
    kfree(data_copy);
}

static ssize_t latency_write(struct file *file, const char __user *buffer, size_t count, loff_t *offset)
{
    char *input_buffer;
    ssize_t status = count;

    if (count > BUFSIZE) {
        count = BUFSIZE;
    }

    input_buffer = kmalloc(count + 1, GFP_KERNEL);
    if (!input_buffer)
        return -ENOMEM;

    if (copy_from_user(input_buffer, buffer, count)) {
        kfree(input_buffer);
        return -EFAULT;
    }

    input_buffer[count] = '\0';
    set_latencies(input_buffer);
    kfree(input_buffer);

    return status;
}

static void set_topology(const char *data, size_t count)
{
    struct sched_domain_topology_level *topology = get_sched_topology();
    int sched_domain = 0;
    int cpu = 0;
    size_t bit_index = 0;
    static cpumask_t use_cpumask;  /* Static to avoid stack usage */
    int num_cpus;
    int comp_cpu;

    num_cpus = num_present_cpus();

    if (topology == NULL) {
        printk(KERN_WARNING "Failed to retrieve Scheduling Domain Topology.\n");
        return;
    }

    while (bit_index < count * 8 && sched_domain < MAX_TOPOLOGY_LEVEL) {
        cpumask_clear(&use_cpumask);

        /* Read bits for current CPU */
        for (comp_cpu = 0; comp_cpu < num_cpus && bit_index < count * 8; comp_cpu++, bit_index++) {
            if (test_bit(bit_index, (unsigned long *)data)) {
                cpumask_set_cpu(comp_cpu, &use_cpumask);
            }
        }

        if (sched_domain == 0) {
            cpumask_copy(&cpuset_array[cpu], &use_cpumask);
        }
        /* 6.17: mask() returns const — cast is intentional, we own this topology buffer */
        cpumask_copy((struct cpumask *)topology[sched_domain].mask(cpu), &use_cpumask);

        cpu++;

        /* If we've processed all CPUs for this level or reached end of data */
        if (cpu == num_cpus || bit_index >= count * 8) {
            sched_domain++;
            cpu = 0;
        }
    }

    set_live_topology(topology);
}

static ssize_t topology_write(struct file *file, const char __user *buffer, size_t count, loff_t *offset)
{
    char *input_buffer;
    ssize_t status = count;

    input_buffer = kmalloc(count, GFP_KERNEL);
    if (!input_buffer)
        return -ENOMEM;

    if (copy_from_user(input_buffer, buffer, count)) {
        kfree(input_buffer);
        return -EFAULT;
    }

    set_topology(input_buffer, count);
    kfree(input_buffer);

    return status;
}

static ssize_t get_info_read(struct file *file, char __user *ubuf, size_t count, loff_t *ppos)
{
    /*
     * Fable investigation, 2026-07-13: was `static char buf[BUFSIZE]`, a
     * single buffer shared and written concurrently by every reader with no
     * synchronization at all. Harmless when only the lock holder reads this
     * (serialized by the benchmark lock itself), but adaptive-spin waiters
     * hammer this at tens of kHz aggregate, so a torn read here corrupts the
     * host_preempted metric's own boundary snapshots. On-stack (not
     * kmalloc'd -- SLUB's slowpath takes a real spinlock, which would be a
     * fresh incidental ivh_pre_lock() trigger for any PF_IVH_ELIGIBLE task,
     * the exact bug class fixed earlier tonight for fd open/close).
     */
    /*
     * FORMAT IS FROZEN AT EXACTLY 4 LINES PER CPU. /proc/vcap_info has an
     * external consumer we don't control the source of (`vcap`, a compiled
     * C++ binary at /home/nick/vsched_main/vcapacity/vcap) with a hardcoded
     * 4-line-per-CPU parser; adding a 5th field (tried 2026-07-13, with
     * is_cpu_preempted()) crashed it with std::invalid_argument from
     * stoull. Any new per-CPU signal goes in its own proc file -- see
     * /proc/vcap_preempted below.
     */
    char buf[2048];
    int len = 0;
    int cpu;
    u64 preempt, steal_time, max_latency;

    if (*ppos > 0)
        return 0;

    for_each_online_cpu(cpu) {
        get_steal_and_preemptions(cpu, &preempt, &steal_time);
        get_max_latency(cpu, &max_latency);

        len += snprintf(buf + len, sizeof(buf) - len,
                       "CPU %d:\n%llu\n%llu\n%llu\n",
                       cpu, preempt, steal_time, max_latency);

        if (len >= sizeof(buf) - 1) {
            break;
        }
    }

    if (count < len)
        return 0;

    if (copy_to_user(ubuf, buf, len))
        return -EFAULT;

    *ppos = len;
    return len;
}

/*
 * /proc/vcap_preempted -- live per-vCPU "is it host-preempted RIGHT NOW"
 * bit, one ASCII byte per CPU ('0'/'1', byte offset == CPU number) plus a
 * trailing newline. Deliberately a NEW, SEPARATE file: /proc/vcap_info's
 * format is frozen (see get_info_read()).
 *
 * Read protocol (designed for a tight userspace spin loop):
 *   pread(fd, &c, 1, holder_cpu)   -- one byte, one pv-op call, no parsing.
 * Only the requested byte range is computed, so a 1-byte pread costs exactly
 * one percpu-memory read, not nr_cpus of them. `cat` still works for humans
 * (sequential reads walk *ppos to EOF).
 *
 * Signal source (preempted_src=0, default): the KVM steal_time.preempted
 * byte, read via pv_ops.lock.vcpu_is_preempted. The HOST sets
 * KVM_VCPU_PREEMPTED at the instant it involuntarily schedules the vCPU
 * thread out (kvm_arch_vcpu_put -> kvm_steal_time_set_preempted) and clears
 * it on the next VM-entry (record_steal_time), so the bit is 1 for exactly
 * the window the vCPU is actually stolen -- edge-precise, zero guest-tick
 * dependence, readable from any other vCPU since steal_time lives in guest
 * memory. This is the same signal the mainline kernel itself trusts for
 * exactly this decision (owner_on_cpu() spin-wait breakout in mutex/rwsem/
 * osq via vcpu_is_preempted()). A vCPU idling in HLT is NOT marked
 * preempted (host only sets the bit on involuntary preemption), so there is
 * no idle false-positive either.
 *
 * NOTE: CONFIG_PARAVIRT_SPINLOCKS is NOT set in this kernel, so the
 * vcpu_is_preempted() inline wrapper compiles to the generic return-false
 * stub -- but kvm_guest_init() installs the real accessor into
 * pv_ops.lock.vcpu_is_preempted whenever KVM_FEATURE_STEAL_TIME is present
 * (arch/x86/kernel/kvm.c, NOT guarded by PARAVIRT_SPINLOCKS), and pv_ops is
 * EXPORT_SYMBOL. So we call through the op ourselves. The op is a
 * callee-save thunk (preserves all regs beyond the C ABI's requirements),
 * which is strictly safe to call as a normal C function pointer.
 * Without PARAVIRT_SPINLOCKS the static initializer leaves the op NULL on
 * non-KVM boots -- guarded below, falling back to is_cpu_preempted().
 *
 * preempted_src=1 (writable at runtime via
 * /sys/module/vsched_module/parameters/preempted_src): serve
 * is_cpu_preempted() heartbeat staleness through the IDENTICAL interface
 * instead, so the tick-granularity noise result (154k backoffs, 2026-07-14)
 * can be reproduced/A-B'd with no userspace or module rebuild.
 */
static int preempted_src; /* 0 = KVM steal_time.preempted (live), 1 = is_cpu_preempted() heartbeat */
module_param(preempted_src, int, 0644);
MODULE_PARM_DESC(preempted_src,
                 "vcap_preempted source: 0=KVM steal_time.preempted bit (default), 1=is_cpu_preempted() heartbeat");

#define VCAP_PREEMPTED_MAX_CPUS 256 /* matches VCAP_MAX_CPUS in NHextend.c */

static bool vcap_cpu_preempted_now(int cpu)
{
    bool (*fn)(long) = (void *)pv_ops.lock.vcpu_is_preempted.func;

    if (preempted_src == 0 && fn)
        return fn(cpu);
    return is_cpu_preempted(cpu) != 0;
}

static ssize_t preempted_read(struct file *file, char __user *ubuf,
                              size_t count, loff_t *ppos)
{
    char kbuf[VCAP_PREEMPTED_MAX_CPUS + 1];
    int ncpu = min_t(int, nr_cpu_ids, VCAP_PREEMPTED_MAX_CPUS);
    int total = ncpu + 1; /* +1: trailing '\n' */
    loff_t pos = *ppos;
    size_t i;

    if (pos < 0)
        return -EINVAL;
    if (pos >= total || count == 0)
        return 0;
    if (count > (size_t)(total - pos))
        count = total - pos;

    /* Fill only the requested window: the hot path -- a waiter's 1-byte
     * pread at offset holder_cpu -- costs exactly one preempted-bit read. */
    for (i = 0; i < count; i++) {
        int cpu = (int)(pos + i);

        if (cpu < ncpu)
            kbuf[i] = (cpu_online(cpu) && vcap_cpu_preempted_now(cpu))
                      ? '1' : '0';
        else
            kbuf[i] = '\n';
    }

    if (copy_to_user(ubuf, kbuf, count))
        return -EFAULT;

    *ppos = pos + count;
    return count;
}

/*
 * /proc/vcap_steal_compare -- side-by-side debug comparator for IVH's
 * REF_TSC-inferred steal time (Plan 2, kernel/sched/core.c:
 * ivh_ref_accumulate() / get_inferred_steal() / get_real_steal()) against
 * paravirt_steal_clock(), the real host-reported number
 * get_steal_and_preemptions() has always returned.
 *
 * Deliberately a NEW, SEPARATE file, for the same reason /proc/vcap_preempted
 * is one: /proc/vcap_info's 4-lines-per-CPU wire format is frozen (see
 * get_info_read() above) and a 5th field crashed `vcap`'s hardcoded parser
 * with std::invalid_argument on 2026-07-13.
 *
 * get_real_steal() exists as a distinct accessor from
 * get_steal_and_preemptions() specifically so this file keeps working after
 * kernel.ivh_steal_source is ever flipped to 1: at that point
 * get_steal_and_preemptions() itself starts returning the INFERRED number,
 * and a comparator built on it would be comparing the inferred value against
 * itself (a permanent delta_ppm of 0) rather than against host truth.
 *
 * Format: one line per online CPU, six space-separated ASCII fields, chosen
 * so a shell one-liner or awk can diff them without a real parser --
 * intentionally NOT frozen the way /proc/vcap_info is, since this file has
 * no consumer but a human or an ad hoc script:
 *
 *   # cpu real_steal_ns inferred_steal_ns samples skipped delta_ppm
 *   0 179645000000 179402118000 4821990 12 -1352
 *
 * delta_ppm = signed (inferred - real) * 1e6 / real. ivh_ref_accumulate()'s
 * two clamps (see its comment in core.c) bias the inferred value low by
 * construction, so delta_ppm is EXPECTED to sit persistently negative; a
 * persistently POSITIVE delta means idle is being under-subtracted somewhere
 * and is a bug, not noise. real_steal_ns == 0 prints delta_ppm as 0 rather
 * than dividing by zero (early boot / a CPU with no steal yet).
 *
 * mul_u64_u64_div_u64() (linux/math64.h) rather than a plain
 * (diff * 1000000) / real: both real_steal_ns and inferred_steal_ns are
 * cumulative-since-boot nanosecond counters, so a naive multiply-then-divide
 * overflows a signed 64-bit intermediate well within plausible uptimes
 * (~1e14 ns * 1e6 already exceeds S64_MAX). mul_u64_u64_div_u64() does the
 * multiply in a 128-bit intermediate before dividing, so it is exact for any
 * pair of u64 operands; the sign is peeled off and reapplied around it.
 *
 * Not on the pread-single-byte-per-CPU protocol preempted_read() uses --
 * this is a diagnostic file, not a hot-path poll target, so a plain
 * kmalloc'd buffer and a single-shot read (same *ppos > 0 => 0 convention as
 * get_info_read() above) is the right amount of mechanism. kmalloc'd, not
 * on-stack, because unlike get_info_read()'s frozen and bounded format this
 * one scales with nr_cpu_ids and a few hundred CPUs' worth of six wide u64
 * fields would be a large on-stack buffer.
 */
static ssize_t steal_compare_read(struct file *file, char __user *ubuf,
                                  size_t count, loff_t *ppos)
{
    char *buf;
    size_t bufsize;
    int len = 0;
    int cpu;
    u64 real_steal, inferred, samples, skipped;
    s64 diff, ppm;
    ssize_t ret;

    if (*ppos > 0)
        return 0;

    /* Generous per-line estimate: "cpu real inferred samples skipped ppm\n"
     * with every field at its worst-case (20-digit u64 / sign) width. */
    bufsize = (size_t)nr_cpu_ids * 160 + 128;
    buf = kmalloc(bufsize, GFP_KERNEL);
    if (!buf)
        return -ENOMEM;

    len += scnprintf(buf + len, bufsize - len,
                     "# cpu real_steal_ns inferred_steal_ns samples skipped delta_ppm\n");

    for_each_online_cpu(cpu) {
        get_real_steal(cpu, &real_steal);
        get_inferred_steal(cpu, &inferred, &samples, &skipped);

        diff = (s64)inferred - (s64)real_steal;
        if (real_steal == 0) {
            ppm = 0;
        } else if (diff >= 0) {
            ppm = (s64)mul_u64_u64_div_u64((u64)diff, 1000000ULL, real_steal);
        } else {
            ppm = -(s64)mul_u64_u64_div_u64((u64)(-diff), 1000000ULL, real_steal);
        }

        len += scnprintf(buf + len, bufsize - len,
                         "%d %llu %llu %llu %llu %lld\n",
                         cpu, real_steal, inferred, samples, skipped, ppm);

        if (len >= bufsize - 1)
            break;
    }

    if (count < len) {
        kfree(buf);
        return 0;
    }

    ret = copy_to_user(ubuf, buf, len) ? -EFAULT : len;
    kfree(buf);
    if (ret > 0)
        *ppos = ret;
    return ret;
}

static void set_act(char *data)
{
    char *token, *cur;
    char *data_copy;
    int cpu_index = 0;
    u64 act_value;

    data_copy = kstrdup(data, GFP_KERNEL);
    if (!data_copy)
        return;

    cur = data_copy;
    while ((token = strsep(&cur, ";")) != NULL) {
        if (*token == '\0')
            continue;
        if (kstrtoull(token, 10, &act_value) == 0) {
            set_ewma_act_ns(cpu_index, act_value);
            cpu_index++;
        }
    }
    kfree(data_copy);
}

static ssize_t act_write(struct file *file, const char __user *buffer, size_t count, loff_t *offset)
{
    char *input_buffer;
    ssize_t status = count;

    if (count > BUFSIZE)
        count = BUFSIZE;

    input_buffer = kmalloc(count + 1, GFP_KERNEL);
    if (!input_buffer)
        return -ENOMEM;

    if (copy_from_user(input_buffer, buffer, count)) {
        kfree(input_buffer);
        return -EFAULT;
    }

    input_buffer[count] = '\0';
    set_act(input_buffer);
    kfree(input_buffer);
    return status;
}

static const struct proc_ops cust_act_ops = {
    .proc_read  = blank_read,
    .proc_write = act_write,
};

/*
 * Like get_information_ops: no .proc_lseek, so proc_reg_open() clears
 * FMODE_LSEEK but leaves FMODE_PREAD set -- pread(fd, buf, n, cpu) works on
 * a persistent fd with no lseek, the exact pattern NHextend.c already uses
 * for /proc/vcap_info (verified empirically there, see the comment above
 * read_vcap_steal()).
 */
static const struct proc_ops preempted_ops = {
    .proc_read  = preempted_read,
    .proc_write = blank_write,
};

static const struct proc_ops steal_compare_ops = {
    .proc_read  = steal_compare_read,
    .proc_write = blank_write,
};

static const struct proc_ops get_information_ops = {
    .proc_read = get_info_read,
    .proc_write = blank_write,
};

static const struct proc_ops cust_capacity_ops = {
    .proc_read = blank_read,
    .proc_write = capacity_write,
};

static const struct proc_ops cust_av_capacity_ops = {
    .proc_read = blank_read,
    .proc_write = av_capacity_write,
};

static const struct proc_ops cust_latency_ops = {
    .proc_read = blank_read,
    .proc_write = latency_write,
};

static const struct proc_ops cust_topo_ops = {
    .proc_read = blank_read,
    .proc_write = topology_write,
};

static int vsched_init(void)
{
    get_info_ent = proc_create("vcap_info", 0666, NULL, &get_information_ops);
    capacity_ent = proc_create("vcapacity_write", 0660, NULL, &cust_capacity_ops);
    latency_ent = proc_create("vlatency_write", 0660, NULL, &cust_latency_ops);
    topo_ent = proc_create("vtopology_write", 0660, NULL, &cust_topo_ops);
    av_capacity_ent = proc_create("vav_capacity_write", 0660, NULL, &cust_av_capacity_ops);
    act_ent = proc_create("vact_write", 0660, NULL, &cust_act_ops);
    preempted_ent = proc_create("vcap_preempted", 0444, NULL, &preempted_ops);
    steal_compare_ent = proc_create("vcap_steal_compare", 0444, NULL, &steal_compare_ops);

    if (!get_info_ent || !capacity_ent || !topo_ent || !latency_ent || !av_capacity_ent || !act_ent || !preempted_ent || !steal_compare_ent) {
        proc_remove(get_info_ent);
        proc_remove(capacity_ent);
        proc_remove(latency_ent);
        proc_remove(topo_ent);
        proc_remove(av_capacity_ent);
        proc_remove(act_ent);
        proc_remove(preempted_ent);
        proc_remove(steal_compare_ent);

        printk(KERN_ALERT "Error: Could not successfully initialize vSched's kernel modules - check your kernel version\n");
        return -ENOMEM;
    }

    printk(KERN_ALERT "Successfully initialized vSched's Kernel modules (vcap_preempted source: %s)\n",
           pv_ops.lock.vcpu_is_preempted.func ? "KVM steal_time.preempted bit"
                                              : "is_cpu_preempted() heartbeat fallback (no KVM pv op)");
    return 0;
}

static void vsched_cleanup(void)
{
    proc_remove(av_capacity_ent);
    proc_remove(act_ent);
    proc_remove(get_info_ent);
    proc_remove(capacity_ent);
    proc_remove(latency_ent);
    proc_remove(topo_ent);
    proc_remove(preempted_ent);
    proc_remove(steal_compare_ent);
}

module_init(vsched_init);
module_exit(vsched_cleanup);
