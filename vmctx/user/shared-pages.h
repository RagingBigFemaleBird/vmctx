/* SPDX-License-Identifier: GPL-2.0 */
/* A session's opaque (object, page offset) has one stable local backing slot.
 * Slots need not be contiguous. Growth and sparse offsets allocate only pages
 * actually requested; no object reserves another object's future address space.
 * This table names bytes, not ownership. The page-transfer protocol still has
 * to supply current bytes before the native adapter may map a slot. */
#ifndef VMCTX_SHARED_PAGES_H
#define VMCTX_SHARED_PAGES_H
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

struct shared_page_identity { uint64_t object, offset, slot; };
static struct shared_page_identity *shared_pages;
static size_t shared_pages_used, shared_pages_capacity;
static uint64_t shared_pages_extent;
static int shared_pages_fd = -1;
static pthread_mutex_t shared_pages_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t shared_page_hash(uint64_t object, uint64_t offset)
{
	uint64_t x = object ^ (offset + UINT64_C(0x9e3779b97f4a7c15));
	x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
	return x ^ (x >> 31);
}

/* Caller holds shared_pages_lock. Publish only a fully rehashed table. */
static int shared_pages_grow(void)
{
	size_t cap = shared_pages_capacity ? shared_pages_capacity * 2 : 256;
	if (cap < shared_pages_capacity || cap > SIZE_MAX / sizeof(*shared_pages)) {
		errno = EOVERFLOW; return -1;
	}
	struct shared_page_identity *next = calloc(cap, sizeof(*next));
	if (!next) return -1;
	for (size_t i = 0; i < shared_pages_capacity; i++) {
		struct shared_page_identity row = shared_pages[i];
		if (!row.object) continue;
		size_t at = shared_page_hash(row.object, row.offset) & (cap - 1);
		while (next[at].object) at = (at + 1) & (cap - 1);
		next[at] = row;
	}
	free(shared_pages);
	shared_pages = next;
	shared_pages_capacity = cap;
	return 0;
}

/* A lookup with create=0 never allocates. The descriptor and slots live for
 * the whole executor session; no reuse is possible while an alias survives.
 * No network call or target-mm operation runs under this lock. */
static off_t shared_page_slot(int fd, uint64_t object, uint64_t offset, int create)
{
	off_t result = -1;
	int error = ENOENT;
	if (fd < 0 || !object || (offset & 4095) || offset > INT64_MAX - 4095) {
		errno = EINVAL; return -1;
	}
	pthread_mutex_lock(&shared_pages_lock);
	if (shared_pages_fd >= 0 && shared_pages_fd != fd) {
		error = EINVAL; goto out;
	}
	if (shared_pages_capacity) {
		size_t at = shared_page_hash(object, offset) & (shared_pages_capacity - 1);
		while (shared_pages[at].object) {
			if (shared_pages[at].object == object && shared_pages[at].offset == offset) {
				result = shared_pages[at].slot;
				goto out;
			}
			at = (at + 1) & (shared_pages_capacity - 1);
		}
	}
	if (!create) goto out;
	if (shared_pages_used >= (uint64_t)INT64_MAX / 4096) {
		error = EOVERFLOW; goto out;
	}
	if (shared_pages_used >= shared_pages_capacity / 2 && shared_pages_grow()) {
		error = errno; goto out;
	}
	if (shared_pages_fd < 0) {
		struct stat st;
		if (fstat(fd, &st)) { error = errno; goto out; }
		if (st.st_size < 0) { error = EINVAL; goto out; }
		shared_pages_extent = st.st_size;
		shared_pages_fd = fd;
	}
	uint64_t slot = (uint64_t)shared_pages_used * 4096;
	if (slot + 4096 > shared_pages_extent) {
		if (ftruncate(fd, slot + 4096)) { error = errno; goto out; }
		shared_pages_extent = slot + 4096;
	}
	size_t at = shared_page_hash(object, offset) & (shared_pages_capacity - 1);
	while (shared_pages[at].object) at = (at + 1) & (shared_pages_capacity - 1);
	shared_pages[at] = (struct shared_page_identity){object, offset, slot};
	shared_pages_used++;
	result = slot;
out:
	pthread_mutex_unlock(&shared_pages_lock);
	if (result < 0) errno = error;
	return result;
}
#endif
