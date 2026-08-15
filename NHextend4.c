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
#include <immintrin.h>
#include <sys/auxv.h>
#include <stdint.h>

/*
 * Host-level steal-time ground truth, read from /proc/vcap_info
 * (custom_modules/vsched_module.c, get_info_read() -> get_steal_and_preemptions()
 * -> paravirt_steal_clock()). Per-CPU raw cumulative steal ns since boot, driven
 * directly by the KVM steal-time MSR -- independent of guest scheduling entirely,
 * unlike cs_preempted_count below (which only catches guest-internal off-CPU
 * gaps, not real host-level vCPU steals).
 */
#define VCAP_MAX_CPUS 256

/*
 * Persistent thread-local fd + pread(fd, buf, sz, 0): the kernel proc handler
 * blocks re-read() via *ppos>0 (single-shot dump per open), but pread() passes
 * a local pos and never touches f_pos, so it re-triggers the dump every call
 * without re-opening the file (avoiding an open()+close() pair per check).
 */
static __thread int vcap_steal_fd = -1;

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
                } else if (field == 2 && cpu >= 0 && cpu < VCAP_MAX_CPUS) {
                        steal_out[cpu] = strtoull(tok, NULL, 10);
                }
                field = (field + 1) % 4;
                tok = strtok_r(NULL, "\n", &saveptr);
        }
        return 0;
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
 * RSEQ_SCHED_STATE_FLAG_IVH_DANGER (include/uapi/linux/rseq.h, 2026-07-20):
 * the kernel publishes this bit in struct rseq_sched_state::state on every
 * return-to-userspace (rseq_update_cpu_node_id() -> ivh_task_rq_in_danger(),
 * kernel/sched/fair.c) whenever this thread's current CPU fails IVH's
 * capacity/time-left gates -- i.e. whenever entering a critical section
 * right now would actually be a migration candidate. Hand-declared here
 * (not yet in system headers) to match the kernel uapi exactly.
 */
#ifndef RSEQ_SCHED_STATE_FLAG_ON_CPU
#define RSEQ_SCHED_STATE_FLAG_ON_CPU (1U << 0)
#endif
#ifndef RSEQ_SCHED_STATE_FLAG_IVH_DANGER
#define RSEQ_SCHED_STATE_FLAG_IVH_DANGER (1U << 1)
#endif

struct rseq_sched_state {
        __u32 version;
        __u32 state;
        __u32 tid;
};

/*
 * Per-thread published sched_state block. One instance per worker thread
 * (each thread registers its own rseq + sched_state_ptr), so this must be
 * __thread, not a single global -- a global would let threads race on the
 * same version/state/tid fields and would only reflect whichever thread
 * registered last.
 */
static __thread struct rseq_sched_state ivh_sched_state __attribute__((aligned(64)));

/*
 * ivh_danger() - read this thread's own advisory danger bit, no syscall.
 * Returns true (fail-open, "assume danger, make the real syscall") whenever
 * the feature isn't actually active for this thread -- old kernel without
 * the bit, failed registration, or -n mode -- so the optimization can only
 * ever remove syscalls, never silently remove real migrations, on a kernel
 * that doesn't support it.
 */
static bool ivh_sched_state_active;

/*
 * Diagnostic override (NHEXTEND_IVH_NO_SKIP=1, default 0): force every
 * ivh_cs_enter_checked() to make the authoritative syscall, bypassing the
 * RSEQ_SCHED_STATE_FLAG_IVH_DANGER local pre-check entirely. This reproduces
 * the pre-advisory-bit behavior (kernel-54-era binary: every lock attempt
 * hits the kernel's own fresh capacity/time-left gate) so a run's
 * round-to-round consistency can be compared with vs without the stale
 * advisory skip. The danger bit is only refreshed on return-to-userspace
 * (tick / syscall return), so in a tight ~1ms-CS userspace loop it can be
 * stale for a whole quantum -- a clear-but-actually-stale bit suppresses
 * migrations that the authoritative gate would have made, which can collapse
 * an otherwise-winning round toward baseline (wash) or worse. Set this to 1
 * to take that variable out.
 */
static int ivh_force_syscall;

static inline bool ivh_danger(void)
{
        if (ivh_force_syscall)
                return true;
        if (!ivh_sched_state_active)
                return true;
        return (ivh_sched_state.state & RSEQ_SCHED_STATE_FLAG_IVH_DANGER) != 0;
}

/*
 * ivh_cs_enter() itself is now the *authoritative* call, unconditionally
 * doing the syscall -- callers that want the cheap local pre-check use
 * ivh_cs_enter_checked() below instead. Kept separate so the one
 * unconditional call NHextend3 needs (the very first entry before rseq/
 * sched_state has had a chance to be populated) still exists.
 */
static inline void ivh_cs_enter(void)
{
        syscall(__NR_ivh_cs_enter);
}

/*
 * ivh_cs_enter_checked() - the actual optimization: skip the syscall
 * entirely when this thread's own last-published danger bit is clear.
 * Returns 1 if the syscall was made, 0 if it was skipped, so callers can
 * keep separate call/skip counters.
 */
static inline int ivh_cs_enter_checked(void)
{
        if (!ivh_danger())
                return 0;
        syscall(__NR_ivh_cs_enter);
        return 1;
}

/* Updated version of rseq structure with cr_counter, wait_counter, timing
 * fields, and sched_state_ptr (+64: opt-in pointer to a userspace-owned
 * struct rseq_sched_state, see register_rseq()). Registering with
 * sizeof < IVH_RSEQ_LEN would silently disable the sched_state_ptr feature
 * (kernel/rseq.c's rseq_get_sched_state_ptr() checks rseq_len), so
 * IVH_RSEQ_LEN, not sizeof(struct rseq_abi), is what gets passed to the
 * rseq() syscall -- see register_rseq(). */
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
        __u64 sched_state_ptr;      /* +64: userspace-owned struct rseq_sched_state* */
} __attribute__((aligned(4 * sizeof(__u64))));

/*
 * Registration lengths. offsetof(), not sizeof(): struct rseq_abi's
 * aligned(32) attribute pads sizeof() up to the next 32-byte multiple (96),
 * which is NOT the byte offset the kernel's rseq_len checks actually care
 * about (kernel/rseq.c compares against offsetof(struct rseq, end) and
 * offsetof(struct rseq, sched_state_ptr) for the real running kernel).
 *   - IVH_RSEQ_LEN_NO_SCHED_STATE (64): the original extended-ABI size,
 *     used verbatim when the running kernel doesn't report a feature size
 *     that includes sched_state_ptr.
 *   - IVH_RSEQ_LEN_SCHED_STATE (72): includes sched_state_ptr.
 * The real runtime length always prefers getauxval(AT_RSEQ_FEATURE_SIZE)
 * when available (register_rseq()) -- these are only the two fallback
 * values when that isn't.
 */
#define IVH_RSEQ_LEN_NO_SCHED_STATE offsetof(struct rseq_abi, sched_state_ptr)
#define IVH_RSEQ_LEN_SCHED_STATE (offsetof(struct rseq_abi, sched_state_ptr) + sizeof(__u64))

static bool no_rseq;
static bool extend_wait;
static bool no_pin;

static int loop_spin = 600000;
static int num_threads = -1;
static int num_busy_threads = 0;

/*
 * ---------------------------------------------------------------------------
 * NHextend4 experiment knobs (NHextend3 is untouched; every default below
 * reproduces NHextend3's exact behaviour, so NH4 with no env set == NH3).
 * ---------------------------------------------------------------------------
 *
 * H2 -- inter-iteration sleep. NHextend3's run_thread() does
 *   do_sleep(100 + tdata->cpu * 27)
 * after every lock release: a FIXED 100..505 us nanosleep that does not scale
 * with loop_spin. Since the offered lock load is
 *   D = N * CS / (CS + sleep)
 * shrinking the CS while holding the sleep fixed collapses D -- i.e. lowering
 * loop_spin does not just shorten the critical section, it also silently
 * de-contends the lock. nh4_sleep_us / nh4_sleep_stagger_us make that term
 * explicit; nh4_sleep_duty_pct instead derives the sleep from the *measured*
 * CS length so that D is held constant across a loop_spin sweep.
 */
static int nh4_sleep_us       = 100;   /* NH4_SLEEP_US        */
static int nh4_sleep_stagger  = 27;    /* NH4_SLEEP_STAGGER_US */
static int nh4_sleep_duty_pct = -1;    /* NH4_SLEEP_DUTY_PCT: sleep = CS*pct/100 */

/*
 * H-extra -- /proc/vcap_info sampling cost. NHextend3 calls read_vcap_steal()
 * twice per critical section, and the *first* call happens while the lock is
 * held. That is a pread() of /proc/vcap_info plus a strtok/sscanf parse of
 * ~4*ncpu lines plus two 2 KB stack memsets, all inside the serialized region,
 * so it puts a fixed floor under the true CS length no matter how small
 * loop_spin gets. nh4_vcap_every=N samples only every Nth CS (ratio ground
 * truth is preserved, the per-CS fixed cost is not); 0 disables it entirely.
 */
static int nh4_vcap_every = 1;         /* NH4_VCAP_EVERY */

/*
 * ---------------------------------------------------------------------------
 * NHextend4 round-2 knobs (2026-08-13 CS-floor deep dive). Defaults still
 * reproduce NHextend3 exactly.
 * ---------------------------------------------------------------------------
 *
 * NH4_LOCKS -- number of INDEPENDENT locks the worker threads are split
 * across (default 1 == NHextend3's single global lock). Thread i uses lock
 * i % NH4_LOCKS, each lock in its own 64-byte cacheline. This exists to test
 * whether NHextend's floor is a property of migration cost per se, or a
 * property of *lock topology*: with one global lock every thread is a
 * mandatory serialisation point, so a thread blocked in
 * wait_for_completion() inside set_cpus_allowed_ptr() idles the ONE lock
 * everybody needs (measured: 67 % lock utilisation under default IVH vs
 * 99.5 % IVH-off). With L independent locks a blocked thread only idles
 * 1/L of the work. hackbench's groups have exactly this shape.
 *
 * NH4_NO_EXTEND -- make extend()/unextend() no-ops, i.e. never publish a
 * nonzero rseq cr_counter. That disables (a) the kernel's rseq grace period
 * (rseq_delay_resched() in kernel/rseq.c only fires when cr_counter's
 * in-critical-section bit is set) and (b) the sched_yield() the harness does
 * when the kernel sets the KERNEL_REQUEST_SCHED bit. The kernel-side global
 * off switch for the same mechanism is sysctl kernel.rseq_sched_extend_usec=0;
 * having both lets the userspace publish cost and the kernel grace period be
 * separated.
 *
 * NH4_NO_WAITCNT -- make wait_enter()/wait_exit() no-ops, i.e. never publish
 * a nonzero rseq wait_counter. rq->user_waiter (kernel/sched/sched.h:308) is
 * fed from this, so it removes the harness's "someone is spinning on me"
 * signal from IVH's view without touching anything else.
 */
static int nh4_locks       = 1;        /* NH4_LOCKS */
static int nh4_no_extend   = 0;        /* NH4_NO_EXTEND */
static int nh4_no_waitcnt  = 0;        /* NH4_NO_WAITCNT */

#define NH4_MAX_LOCKS 64

static void nh4_read_env(void)
{
	const char *s;

	if ((s = getenv("NH4_SLEEP_US")))         nh4_sleep_us       = atoi(s);
	if ((s = getenv("NH4_SLEEP_STAGGER_US"))) nh4_sleep_stagger  = atoi(s);
	if ((s = getenv("NH4_SLEEP_DUTY_PCT")))   nh4_sleep_duty_pct = atoi(s);
	if ((s = getenv("NH4_VCAP_EVERY")))       nh4_vcap_every     = atoi(s);
	if ((s = getenv("NH4_LOCKS")))            nh4_locks          = atoi(s);
	if ((s = getenv("NH4_NO_EXTEND")))        nh4_no_extend      = atoi(s);
	if ((s = getenv("NH4_NO_WAITCNT")))       nh4_no_waitcnt     = atoi(s);
	if (nh4_locks < 1)              nh4_locks = 1;
	if (nh4_locks > NH4_MAX_LOCKS)  nh4_locks = NH4_MAX_LOCKS;

	if (nh4_sleep_us < 0)      nh4_sleep_us = 0;
	if (nh4_sleep_stagger < 0) nh4_sleep_stagger = 0;
	if (nh4_vcap_every < 0)    nh4_vcap_every = 0;
}

#define rmb() asm volatile ("lfence" ::: "memory")
#define wmb() asm volatile ("sfence" ::: "memory")

static pthread_barrier_t pbarrier;

static __thread struct rseq_abi *rseq_map;

/*
 * register_rseq() has to account for glibc (>= 2.35) already having
 * auto-registered rseq for this thread at process/thread start, at this
 * same address, using whatever length getauxval(AT_RSEQ_FEATURE_SIZE)
 * reported to IT. The kernel only reads sched_state_ptr AT REGISTRATION
 * TIME (rseq_get_sched_state_ptr(), kernel/rseq.c) and never re-reads it --
 * so if glibc's registration already "won", writing our sched_state_ptr
 * into the (already-registered) memory afterward would be silently
 * ignored. We must unregister glibc's registration and re-register
 * ourselves with the field populated first. Confirmed live via strace that
 * glibc on this system pre-registers with exactly the auxval-reported
 * length, which is what makes the unregister call's rseq_len match.
 */
static void register_rseq(void)
{
        int ret;
        unsigned long feat_len;
        size_t reg_len;
        bool want_sched_state;

        feat_len = getauxval(AT_RSEQ_FEATURE_SIZE);
        want_sched_state = feat_len >= IVH_RSEQ_LEN_SCHED_STATE;
        reg_len = feat_len ? feat_len : IVH_RSEQ_LEN_NO_SCHED_STATE;

        if (want_sched_state) {
                ivh_sched_state.version = 0;
                ivh_sched_state.tid = (__u32)syscall(SYS_gettid);
                rseq_map->sched_state_ptr = (__u64)(uintptr_t)&ivh_sched_state;
        }

        ret = syscall(__NR_rseq, rseq_map, reg_len, 0, 0x53053053);
        if (ret == 0) {
                ivh_sched_state_active = want_sched_state;
                return;
        }

        if (errno == EINVAL || errno == EBUSY) {
                ret = syscall(__NR_rseq, rseq_map, reg_len, RSEQ_FLAG_UNREGISTER, 0x53053053);
                if (ret < 0) {
                        /*
                         * Longstanding registration we can't match (glibc
                         * used a different length or signature) -- can't
                         * safely take it over. Fail open: sched_state
                         * stays inactive, ivh_danger() always reports
                         * danger, every ivh_cs_enter_checked() call site
                         * still makes the real syscall (old behavior,
                         * just without the new optimization).
                         */
                        ivh_sched_state_active = false;
                        return;
                }
                ret = syscall(__NR_rseq, rseq_map, reg_len, 0, 0x53053053);
                if (ret < 0) {
                        fprintf(stderr, "rseq re-register failed: %m\n");
                        ivh_sched_state_active = false;
                        return;
                }
                ivh_sched_state_active = want_sched_state;
                return;
        }

        fprintf(stderr, "rseq register warning: %m\n");
        ivh_sched_state_active = false;
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
        /* IVH_DANGER local pre-check: how many ivh_cs_enter attempts were
         * skipped locally (no syscall) vs actually made, per call site. */
        unsigned long long                      syscall_skipped_count;
        unsigned long long                      syscall_made_count;
        /* CS preemption tracking: how often was the lock holder preempted */
        unsigned long long                      cs_preempted_count;  /* CS cycles with >100us off-CPU, GUEST-LEVEL proxy */
        /* Host-level steal-time tracking (ground truth, see read_vcap_steal) */
        unsigned long long                      host_preempted_count;
        unsigned long long                      host_preempted_migrated_count;
        /* NH4: how many CSs actually sampled /proc/vcap_info (see nh4_vcap_every).
         * This, not cs_count, is the correct denominator for the host-preempted
         * ratio when sampling is enabled. */
        unsigned long long                      vcap_sampled_count;
        struct data                             *data;
        struct nh4_lockcell                     *cell;   /* NH4_LOCKS: this thread's lock */
        int                                     cpu;
};

/*
 * NH4_LOCKS: one independent lock per cell, each on its own 64-byte
 * cacheline so distinct locks never false-share. nh4_locks == 1 leaves
 * exactly one cell and reproduces NHextend3's single global lock bit for bit.
 */
struct nh4_lockcell {
        unsigned long                   lock;
        unsigned long long              x;
} __attribute__((aligned(64)));

struct data {
        unsigned long long              x;
        struct nh4_lockcell             *cells;
        struct thread_data              *tdata;
        bool                            done;
};

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
        if (no_rseq || nh4_no_waitcnt)
                return;
        inc_wait(&rseq_map->wait_counter);
}

static void wait_exit(void)
{
        if (no_rseq || nh4_no_waitcnt)
                return;
        dec_wait(&rseq_map->wait_counter);
}

static void extend(void)
{
        if (no_rseq || nh4_no_extend)
                return;

        inc_extend(&rseq_map->cr_counter);
}

static int unextend(void)
{
        if (no_rseq || nh4_no_extend)
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

        if (!usecs)
                return;   /* NH4: 0 means "don't sleep at all", not "syscall with 0" */

        ts.tv_sec = 0;
        ts.tv_nsec = usecs * 1000;
        nanosleep(&ts, NULL);
}

/*
 * NH4: measured cost of one loop_spin critical-section body, in ns. Filled in
 * by nh4_calibrate() before the worker threads start, used by
 * NH4_SLEEP_DUTY_PCT to hold the offered lock load constant across a sweep.
 */
static unsigned long long nh4_cs_ns_est;

static void nh4_calibrate(void)
{
        unsigned long long t0, t1;
        int i;

        /* warm the loop, then time it */
        for (i = 0; i < loop_spin; i++)
                wmb();

        t0 = get_time_ns();
        for (i = 0; i < loop_spin; i++)
                wmb();
        t1 = get_time_ns();

        nh4_cs_ns_est = t1 - t0;
}

/* NH4: the inter-iteration sleep this thread takes after releasing the lock. */
static unsigned nh4_thread_sleep_us(int cpu)
{
        if (nh4_sleep_duty_pct >= 0) {
                /*
                 * Duty-cycle mode: sleep is a fixed fraction of the measured
                 * CS, so N*CS/(CS+sleep) -- the offered load on the lock -- is
                 * invariant under loop_spin. The 27/100 stagger ratio of the
                 * original is preserved so threads still desynchronize.
                 */
                unsigned long long base_us = (nh4_cs_ns_est / 1000ULL) *
                        (unsigned long long)nh4_sleep_duty_pct / 100ULL;

                return (unsigned)(base_us +
                        base_us * 27ULL * (unsigned long long)cpu / 100ULL);
        }

        return (unsigned)(nh4_sleep_us + cpu * nh4_sleep_stagger);
}

static void grab_lock(struct thread_data *tdata, struct data *data)
{
        unsigned long long start_wait, start, end, delta;
        unsigned long long end_wait;
        unsigned long long start_wait_ns, start_ns, end_ns;
        unsigned long long start_active_ns, end_active_ns;
        unsigned long prev;
        unsigned long my_lock_val;
        bool contention = false;
        struct nh4_lockcell *cell = tdata->cell;   /* NH4_LOCKS */

        {
                unsigned long long _t0 = get_time_ns();
                int made = ivh_cs_enter_checked();
                unsigned long long _dt = get_time_ns() - _t0;

                if (!made) {
                        tdata->syscall_skipped_count++;
                } else {
                        tdata->syscall_made_count++;
                        tdata->migration_count++;
                        tdata->sum_migration_ns += _dt;
                        if (_dt > tdata->max_migration_ns)
                                tdata->max_migration_ns = _dt;
                        if (_dt > 1000000ULL) /* >1ms = migration got stuck in schedule() */
                                tdata->slow_migration_count++;
                }
        }

        start_wait = get_time();
        start_wait_ns = get_time_ns();

        wait_enter(); /* entering spin/wait region; wait_counter > 0 until lock acquired */
        rmb();
        while (cell->lock && !data->done) {
                contention = true;
                rmb();
        }

        tracefs_printf(NULL, "Grab lock\n");
        if (extend_wait)
                extend();
        do {
                if (!extend_wait)
                        extend();
                start = get_time();
                /*
                 * Recomputed fresh on every attempt, not once before the
                 * wait: with -n (unpinned) threads and a wait long enough
                 * for guest load balancing to move this thread, a
                 * once-computed value could publish a stale CPU in the
                 * lock word, making every waiter monitor the wrong CPU.
                 */
                my_lock_val = (unsigned long)sched_getcpu() + 1;
                prev = cmpxchg(&cell->lock, 0, my_lock_val);
                if (prev) {
                        contention = true;
                        if (!extend_wait && unextend())
                                tdata->extended++;
                        while (cell->lock && !data->done) {
                                rmb();
                        }
                }
        } while (prev && !data->done);

        if (contention)
                tdata->contention++;

        if (data->done) {
                wait_exit(); /* abandoned wait at shutdown */
                return;
        }

        wait_exit(); /* lock acquired; no longer spinning/waiting */
        end_wait = get_time();
        start_ns = get_time_ns();
        start_active_ns = get_time_cputime();

        /*
         * NH4: sample /proc/vcap_info only every nh4_vcap_every'th CS. The
         * pread+parse is inside the lock hold, so at small loop_spin it is a
         * fixed floor under the true (serialized) CS length. Sampling keeps
         * the host-preempted *ratio* meaningful while removing that floor.
         */
        int cs_cpu_start = sched_getcpu();
        unsigned long long steal_before[VCAP_MAX_CPUS];
        bool do_vcap = nh4_vcap_every &&
                       (tdata->cs_count % (unsigned long long)nh4_vcap_every) == 0;

        if (do_vcap) {
                memset(steal_before, 0, sizeof(steal_before));
                read_vcap_steal(steal_before);
        }

        tracefs_printf(NULL, "Have lock!\n");
        delta = end_wait - start_wait;
        if (!tdata->total_wait || tdata->max_wait < delta)
                tdata->max_wait = delta;
        if (!tdata->total_wait || tdata->min_wait > delta)
                tdata->min_wait = delta;
        tdata->total_wait += delta;

        cell->x++;

        if (cell->lock != my_lock_val) {
                printf("Failed locking\n");
                exit(-1);
        }

        /* Loop */
        for (int i = 0; i < loop_spin; i++)
                wmb();

        prev = cmpxchg(&cell->lock, my_lock_val, 0);
        end = get_time();
        end_ns = get_time_ns();
        end_active_ns = get_time_cputime();

        int cs_cpu_end = sched_getcpu();
        unsigned long long steal_after[VCAP_MAX_CPUS];

        if (do_vcap) {
                memset(steal_after, 0, sizeof(steal_after));
                read_vcap_steal(steal_after);
                tdata->vcap_sampled_count++;
        }
        if (do_vcap) {
                /* >100us floor matches the kernel's own >1ms rq->preemptions
                 * filter's order of magnitude, filtering background
                 * steal-counter noise rather than any nonzero delta. */
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

        unsigned sleep_us = nh4_thread_sleep_us(tdata->cpu);

        while (!data->done) {
                grab_lock(tdata, data);
                /* NH4: was hardcoded do_sleep(100 + tdata->cpu * 27) */
                do_sleep(sleep_us);
                rmb();
        }
        return NULL;
}


int main (int argc, char **argv)
{
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

        {
                const char *ls = getenv("NHEXTEND_LOOP_SPIN");

                if (ls)
                        loop_spin = atoi(ls);

                const char *ns = getenv("NHEXTEND_IVH_NO_SKIP");
                if (ns && atoi(ns) != 0)
                        ivh_force_syscall = 1;

                nh4_read_env();
        }

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
                                fprintf(stderr, "usage: NHextend3 [-d|-w|-v|-l|-n] [-b busy_threads] [threads]\n"
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

        /* NH4_LOCKS: one cacheline-isolated lock cell per independent lock. */
        if (posix_memalign((void **)&data.cells, 64,
                           (size_t)nh4_locks * sizeof(*data.cells))) {
                perror("Allocating lock cells");
                exit(-1);
        }
        memset(data.cells, 0, (size_t)nh4_locks * sizeof(*data.cells));

        tracefs_print_init(NULL);

        /* NH4: measure the CS body once, single-threaded and unloaded, so the
         * duty-cycle sleep mode has a real CS length to work from, and so the
         * banner can report the loop_spin -> ns mapping for this build/host. */
        nh4_calibrate();
        printf("NH4 config: loop_spin=%d (calibrated CS ~%llu ns)  threads=%d\n",
               loop_spin, nh4_cs_ns_est, num_threads);
        if (nh4_sleep_duty_pct >= 0)
                printf("NH4 sleep : DUTY mode, %d%% of CS -> %u..%u us (thread 0..%d)\n",
                       nh4_sleep_duty_pct, nh4_thread_sleep_us(0),
                       nh4_thread_sleep_us(num_threads - 1), num_threads - 1);
        else
                printf("NH4 sleep : FIXED mode, %d + cpu*%d us -> %u..%u us\n",
                       nh4_sleep_us, nh4_sleep_stagger, nh4_thread_sleep_us(0),
                       nh4_thread_sleep_us(num_threads - 1));
        printf("NH4 vcap  : sample every %d CS%s\n", nh4_vcap_every,
               nh4_vcap_every == 0 ? " (DISABLED)" : "");
        printf("NH4 locks : %d independent lock%s (%d thread%s each)  extend=%s waitcnt=%s\n",
               nh4_locks, nh4_locks == 1 ? "" : "s",
               num_threads / nh4_locks, (num_threads / nh4_locks) == 1 ? "" : "s",
               nh4_no_extend ? "OFF" : "on", nh4_no_waitcnt ? "OFF" : "on");
        fflush(stdout);

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
                data.tdata[i].cell = &data.cells[i % nh4_locks];
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
                unsigned long long g_syscall_skipped = 0, g_syscall_made = 0;
                unsigned long long g_vcap_sampled = 0;
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
                        g_vcap_sampled   += t->vcap_sampled_count;
                        g_syscall_skipped   += t->syscall_skipped_count;
                        g_syscall_made      += t->syscall_made_count;
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
                printf("IVH_DANGER local pre-check (RSEQ_SCHED_STATE_FLAG_IVH_DANGER, no syscall when clear):\n");
                if (!ivh_sched_state_active) {
                        printf("  INACTIVE for this run (old kernel, or rseq/sched_state registration\n");
                        printf("  didn't take -- every ivh_cs_enter attempt fell back to a real syscall,\n");
                        printf("  identical to pre-optimization behavior).\n");
                } else {
                        unsigned long long g_attempts = g_syscall_skipped + g_syscall_made;
                        unsigned long long g_attempts_cnt = g_attempts ? g_attempts : 1;
                        printf("  Attempts            : %llu  (skipped %llu, syscall made %llu)\n",
                               g_attempts, g_syscall_skipped, g_syscall_made);
                        printf("  Syscalls avoided    : %.2f%%\n",
                               100.0 * (double)g_syscall_skipped / (double)g_attempts_cnt);
                }
                printf("\n");
                printf("CS holder preemption (off-CPU >100us DURING lock hold, GUEST-LEVEL, ru_nivcsw-style proxy):\n");
                printf("  Preempted CS cycles : %llu / %llu  (%.4f%%)\n",
                       g_cs_preempted, g_count,
                       100.0 * g_cs_preempted / g_cs_cnt);
                printf("\n");
                printf("HOST-level steal during hold (real /proc/vcap_info steal_time delta, ground truth):\n");
                {
                        /* NH4: denominator is SAMPLED CSs, not all CSs. */
                        unsigned long long den = g_vcap_sampled ? g_vcap_sampled : 1;

                        printf("  Host-preempted CS cycles : %llu / %llu  (%.4f%%)\n",
                               g_host_preempted, g_vcap_sampled,
                               100.0 * g_host_preempted / den);
                        printf("  (vcap sampled %llu of %llu CSs, NH4_VCAP_EVERY=%d)\n",
                               g_vcap_sampled, g_count, nh4_vcap_every);
                }
                printf("  (of which, thread migrated mid-CS): %llu\n", g_host_migrated);
                printf("\n");
        }

        /* NH4_LOCKS: data.x is the sum over every independent lock cell.
         * With nh4_locks == 1 this is bit-identical to NHextend3's data.x. */
        data.x = 0;
        for (i = 0; i < nh4_locks; i++)
                data.x += data.cells[i].x;
        printf("Ran for %lld times\n", data.x);
        printf("Total wait time: %llu.%06llu  (avg: %llu.%06llu)\n", secs, total_wait - sec2usec(secs),
                                avg_secs, avg_wait - sec2usec(avg_secs));
        printf("Total contention: %lld\n", total_contention);
        printf("Total extended: %lld\n", total_extended);
        printf("      max wait: %lld\n", max_wait);
        printf("           max: %lld (avg: %llu)\n", max, avg_held);
        return 0;
}
