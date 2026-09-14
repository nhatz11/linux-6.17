#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/rseq.h>
#include <linux/types.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sched.h>
#include <sys/auxv.h>
#include <stdint.h>
#include <stddef.h>

struct rseq_sched_state {
        __u32 version;
        __u32 state;
        __u32 tid;
};

#define RSEQ_SCHED_STATE_FLAG_ON_CPU     (1U << 0)
#define RSEQ_SCHED_STATE_FLAG_IVH_DANGER (1U << 1)

static __thread struct rseq_sched_state ivh_sched_state __attribute__((aligned(64)));
static bool ivh_sched_state_active;

struct rseq_abi {
        __u32 cpu_id_start;
        __u32 cpu_id;
        __u64 rseq_cs;
        __u32 flags;
        __u32 node_id;
        __u32 mm_cid;
        __u32 cr_counter;
        __u32 wait_counter;
        __u32 _pad0;
        __u64 last_cs_overall_ns;
        __u64 last_cs_active_ns;
        __u64 last_wait_overall_ns;
        __u64 sched_state_ptr;
} __attribute__((aligned(4 * sizeof(__u64))));

#define IVH_RSEQ_LEN_NO_SCHED_STATE offsetof(struct rseq_abi, sched_state_ptr)
#define IVH_RSEQ_LEN_SCHED_STATE (offsetof(struct rseq_abi, sched_state_ptr) + sizeof(__u64))

static __thread struct rseq_abi *rseq_map;

static void register_rseq(void)
{
        int ret;
        unsigned long feat_len;
        size_t reg_len;
        bool want_sched_state;

        feat_len = getauxval(AT_RSEQ_FEATURE_SIZE);
        want_sched_state = feat_len >= IVH_RSEQ_LEN_SCHED_STATE;
        reg_len = feat_len ? feat_len : IVH_RSEQ_LEN_NO_SCHED_STATE;

        printf("AT_RSEQ_FEATURE_SIZE = %lu, want_sched_state = %d, reg_len = %zu\n",
               feat_len, want_sched_state, reg_len);

        if (want_sched_state) {
                ivh_sched_state.version = 0;
                ivh_sched_state.state = 0;
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
                        fprintf(stderr, "could not unregister prior rseq: %m\n");
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

int main(void)
{
        rseq_map = (void *)__builtin_thread_pointer() + __rseq_offset;
        register_rseq();

        printf("ivh_sched_state_active = %d\n", ivh_sched_state_active);
        printf("state IMMEDIATELY after registration: version=%u state=0x%x tid=%u\n",
               ivh_sched_state.version, ivh_sched_state.state, ivh_sched_state.tid);

        /* Force a bunch of scheduler activity -- sched_yield()s and a
         * sleep -- so notify-resume fires and rseq_update_cpu_node_id()
         * gets a chance to run and write ON_CPU into our struct. */
        for (int i = 0; i < 200; i++) {
                sched_yield();
                usleep(2000);
        }

        printf("state AFTER 200 yields+sleeps:    version=%u state=0x%x tid=%u\n",
               ivh_sched_state.version, ivh_sched_state.state, ivh_sched_state.tid);
        printf("  ON_CPU bit set?    %s\n",
               (ivh_sched_state.state & RSEQ_SCHED_STATE_FLAG_ON_CPU) ? "YES" : "no");
        printf("  IVH_DANGER bit set? %s\n",
               (ivh_sched_state.state & RSEQ_SCHED_STATE_FLAG_IVH_DANGER) ? "YES" : "no");

        return 0;
}
