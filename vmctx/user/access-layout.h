/* SPDX-License-Identifier: GPL-2.0 */
/* Access restrictions survive lazy mappings and cached bytes. Keep the base
 * rights separate from a guard: changing protection cannot remove a guard.
 * Callers serialize journal publication; readers take copies under this lock.
 * No network or kernel operation runs while access_state_lock is held. */
#ifndef VMCTX_ACCESS_LAYOUT_H
#define VMCTX_ACCESS_LAYOUT_H
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include "vmrproto.h"

struct access_range {
	uint64_t as, start, end, sequence;
	unsigned int protection, denied;
};
static struct access_range *access_ranges;
static size_t access_count, access_capacity;
static pthread_mutex_t access_state_lock = PTHREAD_MUTEX_INITIALIZER;

static void access_reserve(size_t count)
{
	if (count <= access_capacity) return;
	size_t n = access_capacity ? access_capacity : 64;
	while (n < count && n <= SIZE_MAX / 2) n *= 2;
	if (n < count || n > SIZE_MAX / sizeof(*access_ranges)) abort();
	void *p = realloc(access_ranges, n * sizeof(*access_ranges));
	if (!p) abort();
	access_ranges = p;
	access_capacity = n;
}

/* Nonoverlapping intervals. Removing an interior span adds at most one row. */
static void access_remove_locked(uint64_t as, uint64_t start, uint64_t end)
{
	access_reserve(access_count + 1);
	for (size_t i = 0; i < access_count; ) {
		struct access_range r = access_ranges[i];
		if (r.as != as || r.end <= start || r.start >= end) { i++; continue; }
		if (r.start < start) {
			access_ranges[i++].end = start;
			if (r.end > end) { r.start = end; access_ranges[access_count++] = r; }
		} else if (r.end > end) access_ranges[i++].start = end;
		else access_ranges[i] = access_ranges[--access_count];
	}
}

static inline void access_forget(uint64_t as)
{
	pthread_mutex_lock(&access_state_lock);
	access_remove_locked(as, 0, UINT64_MAX);
	pthread_mutex_unlock(&access_state_lock);
}

/* Return a covering interval, or the gap ending at the next known interval. */
static int access_lookup(uint64_t as, uint64_t addr, uint64_t end,
			 struct access_range *out)
{
	int found = 0;
	*out = (struct access_range){.as = as, .start = addr, .end = end};
	pthread_mutex_lock(&access_state_lock);
	for (size_t i = 0; i < access_count; i++) {
		struct access_range r = access_ranges[i];
		if (r.as != as || r.end <= addr) continue;
		if (r.start <= addr) { *out = r; found = 1; break; }
		if (r.start < out->end) out->end = r.start;
	}
	pthread_mutex_unlock(&access_state_lock);
	return found;
}

static void access_note(struct access_range r)
{
	pthread_mutex_lock(&access_state_lock);
	access_reserve(access_count + 2);
	access_remove_locked(r.as, r.start, r.end);
	access_ranges[access_count++] = r;
	pthread_mutex_unlock(&access_state_lock);
}

static void access_inherit(uint64_t child, uint64_t parent)
{
	if (child == parent) return;
	pthread_mutex_lock(&access_state_lock);
	access_remove_locked(child, 0, UINT64_MAX);
	size_t n = access_count;
	access_reserve(n * 2 + 1);
	for (size_t i = 0; i < n; i++) {
		if (access_ranges[i].as != parent) continue;
		struct access_range r = access_ranges[i];
		r.as = child;
		r.sequence = 0; /* The child has its own journal and sequence. */
		access_ranges[access_count++] = r;
	}
	pthread_mutex_unlock(&access_state_lock);
}
#endif
