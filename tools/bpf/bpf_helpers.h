/* Local bpf_helpers.h for ivh_atc.bpf.c – self-contained, no libbpf deps */

#ifndef __BPF_HELPERS_H
#define __BPF_HELPERS_H

#ifndef __always_inline
#define __always_inline inline __attribute__((always_inline))
#endif

#define SEC(NAME) __attribute__((section(NAME), used))

/* Declare only helpers your IVH code uses; add more as needed. */
static void *(*bpf_map_lookup_elem)(void *map, const void *key) = (void *)1;
static int (*bpf_map_update_elem)(void *map, const void *key,
                                  const void *value, unsigned long long flags) = (void *)2;
static int (*bpf_map_delete_elem)(void *map, const void *key) = (void *)3;
static long long (*bpf_ktime_get_ns)(void) = (void *)5;
static int (*bpf_trace_printk)(const char *fmt, int fmt_size, ...) = (void *)6;

/* If your code uses other helpers (e.g. bpf_get_current_pid_tgid, ringbuf, etc.),
 * we can add them here as the compiler complains. */

#endif /* __BPF_HELPERS_H */

static void *(*bpf_per_cpu_ptr)(void *percpu_ptr, u32 cpu) = (void *)112;

static int (*bpf_loop)(__u32 iters, int (*callback_fn)(__u32, void *),
                       void *callback_ctx, __u64 flags) = (void *)169;

static void *(*bpf_this_cpu_ptr)(void *percpu_ptr) = (void *)113;
