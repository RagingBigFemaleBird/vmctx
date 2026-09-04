/* SPDX-License-Identifier: GPL-2.0 */
/*
 * own.h -- the shared-memory ownership channel (REDESIGN.md, session 34).
 *
 * The owner's correction after sessions 30-33: the ownership record must be
 * ACTUAL SHARED MEMORY -- a segment every local participant maps and consults
 * at a memory read -- and a transition must BLOCK ON A FUTEX in that segment
 * rather than poll, yield, or guess. This header is that segment and the API
 * over it, compiled into BOTH halves exactly as pgstate.h is.
 *
 * WHAT IT IS, precisely:
 *   - ONE segment per machine (a memfd, MAP_SHARED). Created before any guest
 *     context is cloned, so every clone()d child inherits the mapping; threads
 *     of one process share it for free. This is the "shared memory channel
 *     among vmhome instances / among vmremote instances": the same structure
 *     whether one process or several map it.
 *   - The SOURCE's segment is the AUTHORITY -- the home of every page. The
 *     DESTINATION's segment is a coherent cache of the home record plus the
 *     destination's own kernel facts. Neither machine maps the other's segment
 *     (production is two kernels; a loopback-only shared segment would be the
 *     forbidden optional-correctness arm). The wire carries only transitions.
 *
 * THE ENTRY (one per (address space, page)):
 *   site    in {NONE, HOME, REMOTE}         where the token is (HOME = source)
 *   transit in {NONE, TO_HOME, TO_REMOTE}   at most ONE move in flight
 *   gen     the futex word: bumped on every landing, woken with FUTEX_WAKE
 *   transit_owner, transit_us               diagnosis only, NEVER a decider
 *
 * THE DISCIPLINE, and it is the whole point:
 *   own_begin(as, page, TO_*, &g) == OWN_BEGIN_CLAIMED   claimed the transition;
 *                                     do the one slow step (GET / take /
 *                                     install), then own_land(as, page,
 *                                     new_site), which wakes every waiter.
 *   own_begin(...) == OWN_BEGIN_BUSY  a transition is already in flight; do NOT
 *                                     start a second. `g` is the gen to wait on
 *                                     (read UNDER the lock, so no wakeup can be
 *                                     lost): own_wait(as, page, g, bound) sleeps
 *                                     until it lands, then re-decide from the
 *                                     fresh entry.  (g may legitimately be 0 --
 *                                     the entry has never landed yet.)
 * There is no clock and no tie-break: a waiter waits for the LANDING, which is
 * a real event on a real word, so "in transit or lost?" -- the question that
 * defeated the message directory -- is answered by the entry for every waiter
 * at once.
 *
 * This file is ADDITIVE. Until a call site is migrated onto it, it changes
 * nothing: own_init() maps a segment nothing yet reads. The migration
 * (REDESIGN.md build order) wires the receiving side on first, WITHOUT the
 * source fetch gate.
 */
#ifndef VMCTX_OWN_H
#define VMCTX_OWN_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/syscall.h>

/*
 * FUTEX_WAIT/FUTEX_WAKE by number, so this compiles in the static binaries
 * whether or not <linux/futex.h> is in scope (vmhome includes it; vmremote
 * does not). The private flag is correct even for a process-SHARED mapping in
 * the single-process case; own_init() clears it when the segment is genuinely
 * shared across processes so cross-process wakeups reach sleepers.
 */
#ifndef FUTEX_WAIT
#define FUTEX_WAIT 0
#endif
#ifndef FUTEX_WAKE
#define FUTEX_WAKE 1
#endif
#ifndef FUTEX_PRIVATE_FLAG
#define FUTEX_PRIVATE_FLAG 128
#endif

/* own_begin() outcomes. */
enum { OWN_BEGIN_CLAIMED = 0, OWN_BEGIN_BUSY = 1, OWN_BEGIN_FULL = -1 };

/* Where the token is. */
enum own_site { OWN_NONE = 0, OWN_HOME = 1, OWN_REMOTE = 2 };
/* Which way a token is moving, if any. At most one at a time per entry. */
enum own_transit { OWN_T_NONE = 0, OWN_T_TO_HOME = 1, OWN_T_TO_REMOTE = 2 };

struct own_ent {
	uint64_t as;		/* 0 = free slot                              */
	uint64_t page;
	uint32_t site;		/* enum own_site                              */
	uint32_t transit;	/* enum own_transit                           */
	uint32_t gen;		/* futex word: bumped + woken on every landing */
	uint32_t transit_owner;	/* tid that owns the in-flight move (diagnosis) */
	uint64_t transit_us;	/* when the move began (diagnosis / bound only) */
	/*
	 * How many own_wait()ers are parked on gen, so a landing wakes only
	 * when there is someone to wake. Without it every landing was a
	 * FUTEX_WAKE syscall, and on the guest fault path (one transition per
	 * fault) that was one of three futex calls per fault that woke nobody
	 * (session 38, perf trace: 2.8 futex/fault on wc, ~0 contended).
	 * The waiter increments this BEFORE it reads gen and the lander bumps
	 * gen BEFORE it reads this -- both locked RMWs, so on x86 at least one
	 * of the two sees the other: a waiter that misses the bump is counted
	 * when the lander looks, and a waiter the lander does not see has
	 * already seen the new gen (or FUTEX_WAIT refuses it with EAGAIN).
	 */
	uint32_t waiters;
	uint32_t pad[1];
};

struct own_hdr {
	uint32_t magic;
	uint32_t nslots;
	uint32_t lock;		/* futex mutex over slot alloc + field access */
	uint32_t private_futex;	/* 1 while the segment is single-process       */
	/* Counters, in the shared segment so either process reports them. */
	uint64_t n_begin;	/* transitions claimed                         */
	uint64_t n_begin_busy;	/* begins that found a move already in flight  */
	uint64_t n_land;	/* transitions landed                          */
	uint64_t n_wait;	/* waits entered                               */
	uint64_t n_wait_woken;	/* waits the landing woke                       */
	uint64_t n_wait_timeout;/* waits that hit the diagnosis bound          */
	uint64_t n_full;	/* probe chains that overflowed                */
	uint32_t pad[6];
};

/*
 * The mapping, per process. own_seg points at the shared segment; own_tab is
 * the entry array just past the header. Both are set by own_init() and read
 * lock-free thereafter (the pointers never move; the CONTENTS are guarded by
 * the header lock / per-entry futex).
 */
static struct own_hdr *own_seg;
static struct own_ent *own_tab;
static int own_seg_fd = -1;

#define OWN_MAGIC 0x4e574f76u		/* "vOWN" */
#ifndef OWN_SLOTS
#define OWN_SLOTS (1u << 18)		/* 262144 (as,page) entries per machine */
#endif

static inline long own_futex(uint32_t *w, int op, uint32_t val)
{
	int flags = op;

	if (own_seg && own_seg->private_futex)
		flags |= FUTEX_PRIVATE_FLAG;
	return syscall(SYS_futex, w, flags, val, NULL, NULL, 0);
}

static inline uint64_t own_now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000;
}

/*
 * This thread's id, asked of the kernel once. gettid is a syscall every
 * time, and it was being made five or six times per guest fault (the lock
 * owner record, the pull table, the transition owner) for a value that
 * never changes. A process forked with the cache set would carry its
 * parent's thread id; the only forks here are the guest contexts, which run
 * none of this code, and every use is diagnostic.
 */
static inline uint32_t own_tid(void)
{
	static __thread uint32_t tid;

	if (!tid)
		tid = (uint32_t)syscall(SYS_gettid);
	return tid;
}

/*
 * A futex mutex over the header word. Short critical sections only (slot
 * allocation, a handful of field stores) -- the LONG wait is own_wait() on the
 * entry's own gen word, which does not hold this. Uncontended it is one atomic
 * each way; contended it sleeps rather than spins.
 *
 * Three states (0 free, 1 held, 2 held with a sleeper), so the unlock makes
 * the FUTEX_WAKE syscall only when a sleeper can exist. The two-state form
 * woke on every unlock: two syscalls per transition, every one of them waking
 * nobody.
 */
static inline void own_lock(void)
{
	uint32_t *w = &own_seg->lock;
	uint32_t z = 0;

	if (__atomic_compare_exchange_n(w, &z, 1, 0, __ATOMIC_ACQUIRE,
					__ATOMIC_RELAXED))
		return;
	/* Contended: announce a sleeper (2) and sleep until it reads 0. */
	while (__atomic_exchange_n(w, 2, __ATOMIC_ACQUIRE) != 0)
		own_futex(w, FUTEX_WAIT, 2);
}

static inline void own_unlock(void)
{
	uint32_t *w = &own_seg->lock;

	if (__atomic_exchange_n(w, 0, __ATOMIC_RELEASE) == 2)
		own_futex(w, FUTEX_WAKE, 1);
}

/* A landing (or an abort) woke everyone parked on this entry's gen. */
static inline void own_gen_bump(struct own_ent *e)
{
	__atomic_add_fetch(&e->gen, 1, __ATOMIC_SEQ_CST);
}
static inline void own_gen_wake(struct own_ent *e)
{
	if (__atomic_load_n(&e->waiters, __ATOMIC_SEQ_CST))
		own_futex(&e->gen, FUTEX_WAKE, 0x7fffffff);	/* wake all */
}

/*
 * Create (owner) or attach the ownership segment.
 *
 * make_fd < 0 : create a fresh memfd, size it, initialise the header. Call this
 *               in main BEFORE any context is cloned, so the MAP_SHARED mapping
 *               is inherited by every child.
 * make_fd >=0 : attach an fd inherited/passed from the creator (the segment is
 *               already initialised; just map it).
 *
 * private_futex is set by the CREATOR to 1 and only cleared once the creator
 * knows the segment will be shared across processes (a context has been
 * cloned). A private futex is faster and correct while single-process; a
 * cross-process sleeper needs the shared form so its wakeup is delivered.
 */
static inline int own_init(int make_fd)
{
	size_t sz = sizeof(struct own_hdr) +
		    (size_t)OWN_SLOTS * sizeof(struct own_ent);
	int fd = make_fd;
	void *p;

	if (own_seg)
		return 0;		/* already mapped in this process */

	if (fd < 0) {
		fd = (int)syscall(SYS_memfd_create, "vmctx-ownership", 0);
		if (fd < 0)
			return -1;
		if (ftruncate(fd, (off_t)sz) < 0) {
			close(fd);
			return -1;
		}
	}
	p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		if (make_fd < 0)
			close(fd);
		return -1;
	}
	own_seg = (struct own_hdr *)p;
	own_tab = (struct own_ent *)((char *)p + sizeof(struct own_hdr));
	own_seg_fd = fd;

	if (make_fd < 0) {		/* the creator initialises the header */
		memset(p, 0, sizeof(struct own_hdr));
		own_seg->magic = OWN_MAGIC;
		own_seg->nslots = OWN_SLOTS;
		own_seg->lock = 0;
		own_seg->private_futex = 1;	/* single-process until told */
		/* The ftruncate zero-fills the entry array: as==0 is free. */
	}
	return (own_seg->magic == OWN_MAGIC) ? 0 : -1;
}

/* The fd, so main can hand it to a child that must attach explicitly. Usually
 * unnecessary -- an inherited MAP_SHARED mapping needs no re-attach -- but the
 * fd is what a fresh-clone-that-re-execs would use. */
static inline int own_fd(void) { return own_seg_fd; }

/* Called once the segment is genuinely shared across processes (a context has
 * been cloned), so sleepers are woken cross-process. Idempotent. */
static inline void own_mark_shared(void)
{
	if (own_seg)
		__atomic_store_n(&own_seg->private_futex, 0, __ATOMIC_RELEASE);
}

/* Linear-probed, never deleted (state resets to NONE instead); keyed by
 * (address space, page). Must be called with own_lock() held. Returns NULL
 * only if the table is full and create was asked. */
static inline struct own_ent *own_slot(uint64_t as, uint64_t page, int create)
{
	uint64_t h = as * 1099511628211ull + page * 14695981039346656037ull;
	uint32_t n = own_seg->nslots;
	uint32_t i;

	page &= ~(uint64_t)0xfff;
	for (i = 0; i < n; i++) {
		struct own_ent *e = &own_tab[(h + i) % n];

		if (e->as && e->as == as && e->page == page)
			return e;
		if (!e->as) {
			if (!create)
				return NULL;
			e->as = as;
			e->page = page;
			e->site = OWN_NONE;
			e->transit = OWN_T_NONE;
			e->gen = 0;
			return e;
		}
	}
	own_seg->n_full++;
	return NULL;	/* full: caller treats as "no record", never invents */
}

/* Snapshot the entry's stable fields. 0 if there is no entry (NONE/NONE). */
static inline void own_get(uint64_t as, uint64_t page,
			   enum own_site *site, enum own_transit *transit,
			   uint32_t *gen)
{
	struct own_ent *e;

	own_lock();
	e = own_slot(as, page, 0);
	if (e) {
		if (site)    *site    = (enum own_site)e->site;
		if (transit) *transit = (enum own_transit)e->transit;
		if (gen)     *gen     = e->gen;
	} else {
		if (site)    *site    = OWN_NONE;
		if (transit) *transit = OWN_T_NONE;
		if (gen)     *gen     = 0;
	}
	own_unlock();
}

/*
 * Claim the single transition. If the entry is idle, set transit = `to` and
 * return OWN_BEGIN_CLAIMED: the caller owns the move and must finish it with
 * own_land() (or own_abort() if it fails). If a move is already in flight,
 * return OWN_BEGIN_BUSY and write through *wait_gen the entry's current gen --
 * read UNDER the lock, atomically with observing transit != NONE, so the
 * landing that bumps gen cannot slip between here and own_wait() and be lost.
 * *wait_gen may be 0 (the entry has never landed). A full table returns
 * OWN_BEGIN_FULL: the caller falls back to its record-based path, never to an
 * invented page.
 */
static inline int own_begin(uint64_t as, uint64_t page, enum own_transit to,
			    uint32_t *wait_gen)
{
	struct own_ent *e;

	own_lock();
	e = own_slot(as, page, 1);
	if (!e) {
		own_unlock();
		return OWN_BEGIN_FULL;
	}
	if (e->transit != OWN_T_NONE) {
		own_seg->n_begin_busy++;
		if (wait_gen)
			*wait_gen = e->gen;
		own_unlock();
		return OWN_BEGIN_BUSY;
	}
	e->transit = (uint32_t)to;
	e->transit_owner = own_tid();
	e->transit_us = own_now_us();
	own_seg->n_begin++;
	own_unlock();
	return OWN_BEGIN_CLAIMED;
}

/*
 * Finish the transition this thread claimed with own_begin(): record where the
 * token now is, clear transit, bump gen, and wake every waiter. new_site is
 * where the token landed (HOME after a fetch home, REMOTE after a serve to the
 * destination, or unchanged/NONE if the move was abandoned and the caller is
 * putting the entry back).
 */
static inline void own_land(uint64_t as, uint64_t page, enum own_site new_site)
{
	struct own_ent *e;
	uint32_t *gw = NULL;

	own_lock();
	e = own_slot(as, page, 1);
	if (e) {
		e->site = (uint32_t)new_site;
		e->transit = OWN_T_NONE;
		e->transit_owner = 0;
		e->transit_us = 0;
		own_gen_bump(e);	/* the landing event waiters watch */
		gw = &e->gen;
		own_seg->n_land++;
	}
	own_unlock();
	if (gw)
		own_gen_wake(e);
}

/*
 * Abandon a transition without moving the token (a fetch that failed, a serve
 * refused): clear transit, leave site as it was, bump+wake so no one waits on
 * a move that is not happening. The waiters re-decide and one of them may take
 * the move.
 */
static inline void own_abort(uint64_t as, uint64_t page)
{
	struct own_ent *e;
	uint32_t *gw = NULL;

	own_lock();
	e = own_slot(as, page, 1);
	if (e && e->transit != OWN_T_NONE) {
		e->transit = OWN_T_NONE;
		e->transit_owner = 0;
		e->transit_us = 0;
		own_gen_bump(e);
		gw = &e->gen;
	}
	own_unlock();
	if (gw)
		own_gen_wake(e);
}

/*
 * Sleep until the entry lands (its gen leaves `gen`), bounded only so a lost
 * lander is a named diagnosis rather than a hang -- the bound is NOT a
 * tie-break and its expiry is not an answer; the caller re-reads the entry and
 * acts on what it truly says. Returns 1 if the landing woke it, 0 on the bound.
 *
 * max_us <= 0 waits without a bound (for a caller that has its own outer
 * deadline). The futex compares the CURRENT gen word against `gen`; if it
 * already moved, FUTEX_WAIT returns EAGAIN at once and this returns 1 -- the
 * landing beat the wait, which is exactly right.
 */
static inline int own_wait(uint64_t as, uint64_t page, uint32_t gen, int64_t max_us)
{
	struct own_ent *e;
	uint32_t *gw;
	uint64_t t0 = own_now_us();

	own_lock();
	e = own_slot(as, page, 0);
	gw = e ? &e->gen : NULL;
	own_unlock();
	if (!gw)
		return 1;		/* entry vanished: nothing to wait for */

	own_seg->n_wait++;
	/* Counted in BEFORE gen is read: see own_ent.waiters. */
	__atomic_add_fetch(&e->waiters, 1, __ATOMIC_SEQ_CST);
	for (;;) {
		uint32_t cur = __atomic_load_n(gw, __ATOMIC_SEQ_CST);
		int flags = FUTEX_WAIT;
		struct timespec ts, *tp = NULL;

		if (cur != gen) {
			own_seg->n_wait_woken++;
			__atomic_sub_fetch(&e->waiters, 1, __ATOMIC_SEQ_CST);
			return 1;	/* it landed */
		}
		if (max_us > 0) {
			int64_t left = max_us - (int64_t)(own_now_us() - t0);

			if (left <= 0) {
				own_seg->n_wait_timeout++;
				__atomic_sub_fetch(&e->waiters, 1, __ATOMIC_SEQ_CST);
				return 0;	/* the bound: a diagnosis, not an answer */
			}
			ts.tv_sec = left / 1000000;
			ts.tv_nsec = (left % 1000000) * 1000;
			tp = &ts;
		}
		if (own_seg->private_futex)
			flags |= FUTEX_PRIVATE_FLAG;
		syscall(SYS_futex, gw, flags, gen, tp, NULL, 0);
		/* loop: re-check gen (spurious wake, timeout slice, or landing) */
	}
}

/* Reset an entry to NONE/NONE (a range vacated: the mapping is gone, its
 * ownership record must not outlive it). Bumps+wakes so a waiter does not sleep
 * on a page that no longer exists. */
static inline void own_forget(uint64_t as, uint64_t page)
{
	struct own_ent *e;
	uint32_t *gw = NULL;

	own_lock();
	e = own_slot(as, page, 0);
	if (e) {
		e->site = OWN_NONE;
		e->transit = OWN_T_NONE;
		e->transit_owner = 0;
		e->transit_us = 0;
		own_gen_bump(e);
		gw = &e->gen;
	}
	own_unlock();
	if (gw)
		own_gen_wake(e);
}

static inline const char *own_site_name(enum own_site s)
{
	return s == OWN_HOME ? "HOME" : s == OWN_REMOTE ? "REMOTE" : "NONE";
}
static inline const char *own_transit_name(enum own_transit t)
{
	return t == OWN_T_TO_HOME ? "TO_HOME" :
	       t == OWN_T_TO_REMOTE ? "TO_REMOTE" : "none";
}

#endif /* VMCTX_OWN_H */
