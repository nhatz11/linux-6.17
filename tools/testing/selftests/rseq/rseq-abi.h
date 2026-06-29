/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
#ifndef _RSEQ_ABI_H
#define _RSEQ_ABI_H

/*
 * rseq-abi.h
 *
 * Restartable sequences system call API
 *
 * Copyright (c) 2015-2022 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 */

#include <linux/types.h>
#include <asm/byteorder.h>

enum rseq_abi_cpu_id_state {
	RSEQ_ABI_CPU_ID_UNINITIALIZED			= -1,
	RSEQ_ABI_CPU_ID_REGISTRATION_FAILED		= -2,
};

enum rseq_abi_flags {
	RSEQ_ABI_FLAG_UNREGISTER = (1 << 0),
};

enum rseq_abi_cs_flags_bit {
	RSEQ_ABI_CS_FLAG_NO_RESTART_ON_PREEMPT_BIT	= 0,
	RSEQ_ABI_CS_FLAG_NO_RESTART_ON_SIGNAL_BIT	= 1,
	RSEQ_ABI_CS_FLAG_NO_RESTART_ON_MIGRATE_BIT	= 2,
};

enum rseq_abi_cs_flags {
	RSEQ_ABI_CS_FLAG_NO_RESTART_ON_PREEMPT	=
		(1U << RSEQ_ABI_CS_FLAG_NO_RESTART_ON_PREEMPT_BIT),
	RSEQ_ABI_CS_FLAG_NO_RESTART_ON_SIGNAL	=
		(1U << RSEQ_ABI_CS_FLAG_NO_RESTART_ON_SIGNAL_BIT),
	RSEQ_ABI_CS_FLAG_NO_RESTART_ON_MIGRATE	=
		(1U << RSEQ_ABI_CS_FLAG_NO_RESTART_ON_MIGRATE_BIT),
};

enum rseq_abi_sched_state_flags {
	/*
	 * Task is currently running on a CPU if bit is set.
	 */
	RSEQ_ABI_SCHED_STATE_FLAG_ON_CPU	= (1U << 0),
};

/*
 * struct rseq_abi_sched_state - per-thread scheduler visibility state.
 *
 * Allocated by userspace (typically as TLS), its address is registered
 * with the kernel via the sched_state_ptr field of struct rseq_abi.
 * Cache-line alignment is recommended to avoid false sharing.
 */
struct rseq_abi_sched_state {
	/*
	 * Version of this structure.  Populated (set to 0) by the kernel
	 * at rseq registration time.
	 */
	__u32 version;
	/*
	 * Scheduler state bitmask (enum rseq_abi_sched_state_flags).
	 * Updated by the kernel.  May be read by any userspace thread
	 * with single-copy atomicity semantics.
	 */
	__u32 state;
	/*
	 * TID of the owning thread.  Initialized by userspace before
	 * registration; not modified by the kernel.
	 */
	__u32 tid;
};

/*
 * struct rseq_abi_cs is aligned on 4 * 8 bytes to ensure it is always
 * contained within a single cache-line. It is usually declared as
 * link-time constant data.
 */
struct rseq_abi_cs {
	/* Version of this structure. */
	__u32 version;
	/* enum rseq_abi_cs_flags */
	__u32 flags;
	__u64 start_ip;
	/* Offset from start_ip. */
	__u64 post_commit_offset;
	__u64 abort_ip;
} __attribute__((aligned(4 * sizeof(__u64))));

/*
 * struct rseq_abi is aligned on 4 * 8 bytes to ensure it is always
 * contained within a single cache-line.
 *
 * A single struct rseq_abi per thread is allowed.
 */
struct rseq_abi {
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
	 */
	union {
		__u64 ptr64;

		/*
		 * The "arch" field provides architecture accessor for
		 * the ptr field based on architecture pointer size and
		 * endianness.
		 */
		struct {
#ifdef __LP64__
			__u64 ptr;
#elif defined(__BYTE_ORDER) ? (__BYTE_ORDER == __BIG_ENDIAN) : defined(__BIG_ENDIAN)
			__u32 padding;		/* Initialized to zero. */
			__u32 ptr;
#else
			__u32 ptr;
			__u32 padding;		/* Initialized to zero. */
#endif
		} arch;
	} rseq_cs;

	/*
	 * Restartable sequences flags field.
	 *
	 * This field should only be updated by the thread which
	 * registered this data structure. Read by the kernel.
	 * Mainly used for single-stepping through rseq critical sections
	 * with debuggers.
	 *
	 * - RSEQ_ABI_CS_FLAG_NO_RESTART_ON_PREEMPT
	 *     Inhibit instruction sequence block restart on preemption
	 *     for this thread.
	 * - RSEQ_ABI_CS_FLAG_NO_RESTART_ON_SIGNAL
	 *     Inhibit instruction sequence block restart on signal
	 *     delivery for this thread.
	 * - RSEQ_ABI_CS_FLAG_NO_RESTART_ON_MIGRATE
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
	 * The following fields are local extensions present in this tree
	 * (linux-6.17 vSched/IVH port) and are not part of upstream Linux.
	 * They occupy offsets 28–63 to match the kernel's struct rseq layout.
	 */

	/*
	 * cr_counter: userspace sets bits [31:2] to encode spinlock nesting
	 * depth.  Bit 0 is the bare rseq-entry marker.  Bit 1 is set by the
	 * kernel to request a cooperative yield.  Offsets 28–31.
	 */
	__u32 cr_counter;

	/*
	 * wait_counter: userspace increments (by 4) while spinning on a lock.
	 * Non-zero bits [31:2] mean the thread is waiting to acquire a lock.
	 * Offsets 32–35.
	 */
	__u32 wait_counter;

	/*
	 * _cs_pad: alignment padding so the u64 timing fields below start at
	 * an 8-byte boundary (offset 40).  Offsets 36–39.
	 */
	__u32 _cs_pad;

	/*
	 * last_cs_overall_ns: wall-clock duration (ns) of the most recently
	 * completed critical section.  Written by the locking library at
	 * unlock.  Offsets 40–47.
	 */
	__u64 last_cs_overall_ns;

	/*
	 * last_cs_active_ns: on-CPU duration (ns) of the most recently
	 * completed critical section (CLOCK_THREAD_CPUTIME_ID).  Offsets 48–55.
	 */
	__u64 last_cs_active_ns;

	/*
	 * last_wait_overall_ns: wall-clock wait duration (ns) from first lock
	 * attempt to successful acquisition.  Offsets 56–63.
	 */
	__u64 last_wait_overall_ns;

	/*
	 * sched_state_ptr: pointer to a userspace-allocated
	 * struct rseq_abi_sched_state.  Initialized by userspace before
	 * registration; set to 0 to opt out of the feature.  Read by the
	 * kernel at rseq registration (offset 64, matching the kernel's
	 * struct rseq layout for this tree).
	 */
	__u64 sched_state_ptr;

	/*
	 * Flexible array member at end of structure, after last feature field.
	 * offsetof(struct rseq_abi, end) = 72.
	 */
	char end[];
} __attribute__((aligned(4 * sizeof(__u64))));

#endif /* _RSEQ_ABI_H */
