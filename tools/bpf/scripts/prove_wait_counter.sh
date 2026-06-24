#!/usr/bin/env bash
# prove_wait_counter.sh
#
# Proves the kernel can read wait_counter from userspace rseq memory.
#
# The kernel read lives at fair.c:12953-12956:
#   if (rq->curr->rseq && rq->curr->rseq_len >= 36) {
#       u32 wc;
#       copy_from_user_nofault(&wc, &rq->curr->rseq->wait_counter, 4);
#       curr_user_waiter = (wc & ~3u) != 0;
#   }
#
# This script does NOT depend on running_migration() firing.
# It proves the read mechanism directly using bpftrace.
#
# HOW IT WORKS
# ------------
# 1. A small C program sets wait_counter = 4 in its own rseq TLS struct
#    (same atomic store that wait_enter() uses), then calls sched_yield()
#    in a loop for 5 seconds.
#
# 2. bpftrace hooks the sched_yield syscall entry point.  At syscall entry,
#    curtask == the process that called sched_yield, so curtask->rseq
#    points at the same TLS struct the setter just wrote to.
#
# 3. bpftrace reads (curtask->rseq + 32) using uptr() which forces
#    bpf_probe_read_user() — exactly the same cross-privilege user-memory
#    read that copy_from_user_nofault() performs in fair.c.
#
# 4. When bpftrace prints wait_counter=0x4 / depth=1, that proves:
#    the kernel can read that value from userspace memory using
#    the same mechanism the IVH kernel code uses.
#
# Usage: sudo bash prove_wait_counter.sh
# Does NOT require MY_ivh_atc to be running.
# Requires: bpftrace >= 0.20 (for uptr()), gcc

set -euo pipefail

die() { echo "ERROR: $*" >&2; exit 1; }
hdr() { echo; echo "══════════════════════════════════════════════════════"; echo "  $*"; echo "══════════════════════════════════════════════════════"; }

command -v bpftrace || die "bpftrace not in PATH"
command -v gcc      || die "gcc not in PATH"

# ── Build the setter ─────────────────────────────────────────────────────────
hdr "Building wait_counter setter"

SETTER_SRC=$(mktemp /tmp/wc_setter.XXXXXX.c)
SETTER_BIN=$(mktemp /tmp/wc_setter.XXXXXX)
trap 'rm -f "$SETTER_SRC" "$SETTER_BIN"' EXIT

cat > "$SETTER_SRC" << 'CSRC'
/*
 * wait_counter proof setter:
 *   1. Locate this thread's rseq struct via __rseq_offset.
 *   2. Atomically write wait_counter = 4 (encoding: bits[31:2] = depth,
 *      so depth=1 → value=4; same as wait_enter() in NHextend).
 *   3. Loop on sched_yield() for 5 seconds — gives bpftrace a syscall
 *      entry point where curtask == this thread, so bpftrace (and the
 *      kernel) can read our wait_counter via bpf_probe_read_user.
 */
#include <stdio.h>
#include <stdint.h>
#include <sched.h>
#include <unistd.h>
#include <time.h>
#include <sys/rseq.h>

struct my_rseq {
    uint32_t cpu_id_start;   /* offset  0 */
    uint32_t cpu_id;         /* offset  4 */
    uint64_t rseq_cs;        /* offset  8 */
    uint32_t flags;          /* offset 16 */
    uint32_t node_id;        /* offset 20 */
    uint32_t mm_cid;         /* offset 24 */
    uint32_t cr_counter;     /* offset 28 */
    uint32_t wait_counter;   /* offset 32 ← proof target */
};

int main(void)
{
    if (__rseq_size < 36) {
        fprintf(stderr,
            "rseq_size=%u (need >= 36 to cover wait_counter at offset 32)\n",
            __rseq_size);
        return 1;
    }

    struct my_rseq *rs = (struct my_rseq *)
        ((char *)__builtin_thread_pointer() + __rseq_offset);

    printf("PID           = %d\n", getpid());
    printf("rseq_size     = %u bytes\n", __rseq_size);
    printf("&wait_counter = %p  (rseq base + 32)\n", (void *)&rs->wait_counter);
    printf("before set    : wait_counter = 0x%x\n", rs->wait_counter);
    fflush(stdout);

    __atomic_store_n(&rs->wait_counter, 4u, __ATOMIC_RELEASE);

    printf("after set     : wait_counter = 0x%x  (depth=%u)\n",
           rs->wait_counter, rs->wait_counter >> 2);
    printf("\nLooping sched_yield() for 5 s — bpftrace will intercept.\n");
    fflush(stdout);

    time_t stop = time(NULL) + 5;
    while (time(NULL) < stop)
        sched_yield();

    __atomic_store_n(&rs->wait_counter, 0u, __ATOMIC_RELEASE);
    printf("Cleared wait_counter → 0x%x.\n", rs->wait_counter);
    return 0;
}
CSRC

gcc -O2 -o "$SETTER_BIN" "$SETTER_SRC" 2>/dev/null
echo "  Compiled OK."

# ── Run setter + bpftrace together ───────────────────────────────────────────
hdr "Running setter + bpftrace (5 second window)"
echo "  Userspace sets wait_counter=4, kernel reads it at sched_yield entry."
echo

"$SETTER_BIN" &
SETTER_PID=$!
sleep 0.3   # let setter print its header and start yielding

echo "  Setter PID: $SETTER_PID"
echo "  Starting bpftrace..."
echo

# bpftrace notes:
#   - filtered to SETTER_PID so output is not noisy
#   - curtask->rseq is a USERSPACE pointer stored in a kernel struct;
#     uptr() tells bpftrace to use bpf_probe_read_user (not probe_read_kernel)
#     which is exactly what copy_from_user_nofault does in fair.c:12955
sudo bpftrace -e "
tracepoint:syscalls:sys_enter_sched_yield
/ pid == ${SETTER_PID} /
{
    \$t   = (struct task_struct *)curtask;
    \$rseq = (uint64)\$t->rseq;
    \$sz   = (uint32)\$t->rseq_len;

    /* uptr() forces bpf_probe_read_user — same as copy_from_user_nofault */
    \$wc   = *(uint32 *)uptr(\$rseq + 32);

    printf(\"  pid=%-6d  rseq_len=%-3u  wait_counter=0x%08x  depth=%u  %s\n\",
           pid, \$sz, \$wc, \$wc >> 2,
           \$wc != 0 ? \"<-- KERNEL SEES WAITER\" : \"(zero)\");
}" 2>/dev/null &
BT_PID=$!

wait $SETTER_PID 2>/dev/null || true
kill $BT_PID 2>/dev/null || true
wait $BT_PID 2>/dev/null || true

# ── Summary ──────────────────────────────────────────────────────────────────
hdr "What this proves"
cat << 'EOF'
  Every line showing "KERNEL SEES WAITER" confirms:

    The kernel read mechanism for wait_counter works correctly.

  WHAT HAPPENED:
    1. Userspace (setter):  rs->wait_counter = 4   (offset 32 in rseq TLS)
    2. bpftrace (kernel):   *(uint32 *)uptr(task->rseq + 32) = 4
       ^ uptr() uses bpf_probe_read_user, which is the same helper
         that copy_from_user_nofault wraps.

    fair.c:12953-12956 does exactly this read:
      copy_from_user_nofault(&wc, &rq->curr->rseq->wait_counter, 4)
      → curr_user_waiter = (wc & ~3u) != 0   → TRUE when depth >= 1

  WHAT THIS DOES NOT PROVE:
    running_migration() actually fires and reads it during IVH operation.
    That is a separate question about the TRIGGER (bpf_sched_lock_acquire),
    not the read mechanism itself.  running_migration() is currently broken
    as a trigger and is being redesigned (cpu_capacity-based syscall 470).
EOF
echo
