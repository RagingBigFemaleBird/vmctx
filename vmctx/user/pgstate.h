/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Per-page ownership state machine (a conserved-token protocol).
 *
 * Write-ownership of a page is a single token. At any instant it is at exactly
 * one place: node A, node B, or in transit inside a GRANT on the wire. A node
 * may read/write only while it holds the token (OWNED), and is never OWNED while
 * a hand-off is in flight. The transient states are the real mutual exclusion:
 * they are set UNDER the lock and survive the lock being dropped across the
 * network, so a thread that takes the lock mid-hand-off sees a transient state
 * and WAITS for a stable one rather than acting on a page whose owner is
 * momentarily undefined. That is the property a held lock cannot give once it is
 * dropped for a network round trip.
 *
 * The same header is compiled into both halves (vmremote and vmhome); the table
 * is per-process (each side keeps its own view of every page), and the actions
 * -- evict, install, send, recv -- are the caller's.
 *
 * Discipline for a hand-off, and it is the whole point:
 *   prev = pg_claim_prio(as, page, <a transient>);  // atomic check-and-claim
 *   ... one slow step: evict / send / pull (NO lock held) ...
 *   pg_settle(as, page, <stable state>);            // the hand-off is done
 * A claim that finds another hand-off's transient refuses (-1); what the caller
 * does next is decided by role -- see pg_claim_prio() below.
 */
#ifndef VMCTX_PGSTATE_H
#define VMCTX_PGSTATE_H

#include <pthread.h>
#include <stdint.h>

enum pg_st {
	/* stable -- safe to act on */
	PST_INVALID = 0,	/* peer owns it (or it is in transit); must request */
	PST_SHARED,		/* both nodes hold a read-only copy */
	PST_OWNED,		/* this node holds it exclusive; may read and write */
	/* transient -- a hand-off is in flight; a lock-entry here must WAIT */
	PST_UNINSTALLING,	/* relinquishing: evicting the page locally */
	PST_TRANSFERRING,	/* relinquishing: bytes on the wire to the peer */
	PST_DOWNGRADING,	/* relinquishing write: protecting to read-only/shared */
	PST_REQUESTING,		/* acquiring: asked the peer, awaiting the verdict/bytes */
};

static inline int pg_transient(enum pg_st s) { return s >= PST_UNINSTALLING; }

static inline const char *pgst_name(enum pg_st s)
{
	switch (s) {
	case PST_INVALID:	return "INVALID";
	case PST_SHARED:	return "SHARED";
	case PST_OWNED:		return "OWNED";
	case PST_UNINSTALLING:	return "UNINSTALLING";
	case PST_TRANSFERRING:	return "TRANSFERRING";
	case PST_DOWNGRADING:	return "DOWNGRADING";
	case PST_REQUESTING:	return "REQUESTING";
	}
	return "?";
}

#define PGSTATE_N 8192
struct pg_slot { uint64_t as, page; enum pg_st st; int used; };
static struct pg_slot pgstate_tab[PGSTATE_N];
static pthread_mutex_t pgstate_mx = PTHREAD_MUTEX_INITIALIZER;

/* Linear-probed, never deleted (state resets to INVALID instead); keyed by
 * address space + page. Must be called with pgstate_mx held. */
static struct pg_slot *pg_slot_find(uint64_t as, uint64_t page, int create)
{
	uint64_t h = (as * 1099511628211ULL + page * 14695981039346656037ULL);
	int i;

	for (i = 0; i < PGSTATE_N; i++) {
		struct pg_slot *s = &pgstate_tab[(h + (uint64_t)i) % PGSTATE_N];

		if (s->used && s->as == as && s->page == page)
			return s;
		if (!s->used) {
			if (!create)
				return NULL;
			s->used = 1;
			s->as = as;
			s->page = page;
			s->st = PST_INVALID;
			return s;
		}
	}
	return NULL;	/* table full: caller treats as INVALID, no state kept */
}

/* Set the state. Must be called with pgstate_mx held. */
static void pg_set(uint64_t as, uint64_t page, enum pg_st st)
{
	struct pg_slot *s = pg_slot_find(as, page, 1);

	if (s)
		s->st = st;
}

/*
 * WHICH SIDE THIS PROCESS IS, and it is the missing half of the protocol.
 *
 * Everything above serialises the threads of ONE process. It cannot serialise
 * the two machines, because the table is per-process: each side keeps its own
 * view of every page and neither can see the other's transients. So both sides
 * can enter PST_REQUESTING for the same page at the same instant, each having
 * "atomically" claimed it against its own threads, and then each waits for the
 * other. That is a deadlock the local lock cannot even detect, and it is the one
 * observed:
 *
 *   pid A: fault 0x7ffff7ff6000 (write) unanswered 2s by monitor (vmhome)
 *   pid B: fault 0x7ffff7ff6d00 (read)  unanswered 2s by monitor (vmremote)
 *   vmhome   state D  vmctx_redirect_fault   <- blocked on its own fault
 *   vmremote state S  tcp_recvmsg            <- waiting for us
 *
 * Answering "ask again" to both is not a fix, it is a retry storm: measured
 * WORSE than the deadlock (5 failures in 22 against 4 in 40).
 *
 * A symmetric protocol cannot break a symmetric conflict. The tie needs a rule
 * that does not depend on timing, and there is one available for free: the two
 * sides are not peers. The SOURCE owns the program, its memory and its
 * syscalls, and it must make progress at all cost -- a source that yields has
 * nothing to fall back on. The destination is disposable and may always ask
 * again. So the source wins every conflict and the destination backs off to a
 * WAITING state (INVALID) instead of contending.
 */
static int pg_source_side;	/* 1 in vmhome, 0 in vmremote */
static inline void pg_set_source_side(int v) { pg_source_side = v; }
static unsigned long pg_preempted;	/* conflicts the source won */
static unsigned long pg_yielded;	/* conflicts the destination gave up */

/*
 * Atomic check-and-claim. If the page is stable, set it to `mine` (a transient)
 * and return the prior stable state; if it is already transient (another
 * hand-off owns it), return -1 without touching it. The whole thing is under one
 * lock, so two claimers can never both succeed.
 */
static int pg_try_claim(uint64_t as, uint64_t page, enum pg_st mine)
{
	struct pg_slot *s;
	enum pg_st st;

	pthread_mutex_lock(&pgstate_mx);
	s = pg_slot_find(as, page, 1);
	st = s ? s->st : PST_INVALID;
	if (pg_transient(st)) {
		pthread_mutex_unlock(&pgstate_mx);
		return -1;
	}
	if (s)
		s->st = mine;
	pthread_mutex_unlock(&pgstate_mx);
	return (int)st;
}

/*
 * Claim with the priority rule above. This is what a hand-off should use.
 *
 *   stable                     -> claim it, return the prior stable state
 *   transient, I am the SOURCE -> PREEMPT. The peer's request loses; take the
 *                                 transient and carry on. Returns the state the
 *                                 page had before the peer touched it, which is
 *                                 all the caller needs, and never blocks.
 *   transient, I am the DEST   -> -1. Do not retry in a loop: settle back to
 *                                 INVALID (waiting) and let the source finish.
 *                                 The page will be asked for again by the fault
 *                                 that still needs it, once, not spun on.
 *
 * The asymmetry is the whole point: exactly one side can win, decided by role
 * rather than by who asked first, so the conflict cannot recur on the retry.
 */
static int pg_claim_prio(uint64_t as, uint64_t page, enum pg_st mine)
{
	struct pg_slot *s;
	enum pg_st st;

	pthread_mutex_lock(&pgstate_mx);
	s = pg_slot_find(as, page, 1);
	st = s ? s->st : PST_INVALID;
	if (pg_transient(st)) {
		/*
		 * Both sides refuse a transient. What differs is what they do
		 * NEXT, and that is where the asymmetry belongs:
		 *
		 *   destination -> pg_yield_to_source(): settle back to WAITING
		 *                  and re-fault once, so the conflict is over.
		 *   source      -> keep its own transient and finish the
		 *                  hand-off it already started.
		 *
		 * The source does NOT preempt. A transient here may be another
		 * thread of THIS process mid-transfer, and stealing that
		 * duplicates or drops the conserved token: measured at suite
		 * 63/18 (sm2, sm3, an1). Local exclusion stays exactly as it
		 * was; only the cross-machine tie is decided by role.
		 */
		if (!pg_source_side)
			pg_yielded++;
		else
			pg_preempted++;
		pthread_mutex_unlock(&pgstate_mx);
		return -1;
	}
	if (s)
		s->st = mine;
	pthread_mutex_unlock(&pgstate_mx);
	return (int)st;
}

/*
 * The destination's half of the rule: give up a claim we lost and go back to
 * waiting. Named rather than open-coded so "backs off to WAITING" is one thing
 * that happens in one place.
 */
static void pg_yield_to_source(uint64_t as, uint64_t page)
{
	pthread_mutex_lock(&pgstate_mx);
	pg_set(as, page, PST_INVALID);
	pthread_mutex_unlock(&pgstate_mx);
}

#include <unistd.h>
/* Like pg_try_claim, but spin (bounded) for a page busy with another hand-off
 * rather than failing at once -- for the serve side, which would otherwise have
 * to bounce the request back over the network. Returns -1 only if still busy
 * after max_ms. */
static int pg_claim_wait(uint64_t as, uint64_t page, enum pg_st mine, int max_ms)
{
	int i;

	for (i = 0; i < max_ms * 10; i++) {
		int r = pg_try_claim(as, page, mine);

		if (r >= 0)
			return r;
		usleep(100);
	}
	return -1;
}

/* Set the final (stable) state after a hand-off completes. */
static void pg_settle(uint64_t as, uint64_t page, enum pg_st st)
{
	pthread_mutex_lock(&pgstate_mx);
	pg_set(as, page, st);
	pthread_mutex_unlock(&pgstate_mx);
}

#endif /* VMCTX_PGSTATE_H */
