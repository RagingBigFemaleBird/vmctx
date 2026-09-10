/* SPDX-License-Identifier: GPL-2.0 */
/* Executor mapping metadata, keyed by address space. Values describe the
 * source's shared-object offsets and permissions; no source OS ABI is used.
 * Lookups return copies. Never hold this lock across a kernel or network call.
 * These records do not supply source commit ordering or page incarnations. */
#ifndef VMCTX_SHARED_LAYOUT_H
#define VMCTX_SHARED_LAYOUT_H
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "map-trace.h"

struct shared_range {
	uint64_t as, st, en, off, object_id;
	unsigned int prot;
};
static struct shared_range *shared_rr;
static size_t shared_rn, shared_capacity;
static pthread_mutex_t shared_layout_lock = PTHREAD_MUTEX_INITIALIZER;

static void shared_layout_reserve(size_t need)
{
	if (need <= shared_capacity) return;
	size_t cap = shared_capacity ? shared_capacity : 64;
	while (cap < need && cap <= SIZE_MAX / 2) cap *= 2;
	struct shared_range *next = NULL;
	if (cap >= need && cap <= SIZE_MAX / sizeof(*next))
		next = calloc(cap, sizeof(*next));
	if (!next) {
		fputs("[vmremote] cannot retain shared mapping metadata; stopping\n", stderr);
		abort();
	}
	for (size_t i = 0; i < shared_rn; i++) next[i] = shared_rr[i];
	free(shared_rr);
	shared_rr = next;
	shared_capacity = cap;
}

/* Caller holds the lock and has reserved two spare entries. */
static void shared_layout_remove_locked(uint64_t as, uint64_t st, uint64_t en)
{
	for (size_t i = 0; i < shared_rn; ) {
		struct shared_range old = shared_rr[i];
		if (old.as != as || old.en <= st || old.st >= en) { i++; continue; }
		map_trace("executor", "shared-remove as=%llu range=%llx-%llx removes=%llx-%llx off=%llx",
			(unsigned long long)as, (unsigned long long)st, (unsigned long long)en,
			(unsigned long long)old.st, (unsigned long long)old.en,
			(unsigned long long)old.off);
		if (old.st < st) {
			shared_rr[i++].en = st;
			if (old.en > en) {
				old.off += en - old.st;
				old.st = en;
				shared_rr[shared_rn++] = old;
			}
		} else if (old.en > en) {
			shared_rr[i].off += en - old.st;
			shared_rr[i++].st = en;
		} else shared_rr[i] = shared_rr[--shared_rn];
	}
}

static void shared_layout_forget(uint64_t as, uint64_t st, uint64_t en)
{
	if (en <= st) return;
	pthread_mutex_lock(&shared_layout_lock);
	shared_layout_reserve(shared_rn + 2);
	shared_layout_remove_locked(as, st, en);
	pthread_mutex_unlock(&shared_layout_lock);
}

static void shared_layout_note(uint64_t as, uint64_t st, uint64_t en,
			       uint64_t off, unsigned int prot, uint64_t object_id)
{
	if (!object_id || en <= st || off > INT64_MAX ||
	    en - st > INT64_MAX - off) abort();
	pthread_mutex_lock(&shared_layout_lock);
	shared_layout_reserve(shared_rn + 2);
	shared_layout_remove_locked(as, st, en);
	shared_rr[shared_rn++] = (struct shared_range){as, st, en, off, object_id, prot};
	map_trace("executor", "shared-note as=%llu range=%llx-%llx off=%llx prot=%u",
		(unsigned long long)as, (unsigned long long)st, (unsigned long long)en,
		(unsigned long long)off, prot);
	pthread_mutex_unlock(&shared_layout_lock);
}

static int shared_layout_lookup(uint64_t as, uint64_t addr, struct shared_range *out)
{
	int found = 0;
	pthread_mutex_lock(&shared_layout_lock);
	for (size_t i = 0; i < shared_rn; i++) {
		if (shared_rr[i].as != as || addr < shared_rr[i].st || addr >= shared_rr[i].en) continue;
		if (out) *out = shared_rr[i];
		found = 1;
		break;
	}
	pthread_mutex_unlock(&shared_layout_lock);
	return found;
}

static void shared_layout_inherit(uint64_t child, uint64_t parent)
{
	if (child == parent) return; /* Threads already use the same records. */
	pthread_mutex_lock(&shared_layout_lock);
	size_t n = shared_rn, count = 0;
	for (size_t i = 0; i < n; i++) count += shared_rr[i].as == parent;
	shared_layout_reserve(n + count + 2);
	shared_layout_remove_locked(child, 0, UINT64_MAX);
	n = shared_rn;
	for (size_t i = 0; i < n; i++) {
		if (shared_rr[i].as != parent) continue;
		struct shared_range copy = shared_rr[i];
		copy.as = child;
		shared_rr[shared_rn++] = copy;
	}
	pthread_mutex_unlock(&shared_layout_lock);
}

static void shared_layout_protect(uint64_t as, uint64_t st, uint64_t en,
				  unsigned int prot)
{
	if (en <= st) return;
	pthread_mutex_lock(&shared_layout_lock);
	/* Splitting at each boundary adds at most two entries. */
	shared_layout_reserve(shared_rn + 2);
	size_t n = shared_rn;
	for (size_t i = 0; i < n; i++) {
		struct shared_range old = shared_rr[i];
		if (old.as != as || old.en <= st || old.st >= en) continue;
		uint64_t lo = old.st > st ? old.st : st;
		uint64_t hi = old.en < en ? old.en : en;
		if (old.st < lo) {
			struct shared_range left = old;
			left.en = lo;
			shared_rr[shared_rn++] = left;
		}
		if (old.en > hi) {
			struct shared_range right = old;
			right.st = hi;
			right.off += hi - old.st;
			shared_rr[shared_rn++] = right;
		}
		shared_rr[i] = (struct shared_range){as, lo, hi, old.off + lo - old.st, old.object_id, prot};
	}
	pthread_mutex_unlock(&shared_layout_lock);
}
#endif
