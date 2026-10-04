#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>

#ifdef ENABLE_TRACEFS
#include <tracefs.h>
#else
static inline void tracefs_printf(void *inst, const char *fmt, ...) { }
static inline void tracefs_print_init(void *inst) { }
#endif

#include <time.h>
#include <sys/rseq.h>
#include <linux/types.h>
#include <asm/byteorder.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sched.h>
#include <fcntl.h>

/*
 * Host-level steal-time ground truth, read from /proc/vcap_info
 * (custom_modules/vsched_module.c, get_info_read() -> get_steal_and_preemptions()
 * -> paravirt_steal_clock()). Per-CPU raw cumulative steal ns since boot, driven
 * directly by the KVM steal-time MSR, independent of guest scheduling entirely --
 * unlike the wall-clock-vs-CLOCK_THREAD_CPUTIME_ID gap below, which only catches
 * guest-internal context switches, not real host-level vCPU steals.
 */
#define VCAP_MAX_CPUS 256

/*
 * Confirmed live this session: get_info_read() (vsched_module.c) blocks
 * re-read() via *ppos>0 and has no .proc_lseek (lseek() fails ESPIPE) --
 * but proc_reg_open() only clears FMODE_LSEEK when .proc_lseek is absent
 * (fs/proc/inode.c), leaving FMODE_PREAD set, and ksys_pread64() checks
 * only FMODE_PREAD and passes a *local* pos, never touching f_pos
 * (fs/read_write.c) -- so pread(fd, buf, sz, 0) re-triggers the dump every
 * call without the kernel ever seeing *ppos > 0. Verified empirically:
 * three consecutive pread(fd, ..., 0) on one fd each returned a fresh dump.
 * This removes the open()+close() pair (alloc_fd/file_close_fd) that
 * accounted for ~500,000 of the migrations attributed to
 * bpf_sched_pre_lock_migrate() in one 5s adaptive-spin run tonight --
 * every incidental fd-table lock touch was itself an IVH trigger for any
 * PF_IVH_ELIGIBLE task, unrelated to the lock actually being tested (see
 * tools/bpf/docs -- ask about the "ivh_hotlock_observe migration explosion"
 * investigation if this comment needs more context later). Thread-local fd:
 * pread() never touches f_pos, so a shared fd would technically be safe
 * too, but __thread avoids that argument entirely and matches rseq_map's
 * existing thread-local pattern below.
 */
static __thread int vcap_steal_fd = -1;

/*
 * 2026-07-13: reverted back to the 4-field /proc/vcap_info format.
 * Attempted adding a 5th field (is_cpu_preempted() per CPU, for a live
 * rather than post-hoc adaptive-spinning health check) directly to this
 * shared proc file, but /proc/vcap_info is also consumed by `vcap` (a
 * separate, independent binary at /home/nick/vsched_main/vcapacity/vcap)
 * which has its own hardcoded 4-line-per-CPU parser -- adding a field broke
 * vcap outright (std::invalid_argument from stoull, misaligned field
 * parsing). Reverted vsched_module.c's get_info_read() back to 4 fields.
 * Any live-preempted-bit signal needs its own, separate proc file/interface
 * next time, not a change to this one -- do not repeat this mistake.
 *
 * 2026-07-14: that separate interface now exists -- /proc/vcap_preempted
 * (vsched_module.c, preempted_read()), consumed by holder_vcpu_preempted()
 * below. This file (/proc/vcap_info) stays frozen at 4 fields per CPU.
 */
static int read_vcap_steal(unsigned long long *steal_out)
{
        char buf[8192];
        char *saveptr, *tok;
        ssize_t n;
        int cpu = -1, field = 0;

        if (vcap_steal_fd < 0) {
                vcap_steal_fd = open("/proc/vcap_info", O_RDONLY);
                if (vcap_steal_fd < 0)
                        return -1;
        }
        n = pread(vcap_steal_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0)
                return -1;
        buf[n] = '\0';

        tok = strtok_r(buf, "\n", &saveptr);
        while (tok) {
                if (field == 0) {
                        sscanf(tok, "CPU %d:", &cpu);
                } else if (field == 2 && steal_out && cpu >= 0 && cpu < VCAP_MAX_CPUS) {
                        steal_out[cpu] = strtoull(tok, NULL, 10);
                }
                field = (field + 1) % 4;
                tok = strtok_r(NULL, "\n", &saveptr);
        }
        return 0;
}

/*
 * 2026-07-14: live per-vCPU host-preemption bit, from /proc/vcap_preempted
 * (vsched_module.c, preempted_read()) -- one ASCII byte per CPU, byte
 * offset == CPU number, so pread(fd, &c, 1, holder_cpu) fetches exactly the
 * holder's bit: one syscall, one KVM steal_time.preempted read module-side,
 * zero parsing. Strictly cheaper per check than read_vcap_steal(), which
 * dumps and strtok-parses the whole multi-KB 4-line-per-CPU text file.
 *
 * Unlike is_cpu_preempted() (tick-granular heartbeat: reads "preempted" for
 * the last 2.5ms of every 4ms tick window on a busy-but-healthy vCPU --
 * root cause of the 154,160-backoff explosion, 2026-07-14), the KVM
 * preempted byte is set by the HOST at the instant the vCPU is
 * involuntarily scheduled out and cleared at its next VM-entry: 1 for
 * exactly the stolen window, edge-precise, no guest-tick dependence, no
 * idle false-positives (HLT does not set it).
 *
 * Same thread-local persistent-fd + pread() pattern as vcap_steal_fd above
 * (and the same FMODE_PREAD-without-FMODE_LSEEK proc behavior, verified
 * there). Returns 1 = holder's vCPU is host-preempted right now, 0 = it is
 * running (or offline), -1 = interface unavailable (module not loaded) --
 * callers treat anything but 1 as "healthy", degrading to plain spinning.
 */
static __thread int vcap_preempted_fd = -1;

static int holder_vcpu_preempted(int cpu)
{
        char c;

        if (vcap_preempted_fd < 0) {
                vcap_preempted_fd = open("/proc/vcap_preempted", O_RDONLY);
                if (vcap_preempted_fd < 0)
                        return -1;
        }
        if (pread(vcap_preempted_fd, &c, 1, cpu) != 1)
                return -1;
        return c == '1';
}

/*
 * Pre-lock migration trigger — call BEFORE attempting grab_lock(), not after.
 * Mirrors ivh_pre_lock() in the kernel spinlock path: if the current vCPU is
 * throttled, the task migrates itself synchronously to a healthy vCPU so the
 * lock is acquired there.  No-op when IVH is not loaded (static key in kernel).
 */
#ifndef __NR_ivh_cs_enter
#define __NR_ivh_cs_enter 470
#endif
/*
 * lock_addr/waiters/verdict are IGNORED by the running kernel --
 * sys_ivh_cs_enter is SYSCALL_DEFINE0 (verified: 0 occurrences of Hotlock
 * anywhere in the committed kernel this is built against, see the
 * Checkpoint-H audit in tools/bpf/docs/ivh_build_and_evaluation_plan_2026-07-11.md).
 * Passing them costs nothing (extra unused registers on the syscall ABI)
 * and lets an external kprobe (ivh_hotlock_observe.bpf.c) read the verdict
 * this shim computed, entirely for observation -- this call NEVER skips or
 * alters behavior based on the verdict itself. See ivh_hotlock_is_hot().
 */
static inline void ivh_cs_enter(unsigned long lock_addr, unsigned long waiters,
				 unsigned long verdict)
{
	syscall(__NR_ivh_cs_enter, lock_addr, waiters, verdict);
}

/* Updated version of rseq structure with cr_counter, wait_counter, and timing fields */
struct rseq_abi {
        __u32 cpu_id_start;
        __u32 cpu_id;
        __u64 rseq_cs;
        __u32 flags;
        __u32 node_id;
        __u32 mm_cid;
        __u32 cr_counter;           /* +28: lockholder signal: bits [31:2] = CS nesting depth */
        __u32 wait_counter;         /* +32: waiter signal: bits [31:2] = spin-wait nesting depth */
        __u32 _pad0;                /* +36: alignment padding */
        __u64 last_cs_overall_ns;   /* +40: wall-clock duration of most recent CS (ns) */
        __u64 last_cs_active_ns;    /* +48: unused here; reserved for on-CPU CS time */
        __u64 last_wait_overall_ns; /* +56: wall-clock wait before most recent acquire (ns) */
} __attribute__((aligned(4 * sizeof(__u64))));

static bool no_rseq;
static bool extend_wait;
static bool no_pin;

static int loop_spin = 600000;
static int sleep_mode = 2; /* 0=nanosleep, 1=busy-spin same duration, 2=no wait at all */
static int num_threads = -1;
static int num_busy_threads = 0;

/*
 * Checkpoint H (see ivh_build_and_evaluation_plan_2026-07-11.md): a userspace
 * replica of the kernel's Hotlock formula (kernel/locking/spinlock.c:263-320),
 * ported field-for-field so its classification behavior can be validated
 * before any kernel-side Hotlock code exists in a bootable kernel (confirmed
 * this session: Hotlock is 100% working-tree-only, zero occurrences in the
 * committed/running kernel). This is OBSERVATION ONLY -- it never gates
 * ivh_cs_enter() or skips anything. There is exactly one lock in this
 * benchmark, so a single global waiters/history pair (not a table) is
 * sufficient -- the real kernel table exists because it multiplexes many
 * locks, which doesn't apply here.
 */
#define IVH_HOTLOCK_SCALE   10                          /* fixed point: 1<<10 == "1.0" */
#define IVH_HOTLOCK_HALF    (1 << (IVH_HOTLOCK_SCALE - 1))
#define IVH_HOTLOCK_EWMA_K  3                            /* matches kernel default */

static volatile int ivh_hotlock_waiters = 0;
static volatile int ivh_hotlock_history = 0;

static inline void ivh_hotlock_note_enter(void)
{
	__sync_fetch_and_add(&ivh_hotlock_waiters, 1);
}

static inline void ivh_hotlock_note_exit(void)
{
	__sync_fetch_and_sub(&ivh_hotlock_waiters, 1);
}

/* sample = contended ? "1.0" : "0.0"; new = old + (sample - old) >> k -- verbatim
 * port of ivh_hotlock_update()'s formula, spinlock.c:266,309. */
static inline void ivh_hotlock_update(int contended)
{
	int sample = contended ? (1 << IVH_HOTLOCK_SCALE) : 0;
	int old = ivh_hotlock_history;
	int new = old + ((sample - old) >> IVH_HOTLOCK_EWMA_K);

	ivh_hotlock_history = new;
}

/* is_hot = contended || history > HALF -- verbatim port of spinlock.c:518. */
static inline int ivh_hotlock_is_hot(int waiters_now)
{
	return (waiters_now > 0) || (ivh_hotlock_history > IVH_HOTLOCK_HALF);
}

#define rmb() asm volatile ("lfence" ::: "memory")
#define wmb() asm volatile ("sfence" ::: "memory")

static pthread_barrier_t pbarrier;

static __thread struct rseq_abi *rseq_map;

static void register_rseq(void)
{
        int ret;

        ret = syscall(__NR_rseq, rseq_map, sizeof(struct rseq_abi), 0, 0x53053053);
        if (ret == 0)
                return;

        if (errno == EINVAL) {
                /* Already registered with the same struct — nothing to do */
                return;
        }

        if (errno == EBUSY) {
                /* Registered with a different size — unregister then re-register */
                ret = syscall(__NR_rseq, rseq_map, sizeof(struct rseq_abi),
                              RSEQ_FLAG_UNREGISTER, 0x53053053);
                if (ret < 0) {
                        fprintf(stderr, "rseq unregister failed: %m\n");
                        return;
                }
                ret = syscall(__NR_rseq, rseq_map, sizeof(struct rseq_abi), 0, 0x53053053);
                if (ret < 0) {
                        fprintf(stderr, "rseq re-register failed: %m\n");
                        return;
                }
                return;
        }

        fprintf(stderr, "rseq register warning: %m\n");
}

static void init_extend_map(void)
{
        if (no_rseq)
                return;

        rseq_map = (void *)__builtin_thread_pointer() + __rseq_offset;
        register_rseq();
}

struct data;

struct thread_data {
        unsigned long long                      x_count;
        unsigned long long                      total;
        unsigned long long                      max;
        unsigned long long                      min;
        unsigned long long                      total_wait;
        unsigned long long                      max_wait;
        unsigned long long                      min_wait;
        unsigned long long                      contention;
        unsigned long long                      extended;
        unsigned long long                      last_cs_ns;
        unsigned long long                      last_cs_active_ns;
        unsigned long long                      last_wait_ns;
        unsigned long long                      max_cs_ns;
        unsigned long long                      max_cs_active_ns;
        unsigned long long                      sum_cs_ns;
        unsigned long long                      sum_cs_active_ns;
        unsigned long long                      cs_count;
        /* IVH migration tracking */
        unsigned long long                      migration_count;
        unsigned long long                      sum_migration_ns;
        unsigned long long                      max_migration_ns;
        unsigned long long                      slow_migration_count; /* ivh_cs_enter() > 1ms */
        /* CS preemption tracking: how often was the lock holder preempted */
        unsigned long long                      cs_preempted_count;  /* CS cycles with >100us off-CPU */
        /* Host-level steal-time tracking (ground truth, see read_vcap_steal) */
        unsigned long long                      host_preempted_count;
        unsigned long long                      host_preempted_migrated_count;
        /* Stage A4 adaptive-spin prototype (see ivh_build_and_evaluation_plan_2026-07-11.md) */
        unsigned long long                      adaptive_backoffs;
        unsigned long long                      backoff_wait_ns; /* total time spent backed off (nominal tpause ns) */
        unsigned long long                      wait_wall_ns;   /* wall-clock time spent waiting */
        unsigned long long                      wait_oncpu_ns;  /* on-CPU time spent waiting */
        /* Correlation check: does a backoff during THIS wait predict host_preempted
         * on the CS that follows it -- tests whether the nanosleep()-driven wakeup
         * itself (ordinary, non-IVH-aware CFS placement) is landing the thread
         * somewhere worse right before the CS starts. */
        unsigned long long                      cs_after_backoff_count;
        unsigned long long                      cs_after_backoff_preempted_count;
        unsigned long long                      cs_no_backoff_count;
        unsigned long long                      cs_no_backoff_preempted_count;
        struct data                             *data;
        int                                     cpu;
};

struct data {
        unsigned long long              x;
        unsigned long                   lock;
        struct thread_data              *tdata;
        bool                            done;
};

/*
 * Stage A4 (ivh_build_and_evaluation_plan_2026-07-11.md): adaptive-spinning
 * prototype. The lock word encodes the holder's CPU (0 = free, else
 * holder_cpu+1 -- see grab_lock()'s cmpxchg calls), so a waiter can look up
 * *that specific CPU's* health via the same read_vcap_steal() ground truth
 * already used for the host_preempted metric, instead of spinning blind.
 *
 * This deliberately reuses the existing /proc/vcap_info syscall path rather
 * than a new zero-syscall shared page (Stage A1, not attempted tonight --
 * see the report) -- so it is NOT the "free" version described in the plan,
 * but it is a complete, correctly-scoped prototype of the mechanism itself,
 * and the health check is throttled (see ADAPTIVE_SPIN_BUDGET below)
 * specifically so its own syscall cost doesn't dominate a short spin.
 */
/*
 * 2026-07-14: three-way mode switch so old and new designs stay A/B-able:
 *   0 = plain spin (baseline)
 *   1 = steal-delta backoff (old design: /proc/vcap_info cumulative-steal
 *       snapshot pairs, 100us fresh-accumulation floor, holder-CPU-tagged.
 *       Post-hoc by construction: the steal counter is only updated at
 *       VM-entry, so it can only report a preemption AFTER it ended.)
 *   2 = live holder bit (new design: /proc/vcap_preempted, the KVM
 *       steal_time.preempted byte -- 1 during exactly the window the
 *       holder's vCPU is host-stolen. See holder_vcpu_preempted().)
 */
static int adaptive_spin_enabled = 2; /* 0=plain spin, 1=steal-delta backoff, 2=live holder-preempted bit */
static int wait_time_track_enabled = 0; /* 0 for clean host_preempted runs -- see comment at call site */
static int backoff_recheck_enabled = 1; /* isolation test: does the post-backoff ivh_cs_enter() re-check itself drive up host_preempted, simply by ~doubling the legitimate trigger rate? */
#define ADAPTIVE_SPIN_BUDGET 2000 /* plain-spin iterations between holder-health checks */

/*
 * Mode-2 backoff shape: instead of one fixed 50us wait per trigger, wait in
 * short tpause slices and re-check the live bit between slices, so total
 * backoff tracks the ACTUAL remaining preemption -- a micro-preemption
 * (host tick) costs one 10us slice, a multi-ms steal is ridden out up to
 * the cap. The cap bounds the damage of a stale holder identity: with -n
 * (unpinned) threads the holder can be guest-migrated mid-CS, leaving the
 * lock word pointing at a CPU the holder no longer runs on; the lock-word
 * re-check below can't see that, the cap can.
 */
#define ADAPTIVE_BACKOFF_CHUNK_NS 10000ULL  /* 10us tpause slice between live-bit re-checks */
#define ADAPTIVE_BACKOFF_MAX_NS  200000ULL  /* per-episode cap (stale-holder / migrated-holder guard) */

/*
 * Fable investigation, tonight: nanosleep()-based backoff is not
 * preemption-neutral by construction, regardless of which syscall
 * implements it -- it (a) HLTs the waiter's own vCPU, inviting the host to
 * steal it right at the wake-to-CS boundary the host_preempted metric
 * measures (cost guaranteed, benefit -- freeing the pCPU for the holder --
 * undirected/unverifiable from userspace), (b) hands wake-up placement to
 * ordinary CFS, which *prefers idle CPUs* -- anti-correlated with health,
 * since an idle-looking vCPU is exactly the kind the host is most likely to
 * have already stolen, and (c) every syscall in the path is an incidental
 * IVH trigger while PF_IVH_ELIGIBLE stays process-wide (the same class as
 * the open/close problem fixed earlier tonight, just a smaller instance).
 *
 * Fix: replace the sleep with `tpause` (confirmed available -- all 16 CPUs
 * report `waitpkg` in /proc/cpuinfo, confirmed compiles/runs with
 * -mwaitpkg) -- a timed hardware wait that never leaves TASK_RUNNING, never
 * syscalls, never triggers a wake-placement decision, and never gives the
 * host a vCPU-idle window to steal. This satisfies "must not increase the
 * preempt count" by construction rather than by tuning: there is no
 * scheduling event of any kind for it to land badly from.
 */
#include <immintrin.h>

static unsigned long long tsc_per_ns_x1000 = 3000; /* calibrated at startup, see calibrate_tsc() */

static void calibrate_tsc(void)
{
        struct timespec ts0, ts1;
        unsigned long long tsc0, tsc1, wall_ns;

        clock_gettime(CLOCK_MONOTONIC, &ts0);
        tsc0 = __rdtsc();
        struct timespec sleep_ts = { .tv_sec = 0, .tv_nsec = 20000000 }; /* 20ms, one-time startup cost */
        nanosleep(&sleep_ts, NULL);
        clock_gettime(CLOCK_MONOTONIC, &ts1);
        tsc1 = __rdtsc();

        wall_ns = (unsigned long long)(ts1.tv_sec - ts0.tv_sec) * 1000000000ULL
                  + (ts1.tv_nsec - ts0.tv_nsec);
        if (wall_ns > 0)
                tsc_per_ns_x1000 = (tsc1 - tsc0) * 1000ULL / wall_ns;
}

/* Timed hardware wait for approximately `ns` nanoseconds. IA32_UMWAIT_CONTROL's
 * max_time (confirmed 100000 TSC cycles on this system, ~30-40us) caps a single
 * _tpause() call short of a 50us target, so loop until the real deadline. */
static void tpause_wait_ns(unsigned long long ns)
{
        unsigned long long deadline = __rdtsc() + (ns * tsc_per_ns_x1000) / 1000ULL;

        while (__rdtsc() < deadline)
                _tpause(0 /* C0.2, fast wake */, deadline);
}

/*
 * Called from inside a busy-wait loop body on every iteration; only actually
 * does anything (a real check, costing one syscall) once per
 * ADAPTIVE_SPIN_BUDGET iterations -- the common case (lock free shortly, or
 * holder healthy) never pays for a syscall at all.
 *
 * 2026-07-13: attempted swapping this for a live is_cpu_preempted()-based
 * check (exposed via a 5th /proc/vcap_info field) -- reverted, the 5th
 * field broke `vcap`'s own parser (see the note above read_vcap_steal()).
 * 2026-07-14: root-caused the 154,160-backoff explosion that swap produced
 * anyway: is_cpu_preempted() is tick-granular (clock_preempt refreshed only
 * by account_process_tick(), 4ms at HZ=250, vs its fixed 1.5ms staleness
 * floor), so a busy, healthy holder reads "preempted" 62.5% of the time.
 * Mode 2 below uses the KVM steal_time.preempted byte instead, via the new
 * separate /proc/vcap_preempted -- live like the heartbeat, edge-precise
 * like neither previous design.
 *
 * Mode-2 predicate: no 100us-floor analog and no N-consecutive-check
 * debounce. The floor existed to reject cumulative-counter noise and the
 * tick-granularity false positives are exactly what a debounce CAN'T fix
 * (they're time-correlated -- every check inside the same 2.5ms stale
 * window agrees). The live bit needs neither: reading 1 is a true
 * statement that the holder is off-CPU at this instant. Micro-preemption
 * cost is bounded by the 10us slice + re-check loop instead.
 */
static int adaptive_backoff_step(struct data *data, unsigned long lock_val,
                                  int *spin_count,
                                  int *snapshot_cpu,
                                  unsigned long long *holder_steal_snapshot,
                                  struct thread_data *tdata)
{
        if (!adaptive_spin_enabled || lock_val == 0)
                return 0;

        if (++(*spin_count) < ADAPTIVE_SPIN_BUDGET)
                return 0;
        *spin_count = 0;

        int holder_cpu = (int)(lock_val - 1);

        if (holder_cpu < 0 || holder_cpu >= VCAP_MAX_CPUS)
                return 0;

        if (adaptive_spin_enabled == 2) {
                if (holder_vcpu_preempted(holder_cpu) != 1)
                        return 0;

                tdata->adaptive_backoffs++;

                unsigned long long waited = 0;

                /*
                 * Ride out the preemption in slices: stop as soon as the
                 * lock changes hands (a preempted holder can't release, so
                 * a changed word means our sample was already stale), the
                 * holder's vCPU is running again, or the stale-holder cap
                 * trips. Nominal-slice accounting (no clock reads) keeps
                 * this loop syscall-minimal: one pread per 10us slice.
                 */
                do {
                        tpause_wait_ns(ADAPTIVE_BACKOFF_CHUNK_NS);
                        waited += ADAPTIVE_BACKOFF_CHUNK_NS;
                        rmb();
                        if (data->lock != lock_val || data->done)
                                break;
                } while (waited < ADAPTIVE_BACKOFF_MAX_NS &&
                         holder_vcpu_preempted(holder_cpu) == 1);

                tdata->backoff_wait_ns += waited;
                return 1;
        }

        /* Mode 1: original steal-delta design, kept intact for A/B runs. */
        unsigned long long steal_now[VCAP_MAX_CPUS] = {0};

        if (read_vcap_steal(steal_now) != 0)
                return 0;

        int unhealthy = 0;

        /*
         * Fable investigation: the snapshot must be tagged with the CPU it
         * was taken for -- the lock can change hands between two checks
         * within one wait, and comparing a stale holder's cumulative steal
         * counter against a new holder's is meaningless (two unrelated
         * CPUs' all-time totals). Same 100us floor established for the
         * host_preempted ground-truth check applies here too.
         */
        if (*snapshot_cpu == holder_cpu &&
            steal_now[holder_cpu] > *holder_steal_snapshot &&
            (steal_now[holder_cpu] - *holder_steal_snapshot) > 100000ULL)
                unhealthy = 1;

        *holder_steal_snapshot = steal_now[holder_cpu];
        *snapshot_cpu = holder_cpu;

        if (unhealthy) {
                tdata->adaptive_backoffs++;
                tpause_wait_ns(50000); /* 50us, same duration as the old nanosleep */
                tdata->backoff_wait_ns += 50000;
                return 1;
        }
        return 0;
}

static inline unsigned long
cmpxchg(volatile unsigned long *ptr, unsigned long old, unsigned long new)
{
        unsigned long prev;

        asm volatile("lock; cmpxchg %b1,%2"
                     : "=a"(prev)
                     : "q"(new), "m"(*(ptr)), "0"(old)
                     : "memory");
        return prev;
}

static inline int dec_extend(volatile unsigned *ptr)
{
        if (*ptr & ~3)
                asm volatile("subl %b1,%0"
                             : "+m" (*(volatile char *)ptr)
                             : "iq" (0x4)
                             : "memory");

        return *ptr & 2;
}

static inline void inc_extend(volatile unsigned *ptr)
{
        asm volatile("addl %b1,%0"
                     : "+m" (*(volatile char *)ptr)
                     : "iq" (0x4)
                     : "memory");
}

/* wait_counter mirrors cr_counter encoding: bits [31:2] = nesting depth.
 * No kernel-request bit needed for waiters; dec_wait has no return value. */
static inline void inc_wait(volatile unsigned *ptr)
{
        asm volatile("addl %b1,%0"
                     : "+m" (*(volatile char *)ptr)
                     : "iq" (0x4)
                     : "memory");
}

static inline void dec_wait(volatile unsigned *ptr)
{
        if (*ptr & ~3)
                asm volatile("subl %b1,%0"
                             : "+m" (*(volatile char *)ptr)
                             : "iq" (0x4)
                             : "memory");
}

static void wait_enter(void)
{
        if (no_rseq)
                return;
        inc_wait(&rseq_map->wait_counter);
}

static void wait_exit(void)
{
        if (no_rseq)
                return;
        dec_wait(&rseq_map->wait_counter);
}

static void extend(void)
{
        if (no_rseq)
                return;

        inc_extend(&rseq_map->cr_counter);
}

static int unextend(void)
{
        if (no_rseq)
                return 0;

        if (!dec_extend(&rseq_map->cr_counter))
                return 0;

        rseq_map->cr_counter = 0;
        tracefs_printf(NULL, "Yield!\n");
        sched_yield();
        return 1;
}

#define sec2usec(sec) (sec * 1000000ULL)
#define usec2sec(usec) (usec / 1000000ULL)

static unsigned long long get_time(void)
{
        struct timeval tv;
        unsigned long long time;

        gettimeofday(&tv, NULL);

        time = sec2usec(tv.tv_sec);
        time += tv.tv_usec;

        return time;
}

static unsigned long long get_time_ns(void)
{
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (unsigned long long)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static unsigned long long get_time_cputime(void)
{
        struct timespec ts;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return (unsigned long long)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void do_sleep(unsigned usecs)
{
        struct timespec ts;

        ts.tv_sec = 0;
        ts.tv_nsec = usecs * 1000;
        nanosleep(&ts, NULL);
}

static void grab_lock(struct thread_data *tdata, struct data *data)
{
        unsigned long long start_wait, start, end, delta;
        unsigned long long end_wait;
        unsigned long long start_wait_ns, start_ns, end_ns;
        unsigned long long start_active_ns, end_active_ns;
        unsigned long prev;
        bool contention = false;

        {
                /* Checkpoint H: verdict computed from CURRENT global state (other
                 * threads' waiters/history), before this thread adds itself to the
                 * count -- same "check before I join" semantics as the kernel's
                 * pre-lock hotlock check (spinlock.c:105-119). Ignored by the
                 * syscall; read externally by ivh_hotlock_observe.bpf.c. */
                int hl_waiters_now = ivh_hotlock_waiters;
                int hl_verdict = ivh_hotlock_is_hot(hl_waiters_now);

                unsigned long long _t0 = get_time_ns();
                ivh_cs_enter((unsigned long)&data->lock, (unsigned long)hl_waiters_now,
                             (unsigned long)hl_verdict);
                unsigned long long _dt = get_time_ns() - _t0;
                tdata->migration_count++;
                tdata->sum_migration_ns += _dt;
                if (_dt > tdata->max_migration_ns)
                        tdata->max_migration_ns = _dt;
                if (_dt > 1000000ULL) /* >1ms = migration got stuck in schedule() */
                        tdata->slow_migration_count++;
        }

        unsigned long my_lock_val = (unsigned long)sched_getcpu() + 1;
        int as_spin_count = 0, as_snapshot_cpu = -1;
        unsigned long long as_holder_snapshot = 0;
        int had_backoff_this_wait = 0;
        /*
         * Fable investigation, tonight: get_time_cputime() ->
         * clock_gettime(CLOCK_THREAD_CPUTIME_ID) can't be vDSO-serviced --
         * every call takes task_rq_lock in the kernel, confirmed as the
         * dominant remaining incidental IVH-trigger source in the
         * post-pread-fix kstack trace. Gated behind wait_time_track_enabled
         * (default on; set to 0 for a clean host_preempted measurement run,
         * since these two calls alone are two guaranteed extra kernel-lock
         * touches per CS cycle, independent of adaptive spinning).
         */
        unsigned long long wait_wall_t0_ns = wait_time_track_enabled ? get_time_ns() : 0;
        unsigned long long wait_oncpu_t0_ns = wait_time_track_enabled ? get_time_cputime() : 0;

        start_wait = get_time();
        start_wait_ns = get_time_ns();

        wait_enter(); /* entering spin/wait region; wait_counter > 0 until lock acquired */
        ivh_hotlock_note_enter();
        rmb();
        while (data->lock && !data->done) {
                contention = true;
                if (adaptive_backoff_step(data, data->lock, &as_spin_count, &as_snapshot_cpu, &as_holder_snapshot, tdata))
                        had_backoff_this_wait = 1;
                rmb();
        }

        /*
         * Fable investigation, tonight: a non-yielding tpause backoff still
         * leaves a window where guest load balancing (not IVH) could have
         * moved this thread mid-wait, and the pre-wait ivh_cs_enter() at the
         * top of this function is stale by now for any wait long enough to
         * have backed off at all. One re-check here is the *designed* use
         * of the mechanism (bpf_sched_pre_lock_migrate's own lock_depth++
         * recursion guard makes this safe, no incidental fd/task_rq_lock
         * touches like the open/close problem fixed earlier) -- only paid
         * on the rare path that actually backed off.
         */
        if (had_backoff_this_wait && backoff_recheck_enabled)
                ivh_cs_enter((unsigned long)&data->lock, (unsigned long)ivh_hotlock_waiters,
                             (unsigned long)ivh_hotlock_is_hot(ivh_hotlock_waiters));

        tracefs_printf(NULL, "Grab lock\n");
        if (extend_wait)
                extend();
        do {
                if (!extend_wait)
                        extend();
                start = get_time();
                /*
                 * Fable investigation: recomputed fresh on every attempt, not
                 * once before the wait -- with -n (unpinned) threads and an
                 * average wait long enough for guest load balancing (or the
                 * backoff re-check migration) to move this thread, a
                 * once-computed value could publish a stale CPU in the lock
                 * word for the entire tenure, making every waiter monitor the
                 * wrong CPU's health for that whole CS.
                 */
                my_lock_val = (unsigned long)sched_getcpu() + 1;
                prev = cmpxchg(&data->lock, 0, my_lock_val);
                if (prev) {
                        contention = true;
                        if (!extend_wait && unextend())
                                tdata->extended++;
                        while (data->lock && !data->done) {
                                if (adaptive_backoff_step(data, data->lock, &as_spin_count, &as_snapshot_cpu, &as_holder_snapshot, tdata))
                                        had_backoff_this_wait = 1;
                                rmb();
                        }
                }
        } while (prev && !data->done);

        if (contention)
                tdata->contention++;

        if (data->done) {
                wait_exit(); /* abandoned wait at shutdown */
                ivh_hotlock_note_exit();
                return;
        }

        wait_exit(); /* lock acquired; no longer spinning/waiting */
        ivh_hotlock_note_exit();
        ivh_hotlock_update(contention);
        if (wait_time_track_enabled) {
                tdata->wait_wall_ns += get_time_ns() - wait_wall_t0_ns;
                tdata->wait_oncpu_ns += get_time_cputime() - wait_oncpu_t0_ns;
        }
        end_wait = get_time();
        start_ns = get_time_ns();
        start_active_ns = get_time_cputime();

        int cs_cpu_start = sched_getcpu();
        unsigned long long steal_before[VCAP_MAX_CPUS] = {0};
        read_vcap_steal(steal_before);

        tracefs_printf(NULL, "Have lock!\n");
        delta = end_wait - start_wait;
        if (!tdata->total_wait || tdata->max_wait < delta)
                tdata->max_wait = delta;
        if (!tdata->total_wait || tdata->min_wait > delta)
                tdata->min_wait = delta;
        tdata->total_wait += delta;

        data->x++;

        if (data->lock != my_lock_val) {
                printf("Failed locking\n");
                exit(-1);
        }

        /* Loop */
        for (int i = 0; i < loop_spin; i++)
                wmb();

        prev = cmpxchg(&data->lock, my_lock_val, 0);
        end = get_time();
        end_ns = get_time_ns();
        end_active_ns = get_time_cputime();

        int cs_cpu_end = sched_getcpu();
        unsigned long long steal_after[VCAP_MAX_CPUS] = {0};
        read_vcap_steal(steal_after);
        {
                /*
                 * >0 alone flags on background steal-counter noise (confirmed:
                 * still ~64% hit rate with sysbench fully off, barely down from
                 * ~76% with it on, even though the raw 2s background delta drops
                 * 300-1000x). Use the same 100us floor the guest-level metric
                 * above already uses, matching the order of magnitude of the
                 * kernel's own >1ms rq->preemptions filter (cputime.c:270).
                 */
                bool migrated = (cs_cpu_start != cs_cpu_end);
                long long delta_start = (long long)steal_after[cs_cpu_start] - (long long)steal_before[cs_cpu_start];
                bool host_preempted = delta_start > 100000LL;
                if (migrated) {
                        long long delta_end = (long long)steal_after[cs_cpu_end] - (long long)steal_before[cs_cpu_end];
                        host_preempted = host_preempted || (delta_end > 100000LL);
                        tdata->host_preempted_migrated_count++;
                }
                if (host_preempted)
                        tdata->host_preempted_count++;

                if (had_backoff_this_wait) {
                        tdata->cs_after_backoff_count++;
                        if (host_preempted)
                                tdata->cs_after_backoff_preempted_count++;
                } else {
                        tdata->cs_no_backoff_count++;
                        if (host_preempted)
                                tdata->cs_no_backoff_preempted_count++;
                }
        }

        tracefs_printf(NULL, "released lock!\n");
        tdata->last_cs_ns        = end_ns - start_ns;
        tdata->last_cs_active_ns = end_active_ns - start_active_ns;
        tdata->last_wait_ns      = start_ns - start_wait_ns;
        if (tdata->last_cs_ns > tdata->max_cs_ns)
                tdata->max_cs_ns = tdata->last_cs_ns;
        if (tdata->last_cs_active_ns > tdata->max_cs_active_ns)
                tdata->max_cs_active_ns = tdata->last_cs_active_ns;
        tdata->sum_cs_ns        += tdata->last_cs_ns;
        tdata->sum_cs_active_ns += tdata->last_cs_active_ns;
        tdata->cs_count++;
        /* count CS cycles where the holder was preempted >100us during the hold */
        if ((long long)tdata->last_cs_ns - (long long)tdata->last_cs_active_ns > 100000LL)
                tdata->cs_preempted_count++;
        if (!no_rseq && rseq_map) {
                rseq_map->last_cs_overall_ns   = tdata->last_cs_ns;
                rseq_map->last_cs_active_ns    = tdata->last_cs_active_ns;
                rseq_map->last_wait_overall_ns = tdata->last_wait_ns;
        }

        if (unextend())
                tdata->extended++;
        if (prev != my_lock_val) {
                printf("Failed unlocking\n");
                exit(-1);
        }

        delta = end - start;
        if (!tdata->total || tdata->max < delta) {
                tracefs_printf(NULL, "New max: %lld\n", delta);
                tdata->max = delta;
        }

        if (!tdata->total || tdata->min > delta)
                tdata->min = delta;

        tdata->total += delta;
        tdata->x_count++;
}

static void *busy_thread(void *d)
{
        struct data *data = d;
        int i;

        while (!data->done) {
                for (i = 0; i < 100; i++)
                        wmb();
                do_sleep(10);
                rmb();
        }
        return NULL;
}

static void *run_thread(void *d)
{
        struct thread_data *tdata = d;
        struct data *data = tdata->data;

        init_extend_map();

        pthread_barrier_wait(&pbarrier);

        while (!data->done) {
                grab_lock(tdata, data);
                /* Make slighty different waits */
                /* 100us + cpu * 27us */
                unsigned wait_us = 100 + tdata->cpu * 27;
                if (sleep_mode == 0) {
                        do_sleep(wait_us);
                } else if (sleep_mode == 1) {
                        unsigned long long t0 = get_time_ns();
                        while (get_time_ns() - t0 < (unsigned long long)wait_us * 1000)
                                rmb();
                }
                /* sleep_mode == 2: no wait at all, re-grab immediately */
                rmb();
        }
        return NULL;
}


int main (int argc, char **argv)
{
        calibrate_tsc(); /* one-time, ~20ms, before any thread starts */
        unsigned long long total_wait = 0;
        unsigned long long total_held = 0;
        unsigned long long total_contention = 0;
        unsigned long long total_extended = 0;
        unsigned long long max_wait = 0;
        unsigned long long max = 0;
        unsigned long long secs;
        unsigned long long avg_wait;
        unsigned long long avg_secs;
        unsigned long long avg_held;
        unsigned long long total_count = 0;
        bool verbose = false;
        bool show_last = false;
        pthread_t *threads;
        cpu_set_t *save_affinity;
        cpu_set_t *set_affinity;
        size_t cpu_size;
        struct data data;
        int cpus;
        int ch;
        int i;

        while ((ch = getopt(argc, argv, "dwvlnb:")) >= 0) {
                switch (ch) {
                        case 'd':
                                no_rseq = true;
                                break;
                        case 'n':
                                no_pin = true;
                                break;
                        case 'w':
                                extend_wait = true;
                                break;
                        case 'v':
                                verbose = true;
                                break;
                        case 'l':
                                show_last = true;
                                break;
                        case 'b': {
                                char *endp;
                                num_busy_threads = strtol(optarg, &endp, 10);
                                if (!optarg[0] || *endp || num_busy_threads < 0) {
                                        fprintf(stderr, "Invalid busy thread count: %s\n", optarg);
                                        exit(-1);
                                }
                                break;
                        }
                        default:
                                fprintf(stderr, "usage: NHextend [-d|-w|-v|-l|-n] [-b busy_threads] [threads]\n"
                                                "  -d: disable rseq\n"
                                                "  -n: no CPU pinning (threads float across all CPUs, needed for IVH migration)\n"
                                                "  -w: extend while trying to get lock\n"
                                                "  -v: verbose output\n"
                                                "  -l: print last CS and wait time per thread (ns)\n"
                                                "  -b: number of busy background threads (default: 0)\n"
                                                "  threads: total number of worker threads (default: cpu count)\n");
                                exit(-1);
                }
        }

        if (optind < argc) {
                char *endp;

                num_threads = strtol(argv[optind], &endp, 10);
                if (!argv[optind][0] || *endp || num_threads <= 0) {
                        fprintf(stderr, "Invalid thread count: %s\n", argv[optind]);
                        exit(-1);
                }
                optind++;
        }

        if (optind < argc) {
                fprintf(stderr, "Too many arguments\n");
                exit(-1);
        }

        memset(&data, 0, sizeof(data));

        cpus = sysconf(_SC_NPROCESSORS_CONF);
        if (num_threads <= 0)
                num_threads = cpus;

        cpu_size = CPU_ALLOC_SIZE(cpus);
        save_affinity = CPU_ALLOC(cpus);
        set_affinity = CPU_ALLOC(cpus);
        if (!save_affinity || !set_affinity) {
                perror("Allocating CPU sets");
                exit(-1);
        }
        if (sched_getaffinity(0, cpu_size, save_affinity) < 0) {
                perror("Getting affinity");
                exit(-1);
        }

        /* Create the requested number of lock-worker threads plus busy threads. */
        threads = calloc(num_threads + num_busy_threads, sizeof(*threads));
        if (!threads) {
                perror("threads");
                exit(-1);
        }

        /* Allocate the data for the lock grabbers */
        data.tdata = calloc(num_threads, sizeof(*data.tdata));
        if (!data.tdata) {
                perror("Allocating tdata");
                exit(-1);
        }

        tracefs_print_init(NULL);
        pthread_barrier_init(&pbarrier, NULL, num_threads + 1);

        /* Save current affinity */
        for (i = 0; i < num_threads; i++) {
                int ret;
                int cpu = i % cpus;

                if (!no_pin) {
                        /* Set the affinity to this CPU as threads will inherit it */
                        CPU_ZERO_S(cpu_size, set_affinity);
                        CPU_SET_S(cpu, cpu_size, set_affinity);
                        if (sched_setaffinity(0, cpu_size, set_affinity) < 0) {
                                perror("Setting affinity");
                                fprintf(stderr, " Setting cpu %d\n", cpu);
                                exit(-1);
                        }
                }

                data.tdata[i].data = &data;
                data.tdata[i].cpu = cpu;

                ret = pthread_create(&threads[i], NULL, run_thread, &data.tdata[i]);
                if (ret < 0) {
                        perror("creating lock threads");
                        exit(-1);
                }
        }

        if (!no_pin && sched_setaffinity(0, cpu_size, save_affinity) < 0) {
                perror("Setting saved affinity");
                exit(-1);
        }

        for (i = 0; i < num_busy_threads; i++) {
                int ret = pthread_create(&threads[num_threads + i], NULL, busy_thread, &data);
                if (ret < 0) {
                        perror("creating busy threads");
                        exit(-1);
                }
        }

        pthread_barrier_wait(&pbarrier);
        {
                const char *dur = getenv("NHEXTEND_DURATION");
                sleep(dur ? atoi(dur) : 5);
        }

        data.done = true;
        wmb();
        for (i = 0; i < num_threads + num_busy_threads; i++) {
                pthread_join(threads[i], NULL);
                if (i >= num_threads)
                        continue;
                if (verbose) {
                        printf("thread %i:\n", i);
                        printf("   count:\t%lld\n", data.tdata[i].x_count);
                        printf("   total:\t%lld\n", data.tdata[i].total);
                        printf("     max:\t%lld\n", data.tdata[i].max);
                        printf("     min:\t%lld\n", data.tdata[i].min);
                        printf("   total wait:\t%lld\n", data.tdata[i].total_wait);
                        printf("     max wait:\t%lld\n", data.tdata[i].max_wait);
                        printf("     min wait:\t%lld\n", data.tdata[i].min_wait);
                        printf("   contention:\t%lld\n", data.tdata[i].contention);
                        printf("     extended:\t%lld\n", data.tdata[i].extended);
                }
                total_count += data.tdata[i].x_count;
                total_wait += data.tdata[i].total_wait;
                total_contention += data.tdata[i].contention;
                total_held += data.tdata[i].total;
                total_extended += data.tdata[i].extended;
                if (data.tdata[i].max_wait > max_wait)
                        max_wait = data.tdata[i].max_wait;
                if (data.tdata[i].max > max)
                        max = data.tdata[i].max;
        }

        secs = usec2sec(total_wait);
        avg_wait = total_count ? total_wait / total_count : 0;
        avg_secs = usec2sec(avg_wait);
        avg_held = total_count ? total_held / total_count : 0;

        if (show_last) {
                int violations = 0;
                unsigned long long g_max_ov = 0, g_max_ac = 0;
                unsigned long long g_sum_ov = 0, g_sum_ac = 0, g_count = 0;

                printf("CS stats per thread (ns):\n");
                printf("  %-6s  %-12s  %-12s  %-12s  %-12s  %-12s  %-12s\n",
                       "thread",
                       "avg_overall", "avg_active",
                       "max_overall", "max_active",
                       "max_offcpu", "ok?");
                for (i = 0; i < num_threads; i++) {
                        struct thread_data *t = &data.tdata[i];
                        unsigned long long cnt = t->cs_count ? t->cs_count : 1;
                        unsigned long long avg_ov = t->sum_cs_ns / cnt;
                        unsigned long long avg_ac = t->sum_cs_active_ns / cnt;
                        long long max_off = (long long)t->max_cs_ns - (long long)t->max_cs_active_ns;
                        /* violation: max_overall meaningfully less than max_active */
                        int ok = ((long long)t->max_cs_ns >= (long long)t->max_cs_active_ns - 1000);
                        if (!ok) violations++;
                        printf("  %-6d  %-12llu  %-12llu  %-12llu  %-12llu  %-12lld  %s\n",
                               i, avg_ov, avg_ac,
                               t->max_cs_ns, t->max_cs_active_ns,
                               max_off, ok ? "OK" : "VIOLATION");
                        if (t->max_cs_ns > g_max_ov) g_max_ov = t->max_cs_ns;
                        if (t->max_cs_active_ns > g_max_ac) g_max_ac = t->max_cs_active_ns;
                        g_sum_ov += t->sum_cs_ns;
                        g_sum_ac += t->sum_cs_active_ns;
                        g_count  += t->cs_count;
                }
                printf("\n");
                unsigned long long g_cnt = g_count ? g_count : 1;
                printf("  Global avg overall : %llu ns\n", g_sum_ov / g_cnt);
                printf("  Global avg active  : %llu ns\n", g_sum_ac / g_cnt);
                printf("  Global max overall : %llu ns  (%.1f µs)\n", g_max_ov, g_max_ov / 1000.0);
                printf("  Global max active  : %llu ns  (%.1f µs)\n", g_max_ac, g_max_ac / 1000.0);
                printf("  Max offcpu penalty : %lld ns  (overall - active at worst CS)\n",
                       (long long)g_max_ov - (long long)g_max_ac);
                if (violations)
                        printf("  WARNING: %d violation(s)\n", violations);
                else
                        printf("  Invariant OK: max_overall >= max_active on all threads\n");
                printf("\n");

                /* IVH migration stats — measures ivh_cs_enter() duration, not CS hold */
                unsigned long long g_mig_count = 0, g_mig_sum = 0, g_mig_max = 0;
                unsigned long long g_slow = 0, g_cs_preempted = 0;
                unsigned long long g_host_preempted = 0, g_host_migrated = 0;
                unsigned long long g_backoffs = 0, g_backoff_wait = 0, g_wait_wall = 0, g_wait_oncpu = 0;
                unsigned long long g_after_backoff = 0, g_after_backoff_preempted = 0;
                unsigned long long g_no_backoff = 0, g_no_backoff_preempted = 0;
                printf("IVH migration stats (ivh_cs_enter duration, NOT included in CS above):\n");
                printf("  %-6s  %-10s  %-12s  %-12s  %-12s\n",
                       "thread", "calls", "avg_ns", "max_ns", "slow(>1ms)");
                for (i = 0; i < num_threads; i++) {
                        struct thread_data *t = &data.tdata[i];
                        unsigned long long cnt = t->migration_count ? t->migration_count : 1;
                        printf("  %-6d  %-10llu  %-12llu  %-12llu  %-12llu\n",
                               i, t->migration_count,
                               t->sum_migration_ns / cnt,
                               t->max_migration_ns,
                               t->slow_migration_count);
                        g_mig_count += t->migration_count;
                        g_mig_sum   += t->sum_migration_ns;
                        if (t->max_migration_ns > g_mig_max) g_mig_max = t->max_migration_ns;
                        g_slow      += t->slow_migration_count;
                        g_cs_preempted += t->cs_preempted_count;
                        g_host_preempted += t->host_preempted_count;
                        g_host_migrated  += t->host_preempted_migrated_count;
                        g_backoffs   += t->adaptive_backoffs;
                        g_backoff_wait += t->backoff_wait_ns;
                        g_wait_wall  += t->wait_wall_ns;
                        g_wait_oncpu += t->wait_oncpu_ns;
                        g_after_backoff += t->cs_after_backoff_count;
                        g_after_backoff_preempted += t->cs_after_backoff_preempted_count;
                        g_no_backoff += t->cs_no_backoff_count;
                        g_no_backoff_preempted += t->cs_no_backoff_preempted_count;
                }
                unsigned long long g_mig_cnt = g_mig_count ? g_mig_count : 1;
                unsigned long long g_cs_cnt  = g_count ? g_count : 1;
                printf("\n");
                printf("  Total migrations    : %llu\n", g_mig_count);
                printf("  Avg migration       : %llu ns\n", g_mig_sum / g_mig_cnt);
                printf("  Max migration       : %llu ns  (%.1f ms)\n",
                       g_mig_max, g_mig_max / 1e6);
                printf("  Stuck (>1ms)        : %llu  (%.4f%% of migrations)\n",
                       g_slow, 100.0 * g_slow / g_mig_cnt);
                printf("\n");
                printf("CS holder preemption (off-CPU >100us DURING lock hold, GUEST-LEVEL, ru_nivcsw-driven):\n");
                printf("  Preempted CS cycles : %llu / %llu  (%.4f%%)\n",
                       g_cs_preempted, g_count,
                       100.0 * g_cs_preempted / g_cs_cnt);
                printf("\n");
                printf("HOST-level steal during hold (real /proc/vcap_info steal_time delta, ground truth):\n");
                printf("  Host-preempted CS cycles : %llu / %llu  (%.4f%%)\n",
                       g_host_preempted, g_count,
                       100.0 * g_host_preempted / g_cs_cnt);
                printf("  (of which, thread migrated mid-CS): %llu\n", g_host_migrated);
                printf("\n");
                printf("Stage A4 adaptive-spin prototype (mode=%d: 0=off 1=steal-delta 2=live-bit):\n", adaptive_spin_enabled);
                printf("  Backoffs taken       : %llu\n", g_backoffs);
                printf("  Backoff wall time    : %llu ns  (avg %llu ns/backoff)\n",
                       g_backoff_wait, g_backoffs ? g_backoff_wait / g_backoffs : 0);
                printf("  Wait wall-clock total: %llu ns\n", g_wait_wall);
                printf("  Wait on-CPU total    : %llu ns\n", g_wait_oncpu);
                printf("  Wait off-CPU (burned): %lld ns  (%.2f%% of wait was on-CPU)\n",
                       (long long)g_wait_wall - (long long)g_wait_oncpu,
                       g_wait_wall ? 100.0 * g_wait_oncpu / g_wait_wall : 0.0);
                printf("\n");
                printf("Correlation: does a backoff during the wait predict host_preempted on the CS that follows?\n");
                printf("  CS after a backoff   : %llu / %llu preempted  (%.4f%%)\n",
                       g_after_backoff_preempted, g_after_backoff,
                       g_after_backoff ? 100.0 * g_after_backoff_preempted / g_after_backoff : 0.0);
                printf("  CS with no backoff   : %llu / %llu preempted  (%.4f%%)\n",
                       g_no_backoff_preempted, g_no_backoff,
                       g_no_backoff ? 100.0 * g_no_backoff_preempted / g_no_backoff : 0.0);
                printf("\n");
        }

        printf("Ran for %lld times\n", data.x);
        printf("Total wait time: %llu.%06llu  (avg: %llu.%06llu)\n", secs, total_wait - sec2usec(secs),
                                avg_secs, avg_wait - sec2usec(avg_secs));
        printf("Total contention: %lld\n", total_contention);
        printf("Total extended: %lld\n", total_extended);
        printf("      max wait: %lld\n", max_wait);
        printf("           max: %lld (avg: %llu)\n", max, avg_held);
        return 0;
}
