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
 * Live per-vCPU host-preemption bit, from /proc/vcap_preempted
 * (vsched_module.c, preempted_read()) -- one ASCII byte per CPU, byte
 * offset == CPU number, so pread(fd, &c, 1, holder_cpu) fetches exactly the
 * holder's bit: one syscall, one KVM steal_time.preempted read module-side,
 * zero parsing.
 *
 * Unlike is_cpu_preempted() (tick-granular heartbeat: clock_preempt is only
 * refreshed by account_process_tick(), so a busy-but-healthy vCPU reads
 * "preempted" for most of every tick window), the KVM preempted byte is set
 * by the HOST at the instant the vCPU is involuntarily scheduled out and
 * cleared at its next VM-entry: 1 for exactly the stolen window,
 * edge-precise, no guest-tick dependence, no idle false-positives.
 *
 * Same thread-local persistent-fd + pread() pattern as read_vcap_steal()
 * above. Returns 1 = holder's vCPU is host-preempted right now, 0 = it is
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
 * Timed hardware wait (tpause), not nanosleep(): nanosleep() HLTs the
 * waiter's own vCPU (inviting the host to steal it right at the
 * wake-to-CS boundary), hands wake-up placement to ordinary CFS (which
 * prefers idle CPUs -- anti-correlated with health), and is a syscall
 * (an incidental IVH trigger while PF_IVH_ELIGIBLE is process-wide).
 * tpause never leaves TASK_RUNNING, never syscalls, never triggers a
 * wake-placement decision, and never gives the host a vCPU-idle window
 * to steal -- it satisfies "must not increase the preempt count" by
 * construction rather than by tuning.
 */
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
 * max_time typically caps a single _tpause() call short of the target, so
 * loop until the real deadline. */
static void tpause_wait_ns(unsigned long long ns)
{
        unsigned long long deadline = __rdtsc() + (ns * tsc_per_ns_x1000) / 1000ULL;

        while (__rdtsc() < deadline)
                _tpause(0 /* C0.2, fast wake */, deadline);
}

static int adaptive_spin_enabled = 0; /* 0 = plain spin, 1 = live holder-preempted bit backoff */
/*
 * Defaults preserved from the older NHextend.c port; overridable at runtime via
 * env vars purely for tuning sweeps (ADAPTIVE_SPIN_BUDGET / _CHUNK_NS / _MAX_NS).
 * With no env set, values are identical to the historical compile-time #defines.
 */
static int adaptive_spin_budget = 2000;          /* spin iterations between holder-health checks */
static unsigned long long adaptive_backoff_chunk_ns = 10000ULL; /* 10us tpause slice */
static unsigned long long adaptive_backoff_max_ns  = 200000ULL; /* per-episode cap */
#define ADAPTIVE_SPIN_BUDGET adaptive_spin_budget
#define ADAPTIVE_BACKOFF_CHUNK_NS adaptive_backoff_chunk_ns
#define ADAPTIVE_BACKOFF_MAX_NS  adaptive_backoff_max_ns

/*
 * Pre-lock migration trigger — call BEFORE attempting grab_lock(), not after.
 * Mirrors ivh_pre_lock() in the kernel spinlock path: if the current vCPU is
 * throttled, the task migrates itself synchronously to a healthy vCPU so the
 * lock is acquired there.  No-op when IVH is not loaded (static key in kernel).
 */
#ifndef __NR_ivh_cs_enter
#define __NR_ivh_cs_enter 470
#endif
static inline void ivh_cs_enter(void)
{
        syscall(__NR_ivh_cs_enter);
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
static int num_threads = -1;
static int num_busy_threads = 0;

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
        unsigned long long                      cs_preempted_count;  /* CS cycles with >100us off-CPU, GUEST-LEVEL proxy */
        /* Host-level steal-time tracking (ground truth, see read_vcap_steal) */
        unsigned long long                      host_preempted_count;
        unsigned long long                      host_preempted_migrated_count;
        /* Adaptive-spin (live holder-preempted bit) tracking */
        unsigned long long                      adaptive_backoffs;
        unsigned long long                      backoff_wait_ns;
        unsigned long long                      recheck_migration_count;
        unsigned long long                      sum_recheck_migration_ns;
        struct data                             *data;
        int                                     cpu;
};

struct data {
        unsigned long long              x;
        unsigned long                   lock;
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

/*
 * Called from inside the busy-wait loop body on every iteration; only
 * actually does anything (a real check, costing one syscall) once per
 * ADAPTIVE_SPIN_BUDGET iterations -- the common case (lock free shortly, or
 * holder healthy) never pays for a syscall at all.
 *
 * The lock word encodes the holder's CPU (0 = free, else holder_cpu + 1 --
 * see grab_lock()'s cmpxchg calls), so a waiter can look up *that specific
 * CPU's* live preemption bit instead of spinning blind.
 *
 * No 100us-floor analog and no N-consecutive-check debounce needed (unlike
 * a steal-delta design): reading 1 is a true statement that the holder is
 * off-CPU at this instant, not a noisy cumulative counter. Micro-preemption
 * cost is bounded by the 10us slice + re-check loop instead.
 */
static int adaptive_backoff_step(struct data *data, unsigned long lock_val,
                                  int *spin_count, struct thread_data *tdata)
{
        int holder_cpu;
        unsigned long long waited = 0;

        if (!adaptive_spin_enabled || lock_val == 0)
                return 0;

        if (++(*spin_count) < ADAPTIVE_SPIN_BUDGET)
                return 0;
        *spin_count = 0;

        holder_cpu = (int)(lock_val - 1);
        if (holder_cpu < 0 || holder_cpu >= VCAP_MAX_CPUS)
                return 0;

        if (holder_vcpu_preempted(holder_cpu) != 1)
                return 0;

        tdata->adaptive_backoffs++;

        /*
         * Ride out the preemption in slices: stop as soon as the lock
         * changes hands (a preempted holder can't release, so a changed
         * word means our sample was already stale), the holder's vCPU is
         * running again, or the stale-holder cap trips.
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

static int backoff_recheck_enabled = 1; /* re-trigger ivh_cs_enter() for the waiter itself after a backoff */

static void grab_lock(struct thread_data *tdata, struct data *data)
{
        unsigned long long start_wait, start, end, delta;
        unsigned long long end_wait;
        unsigned long long start_wait_ns, start_ns, end_ns;
        unsigned long long start_active_ns, end_active_ns;
        unsigned long prev;
        unsigned long my_lock_val;
        int as_spin_count = 0;
        int had_backoff_this_wait = 0;
        bool contention = false;

        {
                unsigned long long _t0 = get_time_ns();
                ivh_cs_enter();
                unsigned long long _dt = get_time_ns() - _t0;
                tdata->migration_count++;
                tdata->sum_migration_ns += _dt;
                if (_dt > tdata->max_migration_ns)
                        tdata->max_migration_ns = _dt;
                if (_dt > 1000000ULL) /* >1ms = migration got stuck in schedule() */
                        tdata->slow_migration_count++;
        }

        start_wait = get_time();
        start_wait_ns = get_time_ns();

        wait_enter(); /* entering spin/wait region; wait_counter > 0 until lock acquired */
        rmb();
        while (data->lock && !data->done) {
                contention = true;
                if (adaptive_backoff_step(data, data->lock, &as_spin_count, tdata))
                        had_backoff_this_wait = 1;
                rmb();
        }

        /*
         * A non-yielding tpause backoff can last long enough that the
         * pre-wait ivh_cs_enter() above is stale: ordinary guest load
         * balancing could have moved this thread mid-wait, or the vCPU
         * health picture could simply have changed. One re-check here
         * gives the thread about to become the NEXT holder a real chance
         * to land on a healthy CPU before its critical section starts --
         * only paid on the rare path that actually backed off.
         */
        if (had_backoff_this_wait && backoff_recheck_enabled) {
                unsigned long long _t0 = get_time_ns();
                ivh_cs_enter();
                unsigned long long _dt = get_time_ns() - _t0;
                tdata->recheck_migration_count++;
                tdata->sum_recheck_migration_ns += _dt;
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
                prev = cmpxchg(&data->lock, 0, my_lock_val);
                if (prev) {
                        contention = true;
                        if (!extend_wait && unextend())
                                tdata->extended++;
                        while (data->lock && !data->done) {
                                if (adaptive_backoff_step(data, data->lock, &as_spin_count, tdata))
                                        had_backoff_this_wait = 1;
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

        while (!data->done) {
                grab_lock(tdata, data);
                /* Make slighty different waits */
                /* 100us + cpu * 27us */
                do_sleep(100 + tdata->cpu * 27);
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
        }

        while ((ch = getopt(argc, argv, "adwvlnb:")) >= 0) {
                switch (ch) {
                        case 'a':
                                adaptive_spin_enabled = 1;
                                break;
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
                                fprintf(stderr, "usage: NHextend3 [-a|-d|-w|-v|-l|-n] [-b busy_threads] [threads]\n"
                                                "  -a: enable adaptive spinning (live holder-preempted bit backoff)\n"
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

        {
                const char *e;
                if ((e = getenv("ADAPTIVE_SPIN_BUDGET")))    adaptive_spin_budget = atoi(e);
                if ((e = getenv("ADAPTIVE_BACKOFF_CHUNK_NS"))) adaptive_backoff_chunk_ns = strtoull(e, NULL, 10);
                if ((e = getenv("ADAPTIVE_BACKOFF_MAX_NS")))  adaptive_backoff_max_ns = strtoull(e, NULL, 10);
        }

        if (adaptive_spin_enabled) {
                calibrate_tsc();
                fprintf(stderr, "[adaptive] budget=%d chunk_ns=%llu max_ns=%llu\n",
                        adaptive_spin_budget, adaptive_backoff_chunk_ns, adaptive_backoff_max_ns);
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
                unsigned long long g_adaptive_backoffs = 0, g_backoff_wait_ns = 0;
                unsigned long long g_recheck_count = 0, g_recheck_sum_ns = 0;
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
                        g_adaptive_backoffs += t->adaptive_backoffs;
                        g_backoff_wait_ns   += t->backoff_wait_ns;
                        g_recheck_count     += t->recheck_migration_count;
                        g_recheck_sum_ns    += t->sum_recheck_migration_ns;
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
                printf("CS holder preemption (off-CPU >100us DURING lock hold, GUEST-LEVEL, ru_nivcsw-style proxy):\n");
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
                printf("Adaptive spin (live holder-preempted bit, %s):\n",
                       adaptive_spin_enabled ? "enabled" : "disabled");
                printf("  Backoffs triggered  : %llu\n", g_adaptive_backoffs);
                if (g_adaptive_backoffs)
                        printf("  Avg backoff wait    : %llu ns\n",
                               g_backoff_wait_ns / g_adaptive_backoffs);
                printf("  Total backoff wait  : %llu ns  (%.2f ms)\n",
                       g_backoff_wait_ns, g_backoff_wait_ns / 1e6);
                printf("  Backoff re-check migrations : %llu%s\n", g_recheck_count,
                       backoff_recheck_enabled ? "" : " (disabled)");
                if (g_recheck_count)
                        printf("  Avg re-check migration ns   : %llu\n",
                               g_recheck_sum_ns / g_recheck_count);
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
