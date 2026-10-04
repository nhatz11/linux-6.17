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
#include <inttypes.h>

#include "ivh_adaptive_futex_lock.h"

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

        /*
         * 2026-10-01: was /proc/vcap_info, which the vsched_module provided
         * and which DOES NOT EXIST on this kernel -- open() failed every
         * time, steal_before/steal_after stayed zero, and the
         * "Host-preempted CS cycles" counter was therefore a structural 0
         * regardless of what the host actually did. It was read as evidence
         * that NHextend's critical sections were never preempted.
         *
         * /proc/ivh_cpu_stats (G-LOCK-51+) carries the same quantity, TSC
         * derived, one line per CPU:
         *   # ivh_cpu_stats v2 tsc_khz=... seq=... now_tsc=...
         *   # cpu tsc idle_ns steal_ns used_ns ...
         *   0 <tsc> <idle_ns> <steal_ns> ...
         * Field 4 (1-based) is steal_ns. Header lines start with '#'.
         *
         * pread(fd, buf, sz, 0) is still correct: the handler regenerates
         * its snapshot whenever the passed position is 0, so a cached fd
         * re-triggers a FRESH dump rather than serving stale bytes.
         */
        if (vcap_steal_fd < 0) {
                vcap_steal_fd = open("/proc/ivh_cpu_stats", O_RDONLY);
                if (vcap_steal_fd < 0)
                        return -1;
        }
        n = pread(vcap_steal_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0)
                return -1;
        buf[n] = '\0';

        tok = strtok_r(buf, "\n", &saveptr);
        while (tok) {
                if (tok[0] != '#') {
                        int cpu = -1;
                        unsigned long long tsc, idle, steal;

                        if (sscanf(tok, "%d %llu %llu %llu", &cpu, &tsc, &idle,
                                   &steal) == 4 &&
                            cpu >= 0 && cpu < VCAP_MAX_CPUS)
                                steal_out[cpu] = steal;
                }
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

/*
 * Default CS length, changed 2026-09-11: loop_spin=5000 (~13us CS) is the
 * validated migration-engine sweet spot on the GLOCK rebuild -- confirmed
 * across two independent 10-round interleaved runs (+21.2%/+22.8%, 20/20
 * rounds positive, t=10.2/50.0), with CS length itself unchanged between
 * arms (ruling out the migration-inflates-CS-length confound that sank
 * loop_spin=10000, see tools/bpf/docs/ivh_nhextend3_migration_validation_
 * 2026-09-11.md in the docs repo). Was 600000 (~1.6ms) before this change.
 */
static int loop_spin = 5000;
static int num_threads = -1;
static int num_busy_threads = 0;

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
        unsigned long long                      min_cs_ns;   /* running min of last_cs_ns; the CSmin estimate */
        unsigned long long                      max_cs_active_ns;
        unsigned long long                      sum_cs_ns;
        unsigned long long                      sum_cs_active_ns;
        unsigned long long                      cs_count;
        /*
         * 2026-10-01 handoff instrumentation. lock_cycle = handoff +
         * preamble + hold, and before this the first two terms were a
         * single unmeasured residual that happened to hold the entire
         * migration win. handoff is lock dead time (release -> next
         * acquire); preamble is acquire -> start of the timed hold, which
         * is this benchmark's OWN read_vcap_steal()/sched_getcpu() done
         * while holding the lock -- a syscall, so it costs more wall time
         * on a stolen vCPU than a healthy one and cannot be assumed equal
         * across arms.
         */
        unsigned long long                      handoff_sum_ns;
        unsigned long long                      handoff_count;
        unsigned long long                      handoff_max_ns;
        unsigned long long                      preamble_sum_ns;
        unsigned long long                      preamble_max_ns;
        /* IVH migration tracking */
        unsigned long long                      migration_count;      /* syscalls that CHANGED CPU */
        unsigned long long                      migration_to_healthy; /* ...and landed on cpu>=8 */
        unsigned long long                      sum_moved_ns;         /* syscall ns, moved cases only */
        /*
         * Mid-spin migration decision data (2026-10-01). Classify each WAIT
         * by the health of the vCPU it began on and the one it ended on,
         * using the cs_cpu_start / cs_cpu_hold samples the file already
         * takes. A wait that both starts and ends on a low-capacity vCPU is
         * the only case a mid-spin ivh_cs_enter() could have improved, so
         * stuck_wait_ns is the hard ceiling on what that feature could win.
         */
        unsigned long long                      wait_stuck_cnt, wait_stuck_ns;   /* bad -> bad  */
        unsigned long long                      wait_resc_cnt,  wait_resc_ns;    /* bad -> good */
        unsigned long long                      wait_good_cnt,  wait_good_ns;    /* good start  */
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
        unsigned long long                      hold_preempted_count; /* steal>100us during the HOLD only */
        unsigned long long                      cs_start_healthy;  /* CS began on cpu>=8 */
        unsigned long long                      cs_start_total;
        struct data                             *data;
        int                                     cpu;
};

struct data {
        unsigned long long              x;
        struct ivh_afl_lock             lock;
        struct thread_data              *tdata;
        bool                            done;
        /*
         * Stamped immediately BEFORE ivh_afl_unlock() by the outgoing
         * holder, read by the next thread to acquire. Stamping before the
         * release (rather than after) is deliberate: it can only
         * over-report the gap by the cost of the release store itself,
         * whereas stamping after races with the successor and would let it
         * read the PREVIOUS hold's stamp.
         */
        _Alignas(64) volatile unsigned long long last_release_ns;
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

static int post_sleep = 1;   /* NHEXTEND_POST_SLEEP=0 disables */
static int post_spin;        /* NHEXTEND_POST_SPIN=1: do_spin() instead of do_sleep() */
/*
 * NHEXTEND_NO_HOLD_STEAL=1 drops the hold-side read_vcap_steal() sample.
 *
 * That sample (added 2026-10-01 so the hold could be scored separately from
 * the wait) is a /proc/ivh_cpu_stats read -- a SYSCALL -- taken while the
 * lock is held, which is the very thing design doc sec 6.2 removed from this
 * path once already. Measured 2026-10-01 at loop_spin=5000: the preamble it
 * sits in is 11.98us against an 8.76us hold, i.e. 43% of the serialized lock
 * cycle is this instrumentation. It is harmless at loop_spin=600000 (20us on
 * a 1.2ms hold) and dominant below ~50000.
 *
 * Setting this makes hold_preempted_count meaningless, so it is reported as
 * unavailable rather than as zero.
 */
static int no_hold_steal;
/*
 * NHEXTEND_CS_MIN=1 publishes this thread's RUNNING MINIMUM hold time into
 * rseq->last_cs_overall_ns instead of the most recent one.
 *
 * The kernel's Gate 2 computes  time_left = runway - last_cs_ns  and compares
 * it against ivh_time_left_threshold_ns (fair.c:13913), feeding last_cs_ns
 * from exactly this rseq field (bpf_sched.c:620-631). With the minimum
 * published and the threshold set to 0, the gate becomes "is there enough
 * runway left to complete the shortest critical section I have observed" --
 * which has no free parameter, so no threshold sweep is required to defend
 * it. With the most-recent value published, last_cs_ns carries whatever
 * preemption the previous CS happened to suffer, which is why the swept
 * threshold was doing double duty as a noise margin.
 *
 * NHEXTEND_CS_MIN_NS=<n> pins an explicit constant instead of the running
 * minimum, for the case where CSmin is known a priori.
 */
static int use_cs_min;
static unsigned long long cs_min_pinned_ns;

static void do_sleep(unsigned usecs)
{
        struct timespec ts;

        ts.tv_sec = 0;
        ts.tv_nsec = usecs * 1000;
        nanosleep(&ts, NULL);
}

/*
 * do_spin() -- the post-release backoff, served by burning CPU instead of
 * sleeping. Same duration as the do_sleep() it replaces, so the two are a
 * clean A/B on "idle vCPU vs warm vCPU" with the backoff length held fixed.
 *
 *   do_sleep  gives the vCPU up, so the host may deschedule it and the thread
 *             pays a wake-up plus a cold cache on its next acquisition.
 *   no sleep  returns straight to the lock, so 16 threads hammer one
 *             cacheline and the lock word ping-pongs between vCPUs.
 *   do_spin   keeps the vCPU runnable while touching NOTHING shared -- in
 *             particular it does NOT call ivh_afl_publish_heartbeat(), which
 *             the real critical section does, because that writes l->hb_tsc
 *             and would put the bouncing straight back.
 *
 * Same dependent wmb() loop the critical section runs, so the instruction mix
 * matches a CS; only the shared writes are absent. The clock is read once per
 * batch so the bound costs ~nothing.
 */
#define IVH_SPIN_WMB_BATCH 64

static void do_spin(unsigned usecs)
{
        unsigned long long end = get_time_ns() + (unsigned long long)usecs * 1000ULL;

        for (;;) {
                for (int i = 0; i < IVH_SPIN_WMB_BATCH; i++)
                        wmb();
                if (get_time_ns() >= end)
                        return;
        }
}

static __thread struct thread_data *g_tdata;

static void ivh_afl_hook_before_sleep(void *arg)
{
        (void)arg;
        /* Only -w (extend_wait) mode holds a cr_counter extension across the
         * wait at all; in the default mode the extension is now armed after
         * acquisition (see grab_lock()), so there is nothing to drop here. */
        if (extend_wait && unextend())
                g_tdata->extended++;
        wait_exit();
}

static void ivh_afl_hook_after_wake(void *arg)
{
        (void)arg;
        wait_enter();
        if (extend_wait)
                extend();
}

static void grab_lock(struct thread_data *tdata, struct data *data)
{
        unsigned long long start_wait, start, end, delta;
        unsigned long long end_wait;
        unsigned long long start_wait_ns, start_ns, end_ns;
        unsigned long long start_active_ns, end_active_ns;
        unsigned long long acq_ns = 0;
        int lock_ret;
        int cs_cpu_start, cs_cpu_end, cs_cpu_hold;
        unsigned long long steal_hold[VCAP_MAX_CPUS] = {0};
        unsigned long long steal_before[VCAP_MAX_CPUS] = {0};
        unsigned long long steal_after[VCAP_MAX_CPUS] = {0};

        g_tdata = tdata;

        {
                /*
                 * 2026-10-01: migration_count used to be incremented on every
                 * syscall that was MADE, with no check that the thread
                 * actually moved -- so "Total migrations" was a count of
                 * attempts that got past the advisory danger bit. Every
                 * figure derived from it (cost per migration, migrations per
                 * iteration, the eval tables' migration columns) was really
                 * per-attempt. Bracket the syscall with sched_getcpu() and
                 * count the CPU change; keep the attempt count separately so
                 * the old number is still recoverable.
                 */
                int cpu_pre = sched_getcpu();
                unsigned long long _t0 = get_time_ns();
                int made = ivh_cs_enter_checked();
                unsigned long long _dt = get_time_ns() - _t0;
                int cpu_post = sched_getcpu();

                if (!made) {
                        tdata->syscall_skipped_count++;
                } else {
                        tdata->syscall_made_count++;
                        tdata->sum_migration_ns += _dt;
                        if (_dt > tdata->max_migration_ns)
                                tdata->max_migration_ns = _dt;
                        if (_dt > 1000000ULL) /* >1ms = got stuck in schedule() */
                                tdata->slow_migration_count++;
                        if (cpu_pre != cpu_post && cpu_pre >= 0 && cpu_post >= 0) {
                                tdata->migration_count++;
                                tdata->sum_moved_ns += _dt;
                                if (cpu_post >= 8)
                                        tdata->migration_to_healthy++;
                        }
                }
        }

        start_wait = get_time();
        start_wait_ns = get_time_ns();

        /*
         * Moved outside the critical section (design doc sec 6.2): the old
         * code read this via a pread()+strtok/sscanf parse of an 8KB proc
         * buffer AFTER acquiring the lock, i.e. inside the very ~13us CS
         * this project spent this whole session calibrating -- tens of
         * microseconds of instrumentation overhead inside a 13us window,
         * and a real syscall (a scheduling point) taken while HOLDING the
         * lock, which the new heartbeat would (correctly, but
         * misleadingly) detect as the holder having stalled. This is a
         * cumulative per-CPU counter already filtered at >100us, so
         * widening the sampled window to include the wait costs only a
         * little extra noise, not correctness.
         */
        cs_cpu_start = sched_getcpu();
        read_vcap_steal(steal_before);

        wait_enter(); /* entering spin/wait region; wait_counter > 0 until lock acquired */
        if (extend_wait)
                extend();

        tracefs_printf(NULL, "Grab lock\n");
        start = get_time();

        lock_ret = ivh_afl_lock(&data->lock);
        if (lock_ret != IVH_AFL_OK) {
                if (extend_wait && unextend())
                        tdata->extended++;
                wait_exit(); /* abandoned wait at shutdown */
                return;
        }
        /*
         * The acquire instant. Taken here and not at start_ns below,
         * because everything between the two is work done while already
         * holding the lock and therefore on the serialized path.
         */
        acq_ns = get_time_ns();
        {
                unsigned long long rel =
                        __atomic_load_n(&data->last_release_ns, __ATOMIC_RELAXED);
                /* rel == 0 only for the very first acquisition of the run */
                if (rel && acq_ns > rel) {
                        unsigned long long h = acq_ns - rel;
                        tdata->handoff_sum_ns += h;
                        tdata->handoff_count++;
                        if (h > tdata->handoff_max_ns)
                                tdata->handoff_max_ns = h;
                }
        }

        /*
         * Arm the rseq timeslice extension for the CRITICAL SECTION, here,
         * AFTER the acquisition -- not before the wait.
         *
         * This is parity with NHextend3.c, and its absence was a real
         * (measured) defect in this port, not a style difference.
         * NHextend3's acquire loop arms `extend()` immediately before each
         * cmpxchg attempt and disarms it (`unextend()`, yielding if the
         * kernel had already granted a deferral) the instant that attempt
         * fails -- so a spinning waiter there holds NO extension request,
         * and the request the eventual holder carries into its CS was armed
         * microseconds earlier and is unspent.
         *
         * This file used to arm it once before calling ivh_afl_lock(), which
         * on a contended lock can spin or sleep for MILLISECONDS before
         * returning. The request sat armed across that whole wait, the
         * kernel spent it there (granting the waiter a deferral it had no
         * use for and then setting the yield-owed bit, which this path never
         * answers), and the thread entered its 1.6ms critical section with
         * nothing left to defer preemption with.
         *
         * Measured cost at loop_spin=600000, 8 threads, 10s: CS *active*
         * (on-CPU) time was identical between the two binaries (936us vs
         * 929us, +0.7%), but CS *overall* time was 1,048us vs 948us -- i.e.
         * the holder spent 113us per CS off-CPU here against 19us in
         * NHextend3, and CS cycles with a >100us preemption ran 3.12% vs
         * 1.23%. That entire gap is on the serialized critical path, and it
         * grew with thread count (the longer the wait, the more certainly
         * the extension was spent before the CS began), which is exactly the
         * shape of the 8/4/2-thread regression.
         *
         * Note this is the benchmark's own pre-existing rseq cr_counter
         * mechanism, used here exactly as NHextend3.c uses it -- no kernel
         * migration-engine signal is consulted.
         */
        if (!extend_wait)
                extend();

        wait_exit(); /* lock acquired; no longer spinning/waiting */
        /*
         * 2026-10-01: steal_before/cs_cpu_start above are sampled BEFORE
         * wait_enter(), so the original window is wait+CS. At
         * loop_spin=600000 with 16 threads each iteration takes ~25ms of
         * which the CS is ~1.6ms, so ~94% of the scored window is queue
         * wait -- which migration does not and cannot protect. Sample again
         * HERE, so the hold alone can be scored. Both are reported.
         */
        cs_cpu_hold = sched_getcpu();   /* vDSO, ~20ns -- kept either way */
        if (!no_hold_steal)
                read_vcap_steal(steal_hold);
        end_wait = get_time();
        start_ns = get_time_ns();
        start_active_ns = get_time_cputime();
        if (start_ns > acq_ns) {
                unsigned long long pre = start_ns - acq_ns;
                tdata->preamble_sum_ns += pre;
                if (pre > tdata->preamble_max_ns)
                        tdata->preamble_max_ns = pre;
        }

        tracefs_printf(NULL, "Have lock!\n");
        delta = end_wait - start_wait;
        if (!tdata->total_wait || tdata->max_wait < delta)
                tdata->max_wait = delta;
        if (!tdata->total_wait || tdata->min_wait > delta)
                tdata->min_wait = delta;
        tdata->total_wait += delta;

        data->x++;

        /*
         * Loop.
         *
         * The heartbeat republish interval is gated HERE, by this loop's own
         * counter, rather than by calling ivh_afl_beat() (whose self-gating
         * __thread counter is the right API for a caller with no loop of its
         * own, and the wrong one for this caller -- see the cost note on
         * ivh_afl_beat() in ivh_adaptive_futex_lock.h).
         *
         * Why this matters, measured 2026-09-13: ivh_afl_beat() compiled to a
         * %fs-relative LOAD *and STORE* of its gate counter on every single
         * iteration, immediately before the next sfence -- and sfence has to
         * drain that store, so the per-iteration tax was not the couple of
         * uops it looks like on paper. At loop_spin=600000 that is 600k
         * drained stores per critical section, a fixed cost paid whether or
         * not the adaptive mechanism is ever used. Under real contention (16
         * threads) it is trivial next to what the mechanism saves; at low
         * thread counts there is no stalled holder left to catch, so only the
         * tax remained -- it showed up as a -7% .. -19% throughput regression
         * against plain NHextend3 at 8/4/2/1 threads.
         *
         * next_beat is a plain local whose address never escapes, so it stays
         * in a register across wmb()'s memory clobber: the per-iteration delta
         * against NHextend3.c's own `for (i...) wmb();` loop is now two
         * register-only uops (cmp + a not-taken jcc) and zero memory traffic.
         * Deliberately NOT restructured into a chunked/nested loop (which
         * would also hoist the `loop_spin` global reload out of the inner
         * loop): that would make this CS loop genuinely CHEAPER than the
         * baseline's and flatter the very comparison this benchmark exists to
         * make. The republish schedule is unchanged -- one publish every
         * IVH_AFL_BEAT_INTERVAL iterations, exactly as before.
         */
        {
                int next_beat = IVH_AFL_BEAT_INTERVAL;

                for (int i = 0; i < loop_spin; i++) {
                        wmb();
                        if (__builtin_expect(i == next_beat, 0)) {
                                ivh_afl_publish_heartbeat(&data->lock);
                                next_beat += IVH_AFL_BEAT_INTERVAL;
                        }
                }
        }

        __atomic_store_n(&data->last_release_ns, get_time_ns(), __ATOMIC_RELAXED);
        ivh_afl_unlock(&data->lock);
        end = get_time();
        end_ns = get_time_ns();
        end_active_ns = get_time_cputime();

        cs_cpu_end = sched_getcpu();
        read_vcap_steal(steal_after);
        {
                /* >100us floor matches the kernel's own >1ms rq->preemptions
                 * filter's order of magnitude, filtering background
                 * steal-counter noise rather than any nonzero delta. */
                tdata->cs_start_total++;
                if (cs_cpu_start >= 8)
                        tdata->cs_start_healthy++;
                if (!no_hold_steal) {
                        long long dh = (long long)steal_after[cs_cpu_hold]
                                     - (long long)steal_hold[cs_cpu_hold];
                        if (cs_cpu_hold != cs_cpu_end)
                                dh += (long long)steal_after[cs_cpu_end]
                                    - (long long)steal_hold[cs_cpu_end];
                        if (dh > 100000LL)
                                tdata->hold_preempted_count++;
                }
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
        {
                int bad0 = (cs_cpu_start >= 0 && cs_cpu_start < 8);
                int bad1 = (cs_cpu_hold  >= 0 && cs_cpu_hold  < 8);
                if (bad0 && bad1) {
                        tdata->wait_stuck_cnt++;
                        tdata->wait_stuck_ns += tdata->last_wait_ns;
                } else if (bad0) {
                        tdata->wait_resc_cnt++;
                        tdata->wait_resc_ns += tdata->last_wait_ns;
                } else {
                        tdata->wait_good_cnt++;
                        tdata->wait_good_ns += tdata->last_wait_ns;
                }
        }
        if (tdata->last_cs_ns > tdata->max_cs_ns)
                tdata->max_cs_ns = tdata->last_cs_ns;
        if (!tdata->min_cs_ns || tdata->last_cs_ns < tdata->min_cs_ns)
                tdata->min_cs_ns = tdata->last_cs_ns;
        if (tdata->last_cs_active_ns > tdata->max_cs_active_ns)
                tdata->max_cs_active_ns = tdata->last_cs_active_ns;
        tdata->sum_cs_ns        += tdata->last_cs_ns;
        tdata->sum_cs_active_ns += tdata->last_cs_active_ns;
        tdata->cs_count++;
        /* count CS cycles where the holder was preempted >100us during the hold */
        if ((long long)tdata->last_cs_ns - (long long)tdata->last_cs_active_ns > 100000LL)
                tdata->cs_preempted_count++;
        if (!no_rseq && rseq_map) {
                rseq_map->last_cs_overall_ns   =
                        cs_min_pinned_ns ? cs_min_pinned_ns :
                        (use_cs_min && tdata->min_cs_ns) ? tdata->min_cs_ns :
                        tdata->last_cs_ns;
                rseq_map->last_cs_active_ns    = tdata->last_cs_active_ns;
                rseq_map->last_wait_overall_ns = tdata->last_wait_ns;
        }

        if (unextend())
                tdata->extended++;

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
                /* 100us + cpu * 27us  -- NHEXTEND_POST_SLEEP=0 removes it, which
                 * matters at short CS: at loop_spin=50000 the CS is ~110us while
                 * this sleep averages ~300us across 16 threads, so most of the
                 * measured wait is this sleep rather than lock contention. */
                if (post_sleep) {
                        if (post_spin)
                                do_spin(100 + tdata->cpu * 27);
                        else
                                do_sleep(100 + tdata->cpu * 27);
                }
                rmb();
        }
#ifdef IVH_AFL_STATS
        __atomic_add_fetch(&g_afl_totals.fast_acquires, ivh_afl_stats.fast_acquires, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_afl_totals.slow_acquires, ivh_afl_stats.slow_acquires, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_afl_totals.sleeps, ivh_afl_stats.sleeps, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_afl_totals.wakes_issued, ivh_afl_stats.wakes_issued, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_afl_totals.wakes_skipped, ivh_afl_stats.wakes_skipped, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_afl_totals.eagain, ivh_afl_stats.eagain, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_afl_totals.eintr, ivh_afl_stats.eintr, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_afl_totals.timeouts, ivh_afl_stats.timeouts, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_afl_totals.stale_detections, ivh_afl_stats.stale_detections, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_afl_totals.stale_recheck_aborts, ivh_afl_stats.stale_recheck_aborts, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_afl_totals.wakes_woke_nobody, ivh_afl_stats.wakes_woke_nobody, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_afl_totals.wakes_woke_someone, ivh_afl_stats.wakes_woke_someone, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_afl_totals.total_threads_woken, ivh_afl_stats.total_threads_woken, __ATOMIC_RELAXED);
#endif
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
                {
                        const char *ps = getenv("NHEXTEND_POST_SLEEP");
                        if (ps)
                                post_sleep = atoi(ps);
                        const char *pp = getenv("NHEXTEND_POST_SPIN");
                        if (pp)
                                post_spin = atoi(pp);
                }
                {
                        const char *nh = getenv("NHEXTEND_NO_HOLD_STEAL");
                        if (nh)
                                no_hold_steal = atoi(nh);
                }
                {
                        const char *cm = getenv("NHEXTEND_CS_MIN");
                        if (cm)
                                use_cs_min = atoi(cm);
                        const char *cn = getenv("NHEXTEND_CS_MIN_NS");
                        if (cn)
                                cs_min_pinned_ns = strtoull(cn, NULL, 10);
                }

                const char *ns = getenv("NHEXTEND_IVH_NO_SKIP");
                if (ns && atoi(ns) != 0)
                        ivh_force_syscall = 1;
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
        ivh_afl_global_init();
        ivh_afl_init(&data.lock);
        ivh_afl_set_abort_flag(&data.lock, (const volatile bool *)&data.done);
        ivh_afl_set_hooks(&data.lock, ivh_afl_hook_before_sleep, ivh_afl_hook_after_wake, NULL);

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
        /*
         * Must run before joining: a thread blocked in FUTEX_WAIT does not
         * poll data.done at all, so without this wake, pthread_join() below
         * hangs on any thread that happened to be asleep at end-of-run --
         * i.e. on essentially every run that had any real contention. The
         * IVH_AFL_WAIT_TIMEOUT_NS backstop in the header covers the residual
         * race (a thread between its last abort check and its FUTEX_WAIT
         * syscall entry when this runs); this wake covers everyone else
         * immediately instead of waiting out that timeout.
         */
        ivh_afl_shutdown_wake(&data.lock);
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

                /*
                 * lock_cycle = handoff + preamble + hold. Printed as an
                 * additive decomposition of duration/iterations so the
                 * terms can be checked to sum: a residual that does not
                 * close means one of them is mismeasured.
                 */
                {
                        unsigned long long hs = 0, hc = 0, hm = 0, ps = 0, pm = 0;
                        for (i = 0; i < num_threads; i++) {
                                hs += data.tdata[i].handoff_sum_ns;
                                hc += data.tdata[i].handoff_count;
                                ps += data.tdata[i].preamble_sum_ns;
                                if (data.tdata[i].handoff_max_ns > hm)
                                        hm = data.tdata[i].handoff_max_ns;
                                if (data.tdata[i].preamble_max_ns > pm)
                                        pm = data.tdata[i].preamble_max_ns;
                        }
                        unsigned long long cmin = 0;
                        for (i = 0; i < num_threads; i++)
                                if (data.tdata[i].min_cs_ns &&
                                    (!cmin || data.tdata[i].min_cs_ns < cmin))
                                        cmin = data.tdata[i].min_cs_ns;
                        printf("\nCSmin observed (min hold across threads): %llu ns%s\n", cmin,
                               cs_min_pinned_ns ? "  [PINNED value published]" :
                               use_cs_min ? "  [published to rseq]" : "  [NOT published; last CS used]");
                        {
                        unsigned long long sc=0,sn=0,rc=0,rn=0,gc=0,gn=0;
                        for (i = 0; i < num_threads; i++) {
                                sc += data.tdata[i].wait_stuck_cnt; sn += data.tdata[i].wait_stuck_ns;
                                rc += data.tdata[i].wait_resc_cnt;  rn += data.tdata[i].wait_resc_ns;
                                gc += data.tdata[i].wait_good_cnt;  gn += data.tdata[i].wait_good_ns;
                        }
                        unsigned long long tot = sc+rc+gc, totns = sn+rn+gn;
                        printf("\nWait classified by vCPU health (mid-spin migration headroom):\n");
                        printf("  started cpu<8, ACQUIRED on cpu<8 : %llu (%.2f%%)  avg wait %llu ns  %.2f%% of all wait\n",
                               sc, tot?100.0*sc/tot:0, sc?sn/sc:0, totns?100.0*sn/totns:0);
                        printf("  started cpu<8, acquired on cpu>=8: %llu (%.2f%%)  avg wait %llu ns\n",
                               rc, tot?100.0*rc/tot:0, rc?rn/rc:0);
                        printf("  started cpu>=8                   : %llu (%.2f%%)  avg wait %llu ns\n",
                               gc, tot?100.0*gc/tot:0, gc?gn/gc:0);
                        }
                        printf("\nLock cycle decomposition (ns):\n");
                        printf("  handoff  release -> next acquire  : avg %llu  max %llu  (n=%llu)\n",
                               hc ? hs / hc : 0, hm, hc);
                        printf("  preamble acquire -> timed hold    : avg %llu  max %llu\n",
                               g_cnt ? ps / g_cnt : 0, pm);
                        printf("  hold     timed CS (avg overall)   : avg %llu\n",
                               g_sum_ov / g_cnt);
                        printf("  sum of the three                  : %llu\n",
                               (hc ? hs / hc : 0) + (g_cnt ? ps / g_cnt : 0) + g_sum_ov / g_cnt);
                }
                if (violations)
                        printf("  WARNING: %d violation(s)\n", violations);
                else
                        printf("  Invariant OK: max_overall >= max_active on all threads\n");
                printf("\n");

                /* IVH migration stats — measures ivh_cs_enter() duration, not CS hold */
                unsigned long long g_mig_count = 0, g_mig_sum = 0, g_mig_max = 0;
                unsigned long long g_moved_sum = 0, g_to_healthy = 0;
                unsigned long long g_slow = 0, g_cs_preempted = 0;
                unsigned long long g_host_preempted = 0, g_host_migrated = 0;
                unsigned long long g_syscall_skipped = 0, g_syscall_made = 0;
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
                        g_moved_sum  += t->sum_moved_ns;
                        g_to_healthy += t->migration_to_healthy;
                        g_mig_sum   += t->sum_migration_ns;
                        if (t->max_migration_ns > g_mig_max) g_mig_max = t->max_migration_ns;
                        g_slow      += t->slow_migration_count;
                        g_cs_preempted += t->cs_preempted_count;
                        g_host_preempted += t->host_preempted_count;
                        g_host_migrated  += t->host_preempted_migrated_count;
                        g_syscall_skipped   += t->syscall_skipped_count;
                        g_syscall_made      += t->syscall_made_count;
                }
                unsigned long long g_mig_cnt = g_mig_count ? g_mig_count : 1;
                unsigned long long g_cs_cnt  = g_count ? g_count : 1;
                printf("\n");
                printf("  Syscall ATTEMPTS    : %llu  (passed the advisory danger bit)\n",
                       g_syscall_made);
                printf("  Total migrations    : %llu  (of those, CPU actually changed: %.1f%%)\n",
                       g_mig_count,
                       g_syscall_made ? 100.0 * g_mig_count / g_syscall_made : 0.0);
                printf("    ...to cpu>=8      : %llu  (%.1f%% of migrations)\n", g_to_healthy,
                       g_mig_count ? 100.0 * g_to_healthy / g_mig_count : 0.0);
                printf("  Avg syscall (all)   : %llu ns\n",
                       g_syscall_made ? g_mig_sum / g_syscall_made : 0);
                printf("  Avg syscall (moved) : %llu ns\n", g_moved_sum / g_mig_cnt);
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
                        unsigned long long sh=0, st=0;
                        for (i = 0; i < num_threads; i++) {
                                sh += data.tdata[i].cs_start_healthy;
                                st += data.tdata[i].cs_start_total;
                        }
                {
                        unsigned long long hp=0, ht=0;
                        for (i = 0; i < num_threads; i++) {
                                hp += data.tdata[i].hold_preempted_count;
                                ht += data.tdata[i].cs_start_total;
                        }
                        if (no_hold_steal)
                                printf("  HOLD-only preempted      : UNAVAILABLE (NHEXTEND_NO_HOLD_STEAL=1)\n");
                        else
                        printf("  HOLD-only preempted      : %llu / %llu  (%.4f%%)\n",
                               hp, ht, ht ? 100.0*hp/ht : 0.0);
                }
                        printf("  CS started on cpu>=8     : %llu / %llu  (%.2f%%)\n",
                               sh, st, st ? 100.0*sh/st : 0.0);
                }
                printf("  Host-preempted CS cycles : %llu / %llu  (%.4f%%)\n",
                       g_host_preempted, g_count,
                       100.0 * g_host_preempted / g_cs_cnt);
                printf("  (of which, thread migrated mid-CS): %llu\n", g_host_migrated);
                printf("\n");
        }

        printf("Ran for %lld times\n", data.x);
        printf("Total wait time: %llu.%06llu  (avg: %llu.%06llu)\n", secs, total_wait - sec2usec(secs),
                                avg_secs, avg_wait - sec2usec(avg_secs));
        printf("Total contention: %lld\n", total_contention);
        printf("Total extended: %lld\n", total_extended);
        printf("      max wait: %lld\n", max_wait);
        printf("           max: %lld (avg: %llu)\n", max, avg_held);
#ifdef IVH_AFL_STATS
        printf("\nivh_afl stats (summed across all worker threads):\n");
        printf("  fast_acquires (0->1, wake-skipping path) : %" PRIu64 "\n", g_afl_totals.fast_acquires);
        printf("  slow_acquires (0->2 exchange)            : %" PRIu64 "\n", g_afl_totals.slow_acquires);
        printf("  sleeps (FUTEX_WAIT entered)               : %" PRIu64 "\n", g_afl_totals.sleeps);
        printf("  wakes_issued (unlock found state==2)      : %" PRIu64 "\n", g_afl_totals.wakes_issued);
        printf("  wakes_skipped (unlock found state==1)     : %" PRIu64 "\n", g_afl_totals.wakes_skipped);
        printf("  eagain/eintr/timeouts                     : %" PRIu64 " / %" PRIu64 " / %" PRIu64 "\n",
               g_afl_totals.eagain, g_afl_totals.eintr, g_afl_totals.timeouts);
        printf("  stale_detections                          : %" PRIu64 "\n", g_afl_totals.stale_detections);
        printf("  stale_recheck_aborts (sleep avoided)       : %" PRIu64 "\n", g_afl_totals.stale_recheck_aborts);
        printf("  wakes_woke_nobody (real, direct measure)  : %" PRIu64 "\n", g_afl_totals.wakes_woke_nobody);
        printf("  wakes_woke_someone                        : %" PRIu64 "\n", g_afl_totals.wakes_woke_someone);
        printf("  total_threads_woken                       : %" PRIu64 "\n", g_afl_totals.total_threads_woken);
#endif
        return 0;
}
