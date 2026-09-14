/*
 * ivh_adaptive_futex_lock.h -- a userspace adaptive lock: busy-spin, then
 * check a per-lock TSC heartbeat for holder staleness, then FUTEX_WAIT.
 * Designed 2026-09-11/12 as the userspace analogue of the kernel's PV
 * qspinlock tier-1/tier-2 mechanism (kernel/locking/qspinlock_paravirt.h),
 * one privilege level up: futex(2) is the syscall-level block/wake pair,
 * exactly as HLT+hypercall-kick is at the kernel level.
 *
 * Single-header, static-inline, no build system integration needed -- built
 * for reuse across NHextend-full.c and future userspace benchmarks, not
 * wired to any one caller's struct layout.
 *
 * ============================================================================
 * WHY THIS EXISTS, AND WHY IT ISN'T A NAIVE "SPIN THEN SLEEP" LOCK
 * ============================================================================
 *
 * 1. The heartbeat is keyed by LOCK, not by CPU. The kernel's own analogous
 *    mechanism (struct ivh_tsc_beat, arch/x86/include/asm/ivh_tsc_beat.h) is
 *    per-CPU, which is correct there because a vCPU IS the entity being
 *    checked. It does NOT transfer to unpinned userspace threads: if a
 *    waiter checked "CPU N's heartbeat" and the holder has since migrated
 *    off CPU N, the waiter is watching an empty chair -- and this fails
 *    WORST exactly when it matters most, because Linux's load balancer
 *    preferentially places a long-preempted (just-woken) thread on a
 *    DIFFERENT, less-loaded CPU than the one it left. Keying the heartbeat
 *    to the lock instance itself (whoever holds it, wherever they are)
 *    sidesteps this entirely -- migration-immune by construction, no
 *    kernel changes needed (raw RDTSC is ring-3 executable on this host).
 *
 * 2. The sleep trigger is heartbeat staleness, NOT a spin-count. With N
 *    threads contending a short CS, a waiter routinely queues behind many
 *    perfectly healthy holders -- entirely legitimate waiting that a fixed
 *    spin-count trigger would misread as a stall. The heartbeat does not:
 *    each successive healthy holder republishes it on acquisition, so the
 *    heartbeat stays fresh no matter how long the healthy queue is. It only
 *    goes stale when NOBODY is making progress, which is the one condition
 *    actually worth paying a syscall to fix.
 *
 * 3. Wake-skipping mirrors glibc NPTL's lowlevellock 3-state pattern
 *    (0=free, 1=held/no waiters, 2=held/may have waiters), with the
 *    critical rule spelled out at ivh_afl_lock()'s slow path and
 *    ivh_afl_unlock(): PESSIMISM LIVES ON THE SLEEP/ACQUIRE SIDE, NEVER ON
 *    THE WAKE SIDE. Unlock always writes 0 and wakes iff it swapped out a
 *    2. A thread that acquires via anything other than the direct 0->1 CAS
 *    installs 2 unconditionally, without trying to determine whether other
 *    sleepers remain -- it cannot know, so it assumes the worst. Getting
 *    this backwards (having the woken thread optimistically install 1)
 *    causes a genuine, unrecoverable lost-wakeup hang, not just a
 *    performance regression -- see the comment on ivh_afl_lock()'s slow
 *    path for the concrete failure walkthrough. The self-healing back to a
 *    wake-skipping state happens automatically: the moment contention
 *    genuinely subsides, some acquisition finds the lock free, takes the
 *    0->1 fast path, and every uncontended round-trip after that skips the
 *    syscall again -- no bookkeeping, no waiter count, no guessing.
 *
 * ============================================================================
 * KNOWN LIMITS -- read before reusing this for a different benchmark
 * ============================================================================
 *
 * - NOT recursive: locking twice from the same thread self-deadlocks (it
 *   will spin, watch its own heartbeat go stale, and sleep on itself).
 * - The lock's address is its futex identity: never memcpy/realloc/move a
 *   struct ivh_afl_lock once any thread may have waited on it.
 * - Fails CLOSED if the host's TSC isn't trustworthy (see
 *   ivh_afl_global_init()): degrades to pure busy-spin, never sleeps on a
 *   heartbeat it can't trust. IVH_AFL_DISABLE=1 forces this path
 *   deliberately, for a same-binary A/B "pure spin" comparison arm.
 * - No packed multi-field lock word, ever -- see the comment above
 *   struct ivh_afl_lock. This project's own NHextend3.c has a `cmpxchg()`
 *   macro that LOOKS like a full 64-bit CAS but is actually byte-sized
 *   (verified live: cmpxchg(&word, 0, 0x1234567800000005UL) writes only
 *   0x05). It "works" there only because the values exchanged (0,
 *   sched_getcpu()+1 in [1,16]) all fit in a byte. This header does not use
 *   that macro anywhere -- only correctly-sized __atomic builtins -- and no
 *   future edit here should pack a second field into `state` on the theory
 *   that "it's just 0/1/2, it'll fit in a byte too": that is exactly the
 *   trap, see struct ivh_afl_lock's comment.
 */
#ifndef IVH_ADAPTIVE_FUTEX_LOCK_H
#define IVH_ADAPTIVE_FUTEX_LOCK_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <immintrin.h>

enum { IVH_AFL_OK = 0, IVH_AFL_ABORTED = -1 };

/* ---- tunables, all #define-overridable at build time (-D) ---- */

/* Tier-1 gate: cpu_relax() iterations between two heartbeat checks. Cheap
 * check first, expensive (RDTSC + cross-line load) check less often -- same
 * split as the kernel's PV_PREV_CHECK_MASK. Target ~2-8us between checks;
 * see calibrate_cpu_relax() below for why this is measured, not guessed. */
#ifndef IVH_AFL_SPINS_BEFORE_CHECK
#define IVH_AFL_SPINS_BEFORE_CHECK 256
#endif

/* Staleness threshold in nanoseconds. Floor: must clear one heartbeat
 * republish interval plus one handoff gap for a HEALTHY holder (~15us at
 * this project's validated loop_spin=5000, ~13us CS). Ceiling: must stay
 * below the shortest REAL stall this project has ever measured (NHextend3's
 * own >100us host-preemption filter; kernel rq->preemptions uses >1ms).
 * 50000 sits with ~3x margin on the floor and ~2x margin on the ceiling.
 * NOT derived from expected wait time -- see ivh_afl_lock()'s comment on
 * why healthy queueing must never trip this. */
#ifndef IVH_AFL_STALE_NS
#define IVH_AFL_STALE_NS 50000
#endif

/* Heartbeat republish gate: republish every (mask+1) CS iterations. Must be
 * << the staleness threshold (target >=10x margin) and >> zero (don't
 * perturb the CS with a heartbeat write every single iteration). Default
 * assumes ~2.6ns/iteration (loop_spin=5000 =~ 13us) -- 1024 iterations =~
 * 2.7us, ~18x margin against the 50us default threshold. If loop_spin is
 * swept, this should be swept with it to keep the interval near constant. */
#ifndef IVH_AFL_BEAT_MASK
#define IVH_AFL_BEAT_MASK 0x3FFu
#endif

/* The same gate expressed as a count, for callers that drive the interval
 * from their own loop counter instead of calling ivh_afl_beat() -- the
 * preferred pattern in any hot CS loop, see ivh_afl_beat()'s comment. */
#define IVH_AFL_BEAT_INTERVAL ((int)(IVH_AFL_BEAT_MASK + 1u))

/* FUTEX_WAKE count on unlock. 1 is the validated starting default per this
 * project's "measure before adding complexity" discipline -- waking more
 * than one is a real, known latency hedge (against the woken thread's own
 * scheduling delay) but its payoff has NOT been measured on this host, so
 * it is left as an easy runtime override (IVH_AFL_WAKE env var) rather than
 * built as adaptive logic. */
#ifndef IVH_AFL_WAKE_COUNT
#define IVH_AFL_WAKE_COUNT 1
#endif

/* Every FUTEX_WAIT carries this timeout. This is NOT a tuning knob for
 * responsiveness -- it is a correctness backstop. See ivh_afl_shutdown_wake()
 * for why a broadcast wake alone is not sufficient to guarantee shutdown
 * termination, only a bound on how long a race can persist. */
#ifndef IVH_AFL_WAIT_TIMEOUT_NS
#define IVH_AFL_WAIT_TIMEOUT_NS 10000000L /* 10ms */
#endif

/*
 * The lock itself. THREE separate cachelines, not cosmetic padding:
 *   - `state` is polled by every spinning waiter; `hb_tsc` is written by
 *     the holder every IVH_AFL_BEAT_MASK+1 iterations. On one line, every
 *     republish would invalidate the line under every spinner, injecting a
 *     heartbeat-rate-dependent slowdown into the exact benchmark this
 *     exists to measure.
 *   - config is written once at init and read-mostly after.
 *
 * `state` holds EXACTLY 0/1/2 (see file header, "wake-skipping"). The
 * holder's identity is deliberately NOT encoded here (unlike NHextend3's
 * original `lock = sched_getcpu()+1`) -- with a lock-keyed heartbeat, no
 * waiter ever needs to resolve which CPU or thread the holder is, so there
 * is nothing to pack, and nothing should ever be packed into `state`: it
 * must stay a plain 0/1/2 word operated on only by __atomic builtins sized
 * to its actual (uint32_t) type. See the file header for why "just add one
 * more bit, it still fits in a byte" is exactly the mistake to not make.
 */
struct ivh_afl_lock {
	_Alignas(64) volatile uint32_t state;
	uint32_t _pad0[15];

	_Alignas(64) volatile uint64_t hb_tsc;
	uint64_t _pad1[7];

	_Alignas(64) const volatile bool *abort_flag;
	void (*before_sleep)(void *);
	void (*after_wake)(void *);
	void *hook_arg;
	uint64_t stale_tsc;
	uint32_t wake_count;
	uint32_t enabled; /* 0 => TSC untrusted or IVH_AFL_DISABLE=1: pure spin, never sleep */
#ifdef IVH_AFL_DEBUG
	volatile int owner_tid;
#endif
};

#ifdef IVH_AFL_STATS
struct ivh_afl_stats {
	uint64_t fast_acquires;   /* 0->1 direct CAS: the wake-skipping path */
	uint64_t slow_acquires;   /* acquired via the 0->2 exchange path */
	uint64_t sleeps;          /* FUTEX_WAIT calls actually entered */
	uint64_t wakes_issued;    /* FUTEX_WAKE calls (unlock swapped out a 2) */
	uint64_t wakes_skipped;   /* unlocks that swapped out a 1: syscall skipped */
	uint64_t eagain, eintr, timeouts;
	uint64_t stale_detections;
	uint64_t stale_recheck_aborts; /* the re-probe at the "about to sleep"
	                                 * point found the lock free or a fresh
	                                 * holder -- a pointless sleep avoided */
	uint64_t wakes_woke_nobody;    /* FUTEX_WAKE's own return value was 0 --
	                                 * state==2 said "may have waiters" but
	                                 * none were actually asleep yet. This is
	                                 * the DIRECT measurement of wasted wakes
	                                 * (previously only inferred indirectly
	                                 * by comparing wakes_issued to sleeps). */
	uint64_t wakes_woke_someone;   /* return value > 0: a real wake */
	uint64_t total_threads_woken;  /* sum of all positive return values --
	                                 * can exceed wakes_woke_someone if
	                                 * wake_count > 1 ever pops more than 1 */
};
static __thread struct ivh_afl_stats ivh_afl_stats;
#define IVH_AFL_STAT_INC(field) (ivh_afl_stats.field++)
#else
#define IVH_AFL_STAT_INC(field) do { } while (0)
#endif

/* ---- process-global config, set once by ivh_afl_global_init() ---- */
static uint64_t ivh_afl_tsc_per_ns_x1000 = 3000; /* overwritten by calibration */
static uint64_t ivh_afl_g_stale_tsc;
static uint32_t ivh_afl_g_wake_count = IVH_AFL_WAKE_COUNT;
static uint32_t ivh_afl_g_enabled = 1;
static unsigned ivh_afl_g_spins_before_check = IVH_AFL_SPINS_BEFORE_CHECK;

static inline long ivh_afl_futex(volatile uint32_t *uaddr, int op, uint32_t val,
				  const struct timespec *timeout)
{
	return syscall(SYS_futex, uaddr, op, val, timeout, NULL, 0);
}

/*
 * calibrate_tsc() -- resurrected verbatim (in spirit) from an earlier
 * version of NHextend3.c, git commit 298be1454 in the docs repo
 * (/root/linux-6.17), which had this exact pattern under the same name
 * before it was removed by a later refactor. One-time TSC-per-ns
 * calibration against CLOCK_MONOTONIC.
 */
static void ivh_afl_calibrate_tsc(void)
{
	struct timespec ts0, ts1;
	uint64_t tsc0, tsc1, ns;

	clock_gettime(CLOCK_MONOTONIC, &ts0);
	tsc0 = __rdtsc();
	struct timespec sleep_req = { .tv_sec = 0, .tv_nsec = 20 * 1000 * 1000 };
	nanosleep(&sleep_req, NULL);
	tsc1 = __rdtsc();
	clock_gettime(CLOCK_MONOTONIC, &ts1);

	ns = (uint64_t)(ts1.tv_sec - ts0.tv_sec) * 1000000000ULL +
	     (ts1.tv_nsec - ts0.tv_nsec);
	if (ns == 0)
		return; /* keep the conservative default */

	ivh_afl_tsc_per_ns_x1000 = ((tsc1 - tsc0) * 1000ULL) / ns;
}

/*
 * TSC suitability check -- fail CLOSED (pure spin, never sleep on a
 * heartbeat we can't trust) rather than open. constant_tsc + nonstop_tsc +
 * tsc_reliable together mean one guest-wide timebase that neither stops in
 * idle nor varies with frequency -- the precondition for comparing two
 * different threads' TSC readings at all. Confirmed present on this host
 * (TDX guest) via /proc/cpuinfo at design time; still checked live here so
 * the header degrades safely on any future host where it's reused.
 */
static bool ivh_afl_tsc_trustworthy(void)
{
	FILE *f = fopen("/proc/cpuinfo", "r");
	char line[4096];
	bool has_constant = false, has_nonstop = false, has_reliable = false;

	if (!f)
		return false;
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, "flags", 5) != 0)
			continue;
		if (strstr(line, "constant_tsc")) has_constant = true;
		if (strstr(line, "nonstop_tsc")) has_nonstop = true;
		if (strstr(line, "tsc_reliable")) has_reliable = true;
		break; /* first CPU's flags line is representative on this host */
	}
	fclose(f);
	return has_constant && has_nonstop && has_reliable;
}

/*
 * Once per process, before creating any threads that will touch a lock.
 * Not thread-safe; do not call concurrently.
 */
static void ivh_afl_global_init(void)
{
	const char *wake_env = getenv("IVH_AFL_WAKE");
	const char *disable_env = getenv("IVH_AFL_DISABLE");
	const char *stale_env = getenv("IVH_AFL_STALE_NS");
	const char *spins_env = getenv("IVH_AFL_SPINS");
	unsigned long stale_ns = IVH_AFL_STALE_NS;

	if (wake_env && atoi(wake_env) > 0)
		ivh_afl_g_wake_count = (uint32_t)atoi(wake_env);
	if (stale_env && atol(stale_env) > 0)
		stale_ns = strtoul(stale_env, NULL, 10);
	if (spins_env && atoi(spins_env) > 0)
		ivh_afl_g_spins_before_check = (unsigned)atoi(spins_env);

	ivh_afl_calibrate_tsc();
	ivh_afl_g_stale_tsc = (stale_ns * ivh_afl_tsc_per_ns_x1000) / 1000ULL;

	ivh_afl_g_enabled = 1;
	if (disable_env && atoi(disable_env) != 0)
		ivh_afl_g_enabled = 0;
	if (!ivh_afl_tsc_trustworthy()) {
		fprintf(stderr,
			"ivh_afl: TSC not confirmed reliable (missing constant_tsc/"
			"nonstop_tsc/tsc_reliable) -- disabling adaptive sleep, "
			"falling back to pure spin.\n");
		ivh_afl_g_enabled = 0;
	}
}

static inline void ivh_afl_init(struct ivh_afl_lock *l)
{
	memset((void *)l, 0, sizeof(*l));
	l->state = 0;
	l->hb_tsc = __rdtsc();
	l->stale_tsc = ivh_afl_g_stale_tsc;
	l->wake_count = ivh_afl_g_wake_count;
	l->enabled = ivh_afl_g_enabled;
}

static inline void ivh_afl_set_abort_flag(struct ivh_afl_lock *l,
					   const volatile bool *flag)
{
	l->abort_flag = flag;
}

static inline void ivh_afl_set_hooks(struct ivh_afl_lock *l,
				      void (*before_sleep)(void *),
				      void (*after_wake)(void *),
				      void *arg)
{
	l->before_sleep = before_sleep;
	l->after_wake = after_wake;
	l->hook_arg = arg;
}

static inline void ivh_afl_publish_heartbeat(struct ivh_afl_lock *l)
{
	__atomic_store_n(&l->hb_tsc, __rdtsc(), __ATOMIC_RELAXED);
}

/*
 * Call from inside the critical section. Self-gated by a __thread counter
 * and IVH_AFL_BEAT_MASK.
 *
 * NOT cheap enough to call every iteration of a hot loop -- the original
 * comment here claimed it was, and that claim was measured wrong on
 * 2026-09-13. The gate counter is a __thread variable, so even on the
 * (1023/1024) calls that publish nothing this compiles to a %fs-relative
 * load AND store per call; in a CS loop whose body is a store fence (as in
 * NHextend-full.c) that store has to drain before the next fence, which
 * cost 7-20% of throughput at low thread counts, where the adaptive
 * mechanism has no stall to catch and so returns nothing for it.
 *
 * ANY CALLER WITH A LOOP COUNTER OF ITS OWN SHOULD GATE ON THAT INSTEAD:
 *
 *     int next_beat = IVH_AFL_BEAT_INTERVAL;
 *     for (int i = 0; i < n; i++) {
 *             ...body...
 *             if (i == next_beat) {
 *                     ivh_afl_publish_heartbeat(l);
 *                     next_beat += IVH_AFL_BEAT_INTERVAL;
 *             }
 *     }
 *
 * A local `next_beat` stays in a register across the body's memory
 * clobbers, so the whole gate costs a register compare and a predicted
 * not-taken branch, with no memory traffic at all. Use ivh_afl_beat() only
 * where there is no such counter (straight-line CS code, a few natural
 * call points) -- that is what it is for.
 *
 * The per-thread gate counter is not reset across different locks, so with
 * multiple locks held in sequence the gating is approximate -- benign, the
 * worst case is one beat landing slightly early or late for a given lock.
 */
static inline void ivh_afl_beat(struct ivh_afl_lock *l)
{
	static __thread unsigned counter;

	if ((counter++ & IVH_AFL_BEAT_MASK) == 0)
		ivh_afl_publish_heartbeat(l);
}

static inline void ivh_afl_cpu_relax(void)
{
	asm volatile("pause" ::: "memory");
}

/*
 * Acquire. Returns IVH_AFL_OK, or IVH_AFL_ABORTED if *abort_flag became
 * true while waiting (only meaningful if ivh_afl_set_abort_flag() was
 * called -- see the shutdown-deadlock note on ivh_afl_shutdown_wake()).
 */
static inline int ivh_afl_lock(struct ivh_afl_lock *l)
{
	uint32_t expected;
	unsigned spins = 0;
	struct timespec timeout;
	/* Earliest TSC at which re-reading l->hb_tsc could possibly change
	 * this waiter's stale/not-stale verdict; see "Tier-2a, DEADLINE SKIP"
	 * below. INT64_MIN = "nothing observed yet, read on the first check". */
	int64_t next_hb_read_tsc = INT64_MIN;

	/* --- fast path: uncontended. STRONG cas -- a spurious failure here
	 * would push an uncontended acquire into the slow path and install a
	 * 2, poisoning the wake-skipping state for no reason. This is the
	 * ONLY path that installs 1, and installing 1 here (rather than always
	 * taking the slow path's exchange-to-2) is not an optimization, it is
	 * the mechanism by which the lock ever RETURNS to a wake-skipping
	 * state after contention -- see ivh_afl_unlock()'s comment. */
	expected = 0;
	if (__atomic_compare_exchange_n(&l->state, &expected, 1, false,
					 __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
		IVH_AFL_STAT_INC(fast_acquires);
		ivh_afl_publish_heartbeat(l);
		return IVH_AFL_OK;
	}

	/* --- slow path ---
	 *
	 * PESSIMISM RULE, applies to every acquisition below this point:
	 * whichever thread wins here installs state=2 UNCONDITIONALLY, never
	 * 1, regardless of whether it can see other waiters. It cannot know
	 * whether other sleepers remain (state carries no waiter count), so
	 * it must assume the worst. This is what closes the lost-wakeup hang:
	 * if the winner here instead optimistically installed 1 (reasoning
	 * "I got woken/won the race, the queue must be empty now"), a
	 * concrete failure follows --
	 *
	 *   T1 holds. T2, T3 both spin, both fail the fast path, both install
	 *   state=2 via the exchange below, both enter FUTEX_WAIT(&state, 2).
	 *   T1 unlocks: swaps out 2 -> wakes exactly one via FUTEX_WAKE(1).
	 *   Kernel pops T2. state=0. T3 is STILL ASLEEP; nothing records that.
	 *   T2 wakes, sees state==0, takes the (WRONG) optimistic 0->1 CAS.
	 *   T2 runs its CS, unlocks: swaps out 1 -> wake SKIPPED (state==1
	 *   looked like "no waiters"). state=0.
	 *   No thread now holds the lock and no thread will ever call
	 *   FUTEX_WAKE again on a quiescent workload. T3 is not slow, it is
	 *   PERMANENTLY LOST -- blocked in the kernel on &state==2, and
	 *   nothing will ever wake it.
	 *
	 * The correct rule instead has the woken/winning thread install 2
	 * again unconditionally. Worst case: it was wrong (no one else was
	 * actually asleep), and its own eventual unlock issues one
	 * FUTEX_WAKE that wakes nobody -- one bounded, self-healing wasted
	 * syscall, not an unrecoverable hang. The two error directions are
	 * NOT symmetric (bounded waste vs. permanent loss), which is why
	 * every judgment call in this state machine resolves toward 2.
	 *
	 * Self-healing back to state 1 (and wake-skipping) happens without
	 * any of this bookkeeping: it occurs automatically the next time some
	 * acquisition arrives and finds the lock genuinely free, and takes
	 * the FAST path above instead of this one.
	 */
	if (__atomic_exchange_n(&l->state, 2, __ATOMIC_ACQUIRE) == 0) {
		IVH_AFL_STAT_INC(slow_acquires);
		ivh_afl_publish_heartbeat(l);
		return IVH_AFL_OK;
	}

	timeout.tv_sec = IVH_AFL_WAIT_TIMEOUT_NS / 1000000000L;
	timeout.tv_nsec = IVH_AFL_WAIT_TIMEOUT_NS % 1000000000L;

	for (;;) {
		if (l->abort_flag && *l->abort_flag)
			return IVH_AFL_ABORTED;

		if (__atomic_load_n(&l->state, __ATOMIC_RELAXED) == 0) {
			if (__atomic_exchange_n(&l->state, 2, __ATOMIC_ACQUIRE) == 0) {
				IVH_AFL_STAT_INC(slow_acquires);
				ivh_afl_publish_heartbeat(l);
				return IVH_AFL_OK;
			}
			continue; /* someone beat us; state is already 2 again */
		}

		if (!l->enabled) {
			ivh_afl_cpu_relax();
			continue; /* pure-spin fallback: never sleep */
		}

		if (++spins < ivh_afl_g_spins_before_check) {
			ivh_afl_cpu_relax();
			continue;
		}
		spins = 0;

		/*
		 * Tier-2: the gated, more expensive staleness check. Signed
		 * subtraction is deliberate -- if hb is momentarily ahead of
		 * now (bounded skew from RDTSC's lack of ordering, or a small
		 * residual cross-vCPU TSC offset), an UNSIGNED subtraction
		 * underflows to ~2^64 ("infinitely stale") and causes an
		 * immediate spurious sleep. Signed makes that case compare
		 * as fresher-than-threshold instead, which is the safe
		 * direction. This is the single most likely one-character
		 * bug in this file -- if staleness ever fires constantly on
		 * an idle host, check this cast first.
		 *
		 * Tier-2a, added 2026-09-13 -- DEADLINE SKIP. RDTSC is local
		 * and free of coherence traffic; the load of l->hb_tsc is not.
		 * It pulls the holder's heartbeat line into this waiter's
		 * cache in Shared state, so the holder's next republish has
		 * to take it back exclusive. With several waiters each
		 * probing every IVH_AFL_SPINS_BEFORE_CHECK spins, that line
		 * ping-pongs continuously and the cost lands ON THE HOLDER,
		 * i.e. directly on the serialized critical path -- measured
		 * at ~4-5% of throughput at 4 threads on this host.
		 *
		 * The skip removes most of those loads for free, using a
		 * fact the previous code had already computed and thrown
		 * away: having just seen heartbeat value `hb`, this waiter
		 * CANNOT legitimately conclude staleness before hb +
		 * stale_tsc, no matter what happens in between. Heartbeats
		 * only ever move forward (a republish by this holder, or an
		 * acquisition by the next one), so a later read can only push
		 * that deadline further out, never pull it in. Re-reading
		 * hb_tsc before the deadline therefore cannot change this
		 * waiter's decision -- it can only generate coherence
		 * traffic. So: remember the deadline, and until it passes,
		 * spin on RDTSC alone and never touch the line.
		 *
		 * This changes no threshold and no state-machine rule. Worst
		 * case detection latency is unchanged (a holder that stalls
		 * immediately after its last beat is still detected one
		 * stale_tsc later, which is the definition of the threshold);
		 * the only thing given up is the ability to notice a stall
		 * EARLIER than the threshold allows, which was never a
		 * legitimate conclusion to draw in the first place.
		 */
		{
			int64_t now = (int64_t)__rdtsc();
			int64_t hb;

			if (now < next_hb_read_tsc) {
				ivh_afl_cpu_relax();
				continue;
			}

			hb = (int64_t)__atomic_load_n(&l->hb_tsc, __ATOMIC_RELAXED);

			if (now - hb < (int64_t)l->stale_tsc) {
				next_hb_read_tsc = hb + (int64_t)l->stale_tsc;
				ivh_afl_cpu_relax();
				continue;
			}
			IVH_AFL_STAT_INC(stale_detections);

			/* Re-probe: the holder may have released between the
			 * heartbeat read above and now. */
			if (__atomic_exchange_n(&l->state, 2, __ATOMIC_ACQUIRE) == 0) {
				IVH_AFL_STAT_INC(slow_acquires);
				ivh_afl_publish_heartbeat(l);
				return IVH_AFL_OK;
			}
			/* Double-check: a NEW, healthy holder may have taken
			 * it between the first read and the re-probe just
			 * above -- without this we could commit to a
			 * FUTEX_WAIT against a perfectly healthy holder. */
			now = (int64_t)__rdtsc();
			hb = (int64_t)__atomic_load_n(&l->hb_tsc, __ATOMIC_RELAXED);
			if (now - hb < (int64_t)l->stale_tsc) {
				IVH_AFL_STAT_INC(stale_recheck_aborts);
				next_hb_read_tsc = hb + (int64_t)l->stale_tsc;
				ivh_afl_cpu_relax();
				continue;
			}
		}

		if (l->before_sleep)
			l->before_sleep(l->hook_arg);

		IVH_AFL_STAT_INC(sleeps);
		{
			long r = ivh_afl_futex(&l->state, FUTEX_WAIT_PRIVATE, 2, &timeout);
			if (r < 0) {
				if (errno == EAGAIN) IVH_AFL_STAT_INC(eagain);
				else if (errno == EINTR) IVH_AFL_STAT_INC(eintr);
				else if (errno == ETIMEDOUT) IVH_AFL_STAT_INC(timeouts);
			}
			/* Every return (woken / EAGAIN / EINTR / ETIMEDOUT) means
			 * the same thing: loop and re-probe. None of them means
			 * "you now hold the lock." */
		}

		/* Arbitrary time has passed inside the kernel, and the lock
		 * has very likely changed hands: the deadline-skip window
		 * computed before the sleep is meaningless now. Re-read the
		 * heartbeat on the first check after waking. */
		next_hb_read_tsc = INT64_MIN;

		if (l->after_wake)
			l->after_wake(l->hook_arg);
	}
}

/*
 * Release. Always writes 0 (never 2 -- see file header on why "sticky"
 * unlock would defeat wake-skipping forever) and wakes iff the state we
 * just swapped out was 2.
 */
static inline void ivh_afl_unlock(struct ivh_afl_lock *l)
{
	uint32_t prev = __atomic_exchange_n(&l->state, 0, __ATOMIC_RELEASE);

#ifdef IVH_AFL_DEBUG
	if (prev == 0) {
		fprintf(stderr, "ivh_afl: unlock of an unheld lock (caller bug)\n");
		abort();
	}
#endif
	if (prev == 2) {
		IVH_AFL_STAT_INC(wakes_issued);
#ifdef IVH_AFL_STATS
		{
			long woken = ivh_afl_futex(&l->state, FUTEX_WAKE_PRIVATE,
						   l->wake_count, NULL);
			if (woken > 0) {
				IVH_AFL_STAT_INC(wakes_woke_someone);
				ivh_afl_stats.total_threads_woken += (uint64_t)woken;
			} else {
				IVH_AFL_STAT_INC(wakes_woke_nobody);
			}
		}
#else
		ivh_afl_futex(&l->state, FUTEX_WAKE_PRIVATE, l->wake_count, NULL);
#endif
	} else {
		IVH_AFL_STAT_INC(wakes_skipped);
	}
}

/*
 * Shutdown helper: call after setting the caller's own done/abort flag
 * (which ivh_afl_set_abort_flag() pointed this lock at), BEFORE joining any
 * threads that might be asleep in ivh_afl_lock(). Without this, a thread
 * blocked in FUTEX_WAIT does not poll anything and pthread_join() hangs.
 *
 * This alone is NOT sufficient by itself -- a thread can be between its
 * last abort-flag check and its FUTEX_WAIT syscall entry when this runs,
 * and would then sleep AFTER the broadcast. That is exactly what
 * IVH_AFL_WAIT_TIMEOUT_NS exists to bound: such a thread wakes on its own
 * within that timeout regardless, re-checks the abort flag, and returns
 * IVH_AFL_ABORTED. Both pieces are required together.
 */
static inline void ivh_afl_shutdown_wake(struct ivh_afl_lock *l)
{
	ivh_afl_futex(&l->state, FUTEX_WAKE_PRIVATE, (uint32_t)INT32_MAX, NULL);
}

#endif /* IVH_ADAPTIVE_FUTEX_LOCK_H */
