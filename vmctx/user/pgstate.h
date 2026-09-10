/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Process-local exclusion for page transfers in vmremote. The transient state
 * survives dropping the mutex for page and network operations. A successful
 * claimant must settle its own transfer; a failed claimant must leave the
 * incumbent unchanged. This table neither arbitrates between machines nor
 * proves where the current bytes or writable mappings reside.
 *
 * Entries currently survive for the process lifetime. Address-space retirement
 * and transfer identities must be supplied by the surrounding protocol.
 */
#ifndef VMCTX_PGSTATE_H
#define VMCTX_PGSTATE_H

#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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

#define PGSTATE_N 8192 /* initial capacity; grows without dropping claims */
struct pg_slot { uint64_t as, page, episode; enum pg_st st; int used; };
/* A reservation names one acquisition, not just a reusable page key. It may
 * be passed to a different worker, but only its exact episode can finish. */
struct pg_claim { uint64_t as, page, episode; enum pg_st prior; };
static struct pg_slot *pgstate_tab;
static size_t pgstate_capacity, pgstate_used;
static unsigned long pgstate_grows;
static pthread_mutex_t pgstate_mx = PTHREAD_MUTEX_INITIALIZER;

static uint64_t pg_slot_hash(uint64_t as, uint64_t page)
{
	uint64_t h = as * 1099511628211ULL ^
		     (page >> 12) * 14695981039346656037ULL;

	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdULL;
	h ^= h >> 33;
	return h;
}

/* All slot pointers stay under pgstate_mx, so growth can move the array.
 * Allocation failure must stop the monitor: a successful unrecorded claim
 * would allow two transfers to destroy each other's only copy of a page. */
static void pgstate_grow(void)
{
	size_t cap;
	struct pg_slot *next;

	if (pgstate_capacity > SIZE_MAX / 2 / sizeof(*next))
		goto failed;
	cap = pgstate_capacity ? pgstate_capacity * 2 : PGSTATE_N;
	next = calloc(cap, sizeof(*next));
	if (!next)
		goto failed;
	for (size_t i = 0; i < pgstate_capacity; i++) {
		struct pg_slot *old = &pgstate_tab[i];
		size_t pos;

		if (!old->used)
			continue;
		pos = pg_slot_hash(old->as, old->page) & (cap - 1);
		while (next[pos].used)
			pos = (pos + 1) & (cap - 1);
		next[pos] = *old;
	}
	free(pgstate_tab);
	pgstate_tab = next;
	pgstate_capacity = cap;
	pgstate_grows++;
	fprintf(stderr, "vmctx: page-transfer table capacity=%zu tracked=%zu bytes=%zu growths=%lu\n",
		cap, pgstate_used, cap * sizeof(*next), pgstate_grows);
	return;
failed:
	fputs("vmctx: cannot grow the page-transfer table; refusing an untracked transfer\n", stderr);
	abort();
}

/* Linear-probed, never deleted (state resets to INVALID instead); keyed by
 * address space + page. Must be called with pgstate_mx held. */
static struct pg_slot *pg_slot_find(uint64_t as, uint64_t page, int create)
{
	uint64_t h;
	size_t pos;

	page &= ~UINT64_C(4095);
	h = pg_slot_hash(as, page);
	if (!pgstate_capacity) {
		if (!create)
			return NULL;
		pgstate_grow();
	}
	pos = h & (pgstate_capacity - 1);
	while (pgstate_tab[pos].used) {
		struct pg_slot *s = &pgstate_tab[pos];

		if (s->as == as && s->page == page)
			return s;
		pos = (pos + 1) & (pgstate_capacity - 1);
	}
	if (!create)
		return NULL;
	if (pgstate_used >= pgstate_capacity / 2) {
		pgstate_grow();
		pos = h & (pgstate_capacity - 1);
		while (pgstate_tab[pos].used)
			pos = (pos + 1) & (pgstate_capacity - 1);
	}
	pgstate_tab[pos] = (struct pg_slot) {
		.as = as, .page = page, .st = PST_INVALID, .used = 1,
	};
	pgstate_used++;
	return &pgstate_tab[pos];
}

/*
 * Atomic check-and-claim. If the page is stable, set it to `mine` (a transient)
 * and return the prior stable state; if it is already transient (another
 * hand-off owns it), return -1 without touching it. The whole thing is under one
 * lock, so two claimers can never both succeed.
 */
static int pg_try_claim(uint64_t as, uint64_t page, enum pg_st mine,
		       struct pg_claim *claim)
{
	struct pg_slot *s;
	enum pg_st st;

	if (!claim || !pg_transient(mine) || mine > PST_REQUESTING) {
		errno = EINVAL;
		return -1;
	}
	/* Do not overwrite a caller's outstanding reservation. */
	if (claim->episode) { errno = EALREADY; return -1; }
	page &= ~UINT64_C(4095);
	pthread_mutex_lock(&pgstate_mx);
	s = pg_slot_find(as, page, 1);
	st = s->st;
	if (pg_transient(st)) {
		pthread_mutex_unlock(&pgstate_mx);
		errno = EBUSY;
		return -1;
	}
	if (s->episode == UINT64_MAX) {
		pthread_mutex_unlock(&pgstate_mx);
		fputs("vmctx: transfer episode exhausted; refusing identity reuse\n", stderr);
		abort();
	}
	s->episode++;
	s->st = mine;
	*claim = (struct pg_claim){as, page, s->episode, st};
	pthread_mutex_unlock(&pgstate_mx);
	return (int)st;
}

#include <unistd.h>
/* Like pg_try_claim, but spin (bounded) for a page busy with another hand-off
 * rather than failing at once -- for the serve side, which would otherwise have
 * to bounce the request back over the network. Returns -1 only if still busy
 * after max_ms. */
static inline int pg_claim_wait(uint64_t as, uint64_t page, enum pg_st mine, int max_ms,
			 struct pg_claim *claim)
{
	int i;

	for (i = 0; i < max_ms * 10; i++) {
		int r = pg_try_claim(as, page, mine, claim);

		if (r >= 0 || errno != EBUSY)
			return r;
		usleep(100);
	}
	return -1;
}

/* Finish only this reservation. Failed or repeated completion cannot alter a
 * later owner, even after table growth or another transfer of identical bytes.
 * Validation precedes mutation; a bad final state leaves the claim usable. */
static int pg_finish(struct pg_claim *claim, enum pg_st st)
{
	if (!claim || !claim->episode || st < PST_INVALID || pg_transient(st)) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&pgstate_mx);
	struct pg_slot *s = pg_slot_find(claim->as, claim->page, 0);
	if (!s || !pg_transient(s->st) || s->episode != claim->episode) {
		pthread_mutex_unlock(&pgstate_mx);
		errno = ESTALE;
		return -1;
	}
	s->st = st;
	pthread_mutex_unlock(&pgstate_mx);
	claim->episode = 0;
	return 0;
}

/* Production callers cannot continue after losing their reservation. */
static void pg_finish_required(struct pg_claim *claim, enum pg_st st)
{
	if (pg_finish(claim, st)) {
		perror("vmctx: finishing an unowned page transfer");
		abort();
	}
}

/* A request keeps its reservation through reply construction/transmission.
 * Return, continue and an I/O error all release exactly the acquired episode.
 * Callers publish a final stable state once their local storage work commits. */
struct pg_request_claim {
	struct pg_claim claim;
	enum pg_st final;
};
static inline void pg_request_release(struct pg_request_claim *request)
{
	if (request->claim.episode)
		pg_finish_required(&request->claim, request->final);
}

#endif /* VMCTX_PGSTATE_H */
