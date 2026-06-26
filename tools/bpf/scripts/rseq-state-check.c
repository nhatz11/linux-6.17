/*
 * rseq-state-check.c — verify rseq_sched_state is readable/writable by kernel.
 *
 * Build:
 *   gcc -O2 -o /tmp/rseq-state-check \
 *       /home/nick/kernels/linux-6.17-rseqport/tools/bpf/scripts/rseq-state-check.c
 *
 * Run with IVH OFF for independent measurement.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#ifndef __NR_rseq
#define __NR_rseq 334
#endif
#define RSEQ_SIG          0x53053053
#define RSEQ_FLAG_UNREGISTER 1

/*
 * Full struct rseq layout for linux-6.17-rseqport.
 * sched_state_ptr is at offset 64; struct end[] is at offset 72.
 * Do NOT use __attribute__((aligned(32))) here — that would pad sizeof
 * to 96 while keeping field offsets at 64, making sizeof misleading.
 * We only need it for field-offset access.
 */
struct my_rseq {
    uint32_t cpu_id_start;         /*  0 */
    uint32_t cpu_id;               /*  4 */
    uint64_t rseq_cs;              /*  8 */
    uint32_t flags;                /* 16 */
    uint32_t node_id;              /* 20 */
    uint32_t mm_cid;               /* 24 */
    uint32_t cr_counter;           /* 28 */
    uint32_t wait_counter;         /* 32 */
    uint32_t _pad0;                /* 36 */
    uint64_t last_cs_overall_ns;   /* 40 */
    uint64_t last_cs_active_ns;    /* 48 */
    uint64_t last_wait_overall_ns; /* 56 */
    uint64_t sched_state_ptr;      /* 64 */
    /* end[] at offset 72 */
};

#define RSEQ_LEN 72  /* offsetof(struct rseq, end) with sched_state_ptr */

/*
 * struct rseq_sched_state — kernel writes version=0 and state on sched events.
 * bit 0 of state = RSEQ_SCHED_STATE_FLAG_ON_CPU.
 */
struct rseq_sched_state {
    uint32_t version;  /* kernel writes 0 at registration */
    uint32_t state;    /* bit 0: ON_CPU */
    uint32_t tid;
} __attribute__((aligned(4)));

#define ON_CPU_FLAG 1u

/* glibc exports these for the per-thread rseq TLS location */
extern int __rseq_offset;
extern unsigned int __rseq_size;

static const char *state_str(uint32_t s)
{
    return (s & ON_CPU_FLAG) ? "ON_CPU (1)" : "off-cpu (0)";
}

int main(void)
{
    pid_t tid = (pid_t)syscall(SYS_gettid);

    /*
     * Locate glibc's per-thread rseq struct in TLS.
     * The kernel registered it at glibc startup; task->rseq == glibc_rs.
     */
    struct my_rseq *glibc_rs = (struct my_rseq *)
        ((char *)__builtin_thread_pointer() + __rseq_offset);

    printf("=== rseq_sched_state read/write check ===\n\n");

    /* --- Layout --- */
    size_t ssp_off = (size_t)((char*)&glibc_rs->sched_state_ptr - (char*)glibc_rs);
    printf("[0] Layout\n");
    printf("    __rseq_offset         = %d\n", __rseq_offset);
    printf("    __rseq_size           = %u  (expect 72)\n", __rseq_size);
    printf("    glibc_rs (TLS addr)   = %p\n", (void*)glibc_rs);
    printf("    &sched_state_ptr      = offset %zu  (expect 64)\n", ssp_off);
    printf("    sizeof(my_rseq)       = %zu  (expect 72 — no trailing pad)\n\n",
           sizeof(*glibc_rs));

    /* Allocate rseq_sched_state in a faulted-in page (pagefault_disable path). */
    struct rseq_sched_state *ss = mmap(NULL, 4096, PROT_READ|PROT_WRITE,
                                       MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (ss == MAP_FAILED) { perror("mmap"); return 1; }
    memset(ss, 0, 4096);

    ss->tid     = (uint32_t)tid;
    ss->version = 0xdeadbeef;  /* canary — kernel must overwrite with 0 */
    ss->state   = 0;

    /* Touch ss to guarantee the page is faulted in before registration.
     * The kernel writes state with pagefault_disable() and silently skips
     * if the page is not present. */
    volatile uint32_t dummy = ss->version; (void)dummy;

    /* --- Write sched_state_ptr and re-register --- */
    printf("[1] Registering with sched_state_ptr\n");
    printf("    Unregistering glibc's TLS rseq (addr=%p, size=%u)...\n",
           (void*)glibc_rs, __rseq_size);

    glibc_rs->sched_state_ptr = (uint64_t)(uintptr_t)ss;

    long ret = syscall(__NR_rseq, glibc_rs, __rseq_size,
                       RSEQ_FLAG_UNREGISTER, RSEQ_SIG);
    if (ret != 0) {
        printf("    unregister: ret=%ld errno=%s\n", ret, strerror(errno));
        /* Continue anyway — maybe already unregistered */
    } else {
        printf("    unregister OK\n");
    }

    ret = syscall(__NR_rseq, glibc_rs, RSEQ_LEN, 0, RSEQ_SIG);
    if (ret == 0) {
        printf("    register OK (len=%d, sched_state_ptr=%p)\n",
               RSEQ_LEN, (void*)ss);
    } else {
        printf("    register: ret=%ld errno=%s\n", ret, strerror(errno));
    }
    printf("\n");

    /* --- version written by kernel --- */
    printf("[2] Kernel-write at registration\n");
    printf("    ss->version = 0x%08x  (expect 0x00000000)\n", ss->version);
    printf("    ss->state   = 0x%08x  = %s\n", ss->state, state_str(ss->state));
    printf("    ss->tid     = %u\n\n", ss->tid);

    /* --- ON_CPU flag while running --- */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32_t s_now = __atomic_load_n(&ss->state, __ATOMIC_ACQUIRE);
    printf("[3] state while on-CPU: 0x%x = %s  (expect ON_CPU)\n\n",
           s_now, state_str(s_now));

    /* --- Yield loop --- */
    printf("[4] 20x sched_yield() — ON_CPU re-set on return:\n");
    int on_cpu_count = 0;
    for (int i = 0; i < 20; i++) {
        sched_yield();
        uint32_t s = __atomic_load_n(&ss->state, __ATOMIC_ACQUIRE);
        if (s & ON_CPU_FLAG) on_cpu_count++;
        printf("    yield %2d: state=0x%x (%s)\n", i+1, s, state_str(s));
    }
    printf("    ON_CPU on return: %d/20  (expect 20/20)\n\n", on_cpu_count);

    /* --- Summary --- */
    printf("=== Summary ===\n");
    printf("  __rseq_size covers sched_state_ptr (>=72) : %s\n",
           __rseq_size >= 72 ? "PASS" : "FAIL");
    printf("  sched_state_ptr at offset 64               : %s\n",
           ssp_off == 64 ? "PASS" : "FAIL");
    printf("  version=0 written by kernel at register    : %s\n",
           ss->version == 0 ? "PASS" : "FAIL");
    printf("  state=ON_CPU while running                 : %s\n",
           (s_now & ON_CPU_FLAG) ? "PASS" : "FAIL");
    printf("  state re-set ON_CPU after yield            : %s (%d/20)\n",
           on_cpu_count >= 18 ? "PASS" : "WARN", on_cpu_count);

    munmap(ss, 4096);
    return 0;
}
