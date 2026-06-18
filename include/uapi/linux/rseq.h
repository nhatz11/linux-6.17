/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_RSEQ_H
#define _UAPI_LINUX_RSEQ_H

/*
 * linux/rseq.h
 *
 * Restartable sequences system call API
 *
 * Copyright (c) 2015-2018 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#include <linux/types.h>
#include <asm/byteorder.h>

enum rseq_cpu_id_state {
	RSEQ_CPU_ID_UNINITIALIZED		= -1,
	RSEQ_CPU_ID_REGISTRATION_FAILED		= -2,
};

enum rseq_flags {
	RSEQ_FLAG_UNREGISTER = (1 << 0),
};

enum rseq_cs_flags_bit {
	RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT_BIT	= 0,
	RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL_BIT	= 1,
	RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE_BIT	= 2,
};

enum rseq_cs_flags {
	RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT	=
		(1U << RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT_BIT),
	RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL	=
		(1U << RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL_BIT),
	RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE	=
		(1U << RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE_BIT),
};

enum rseq_cr_flags_bit {
	RSEQ_CR_FLAG_IN_CRITICAL_SECTION_BIT	= 0,
	RSEQ_CR_FLAG_KERNEL_REQUEST_SCHED_BIT	= 1,
};

enum rseq_cr_flags {
	RSEQ_CR_FLAG_KERNEL_REQUEST_SCHED =
		(1U << RSEQ_CR_FLAG_KERNEL_REQUEST_SCHED_BIT),
};

#define RSEQ_CR_FLAG_IN_CRITICAL_SECTION_MASK	(~0U >> 1)

/*
 * struct rseq_cs is aligned on 4 * 8 bytes to ensure it is always
 * contained within a single cache-line. It is usually declared as
 * link-time constant data.
 */
struct rseq_cs {
	/* Version of this structure. */
	__u32 version;
	/* enum rseq_cs_flags */
	__u32 flags;
	__u64 start_ip;
	/* Offset from start_ip. */
	__u64 post_commit_offset;
	__u64 abort_ip;
} __attribute__((aligned(4 * sizeof(__u64))));

/*
 * struct rseq is aligned on 4 * 8 bytes to ensure it is always
 * contained within a single cache-line.
 *
 * A single struct rseq per thread is allowed.
 */
struct rseq {
	/*
	 * Restartable sequences cpu_id_start field. Updated by the
	 * kernel. Read by user-space with single-copy atomicity
	 * semantics. This field should only be read by the thread which
	 * registered this data structure. Aligned on 32-bit. Always
	 * contains a value in the range of possible CPUs, although the
	 * value may not be the actual current CPU (e.g. if rseq is not
	 * initialized). This CPU number value should always be compared
	 * against the value of the cpu_id field before performing a rseq
	 * commit or returning a value read from a data structure indexed
	 * using the cpu_id_start value.
	 */
	__u32 cpu_id_start;
	/*
	 * Restartable sequences cpu_id field. Updated by the kernel.
	 * Read by user-space with single-copy atomicity semantics. This
	 * field should only be read by the thread which registered this
	 * data structure. Aligned on 32-bit. Values
	 * RSEQ_CPU_ID_UNINITIALIZED and RSEQ_CPU_ID_REGISTRATION_FAILED
	 * have a special semantic: the former means "rseq uninitialized",
	 * and latter means "rseq initialization failed". This value is
	 * meant to be read within rseq critical sections and compared
	 * with the cpu_id_start value previously read, before performing
	 * the commit instruction, or read and compared with the
	 * cpu_id_start value before returning a value loaded from a data
	 * structure indexed using the cpu_id_start value.
	 */
	__u32 cpu_id;
	/*
	 * Restartable sequences rseq_cs field.
	 *
	 * Contains NULL when no critical section is active for the current
	 * thread, or holds a pointer to the currently active struct rseq_cs.
	 *
	 * Updated by user-space, which sets the address of the currently
	 * active rseq_cs at the beginning of assembly instruction sequence
	 * block, and set to NULL by the kernel when it restarts an assembly
	 * instruction sequence block, as well as when the kernel detects that
	 * it is preempting or delivering a signal outside of the range
	 * targeted by the rseq_cs. Also needs to be set to NULL by user-space
	 * before reclaiming memory that contains the targeted struct rseq_cs.
	 *
	 * Read and set by the kernel. Set by user-space with single-copy
	 * atomicity semantics. This field should only be updated by the
	 * thread which registered this data structure. Aligned on 64-bit.
	 *
	 * 32-bit architectures should update the low order bits of the
	 * rseq_cs field, leaving the high order bits initialized to 0.
	 */
	__u64 rseq_cs;

	/*
	 * Restartable sequences flags field.
	 *
	 * This field should only be updated by the thread which
	 * registered this data structure. Read by the kernel.
	 * Mainly used for single-stepping through rseq critical sections
	 * with debuggers.
	 *
	 * - RSEQ_CS_FLAG_NO_RESTART_ON_PREEMPT
	 *     Inhibit instruction sequence block restart on preemption
	 *     for this thread.
	 * - RSEQ_CS_FLAG_NO_RESTART_ON_SIGNAL
	 *     Inhibit instruction sequence block restart on signal
	 *     delivery for this thread.
	 * - RSEQ_CS_FLAG_NO_RESTART_ON_MIGRATE
	 *     Inhibit instruction sequence block restart on migration for
	 *     this thread.
	 */
	__u32 flags;

	/*
	 * Restartable sequences node_id field. Updated by the kernel. Read by
	 * user-space with single-copy atomicity semantics. This field should
	 * only be read by the thread which registered this data structure.
	 * Aligned on 32-bit. Contains the current NUMA node ID.
	 */
	__u32 node_id;

	/*
	 * Restartable sequences mm_cid field. Updated by the kernel. Read by
	 * user-space with single-copy atomicity semantics. This field should
	 * only be read by the thread which registered this data structure.
	 * Aligned on 32-bit. Contains the current thread's concurrency ID
	 * (allocated uniquely within a memory map).
	 */
	__u32 mm_cid;

	/*
	 * cr_counter: user space sets any bit to indicate it is in a
	 * critical section. Bit 1 is reserved for the kernel to request
	 * that the task yield (RSEQ_CR_FLAG_KERNEL_REQUEST_SCHED).
	 * Any non-zero value in bits [31:1] means in critical section.
	 */
	__u32 cr_counter;

	/*
	 * wait_counter: user space increments (by 4, same encoding as
	 * cr_counter bits [31:2]) to indicate it is in a spin/wait region
	 * waiting to acquire a lock. Bits [1:0] are reserved.
	 * Non-zero bits [31:2] mean the thread is currently spinning/waiting.
	 * Distinct from cr_counter: cr_counter tracks holding; wait_counter
	 * tracks waiting.
	 */
	__u32 wait_counter;

	/*
	 * _cs_pad: explicit alignment padding so the u64 CS timing fields
	 * below begin on an 8-byte boundary (wait_counter ends at offset 36;
	 * next 8-byte boundary is offset 40).  Reserved for future use.
	 */
	__u32 _cs_pad;

	/*
	 * last_cs_overall_ns: written by user space at the moment a spinlock
	 * or rseq-tracked critical section is released.  Contains the wall-
	 * clock duration of the most recently completed CS in nanoseconds:
	 *
	 *   last_cs_overall_ns = unlock_ts(CLOCK_MONOTONIC)
	 *                      - lock_ts(CLOCK_MONOTONIC)
	 *
	 * "CS" means from successful lock acquisition to the moment of unlock.
	 * Written by the locking library (LD_PRELOAD or glibc pthread), not
	 * by the kernel.  Zero until the first CS completes.
	 */
	__u64 last_cs_overall_ns;

	/*
	 * last_cs_active_ns: written by user space at unlock alongside
	 * last_cs_overall_ns.  Contains the on-CPU (active) duration of the
	 * most recently completed CS in nanoseconds:
	 *
	 *   last_cs_active_ns = unlock_ts(CLOCK_THREAD_CPUTIME_ID)
	 *                     - lock_ts(CLOCK_THREAD_CPUTIME_ID)
	 *
	 * CLOCK_THREAD_CPUTIME_ID does not advance while the thread is off-CPU,
	 * so last_cs_overall_ns - last_cs_active_ns gives the off-CPU time
	 * accumulated while the lock was held.
	 * Written by the locking library, not by the kernel.
	 */
	__u64 last_cs_active_ns;

	/*
	 * last_wait_overall_ns: written by user space at unlock alongside the
	 * CS timing fields.  Contains the wall-clock duration from the moment
	 * the locking library first attempted to acquire the lock (before any
	 * spinning) to the moment the lock was successfully acquired:
	 *
	 *   last_wait_overall_ns = lock_acquired_ts(CLOCK_MONOTONIC)
	 *                        - lock_attempt_ts(CLOCK_MONOTONIC)
	 *
	 * When the lock is acquired without contention this value is the
	 * latency of the first CAS; under contention it includes all spinning
	 * time.  Together with last_cs_overall_ns it gives the full lock-cycle
	 * wall time: last_wait_overall_ns + last_cs_overall_ns.
	 *
	 * Note: last_wait_active_ns is intentionally omitted from the ABI.
	 * Spinning is CPU-bound so wait_active ≈ wait_overall in the common
	 * case, and adding a second wait u64 would push sizeof to 96 bytes,
	 * which increases the chance of spanning two cache lines under the
	 * current 32-byte alignment.  The locking library may track it
	 * internally.
	 *
	 * This field fills the 8 bytes of alignment padding that existed
	 * between last_cs_active_ns and the struct boundary, so sizeof remains
	 * 64 bytes.
	 */
	__u64 last_wait_overall_ns;

	/*
	 * Flexible array member at end of structure, after last feature field.
	 * offsetof(struct rseq, end) = 64.
	 * AT_RSEQ_FEATURE_SIZE will report 64 after a kernel rebuild.
	 */
	char end[];
} __attribute__((aligned(4 * sizeof(__u64))));

#endif /* _UAPI_LINUX_RSEQ_H */
