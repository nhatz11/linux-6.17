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

static void *(*bpf_per_cpu_ptr)(void *percpu_ptr, u32 cpu) = (void *)153;

static int (*bpf_loop)(__u32 iters, int (*callback_fn)(__u32, void *),
                       void *callback_ctx, __u64 flags) = (void *)181;

static void *(*bpf_this_cpu_ptr)(void *percpu_ptr) = (void *)154;

static void *(*bpf_get_current_task_btf)(void) = (void *)158;

static long (*bpf_probe_read_kernel)(void *dst, __u32 size, const void *unsafe_ptr) = (void *)113;

static void *(*bpf_ringbuf_reserve)(void *ringbuf, __u64 size, __u64 flags) = (void *)131;
static void (*bpf_ringbuf_submit)(void *data, __u64 flags) = (void *)132;
static void (*bpf_ringbuf_discard)(void *data, __u64 flags) = (void *)133;

/* Map definition helpers — mirrors libbpf bpf_helpers.h */
#ifndef __uint
#define __uint(name, val) int (*name)[val]
#endif
#ifndef __type
#define __type(name, val) typeof(val) *name
#endif

/*
 * BPF_PROG macro — mirrors tools/lib/bpf/bpf_tracing.h.
 *
 * BPF trampoline programs receive a single ctx pointer in r1 (an array of
 * u64 slots, one per hook argument).  The C compiler assigns the hook's C
 * parameter names to r1, r2, … instead, which means:
 *   - rq->field  compiles to *(r1 + big_offset) → btf_ctx_access rejects it
 *   - scalar args (now, idle_cpus) are in r2/r3 which are NOT_INIT at entry
 *
 * BPF_PROG generates an outer wrapper that loads each arg from ctx[] with a
 * size-8 read (validated by btf_ctx_access), then calls the __always_inline
 * body with properly-typed registers.  rq→field accesses then go through a
 * PTR_TO_BTF_ID register, which the verifier allows.
 */
#ifndef ___bpf_concat
#define ___bpf_concat(a, b) a ## b
#endif
#ifndef ___bpf_apply
#define ___bpf_apply(fn, n) ___bpf_concat(fn, n)
#endif
#ifndef ___bpf_nth
#define ___bpf_nth(_, _1, _2, _3, _4, _5, _6, _7, _8, _9, _a, _b, _c, N, ...) N
#endif
#ifndef ___bpf_narg
#define ___bpf_narg(...) \
	___bpf_nth(_, ##__VA_ARGS__, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#endif

#define ___bpf_ctx_cast0()           ctx
#define ___bpf_ctx_cast1(x)          ___bpf_ctx_cast0(), ctx[0]
#define ___bpf_ctx_cast2(x, a...)    ___bpf_ctx_cast1(a), ctx[1]
#define ___bpf_ctx_cast3(x, a...)    ___bpf_ctx_cast2(a), ctx[2]
#define ___bpf_ctx_cast4(x, a...)    ___bpf_ctx_cast3(a), ctx[3]
#define ___bpf_ctx_cast5(x, a...)    ___bpf_ctx_cast4(a), ctx[4]
#define ___bpf_ctx_cast6(x, a...)    ___bpf_ctx_cast5(a), ctx[5]
#define ___bpf_ctx_cast7(x, a...)    ___bpf_ctx_cast6(a), ctx[6]
#define ___bpf_ctx_cast8(x, a...)    ___bpf_ctx_cast7(a), ctx[7]
#define ___bpf_ctx_cast(a...)        ___bpf_apply(___bpf_ctx_cast, ___bpf_narg(a))(a)

#define BPF_PROG(name, args...)						\
name(unsigned long long *ctx);						\
static __always_inline typeof(name(0))					\
____##name(unsigned long long *ctx, ##args);				\
typeof(name(0)) name(unsigned long long *ctx)				\
{									\
	_Pragma("GCC diagnostic push")					\
	_Pragma("GCC diagnostic ignored \"-Wint-conversion\"")		\
	return ____##name(___bpf_ctx_cast(args));			\
	_Pragma("GCC diagnostic pop")					\
}									\
static __always_inline typeof(name(0))					\
____##name(unsigned long long *ctx, ##args)
static long (*bpf_probe_read_user)(void *dst, __u32 size, const void *unsafe_ptr) = (void *)112;
static __u32 (*bpf_get_smp_processor_id)(void) = (void *)8;
static __u64 (*bpf_get_current_pid_tgid)(void) = (void *)14;
static long (*bpf_get_current_comm)(void *buf, __u32 buf_size) = (void *)16;
#ifndef NULL
#define NULL ((void *)0)
#endif
