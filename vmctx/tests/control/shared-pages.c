// SPDX-License-Identifier: GPL-2.0
/* Grow overlapping object views concurrently, preserving old bytes and a
 * neighboring object. Logical sparse offsets must not inflate the backing. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include "../../user/shared-pages.h"
static int fd;
static void *grow(void *unused)
{
	(void)unused;
	for (uint64_t i = 0; i < 8192; i++) {
		off_t a = shared_page_slot(fd, 71, i * 4096, 1);
		off_t b = shared_page_slot(fd, 72, (UINT64_C(1) << 60) + i * 4096, 1);
		assert(a >= 0 && b >= 0 && a != b);
		assert(shared_page_slot(fd, 71, i * 4096, 0) == a);
	}
	return NULL;
}
int main(void)
{
	fd = memfd_create("shared-identity", MFD_CLOEXEC);
	assert(fd >= 0);
	assert(shared_page_slot(fd, 71, 0, 0) == -1 && errno == ENOENT);
	off_t first = shared_page_slot(fd, 71, 0, 1);
	off_t neighbor = shared_page_slot(fd, 72, 0, 1);
	assert(first == 0 && neighbor != first);
	uint64_t a = 0x123456789abcdef, b = 0xfeedccddabdecdef, got;
	assert(pwrite(fd, &a, sizeof(a), first) == sizeof(a));
	assert(pwrite(fd, &b, sizeof(b), neighbor) == sizeof(b));
	pthread_t t[4];
	for (int i = 0; i < 4; i++) assert(!pthread_create(&t[i], NULL, grow, NULL));
	for (int i = 0; i < 4; i++) assert(!pthread_join(t[i], NULL));
	assert(shared_page_slot(fd, 71, 0, 0) == first);
	assert(shared_page_slot(fd, 72, 0, 0) == neighbor);
	assert(pread(fd, &got, sizeof(got), first) == sizeof(got) && got == a);
	assert(pread(fd, &got, sizeof(got), neighbor) == sizeof(got) && got == b);
	struct stat st; assert(!fstat(fd, &st));
	assert(st.st_size == (8192 * 2 + 1) * 4096);
	assert(shared_page_slot(fd, 0, 0, 1) < 0 && errno == EINVAL);
	assert(shared_page_slot(fd, 71, 1, 1) < 0 && errno == EINVAL);
	assert(shared_page_slot(fd, 71, UINT64_MAX - 4095, 1) < 0 && errno == EINVAL);
	close(fd);
	puts("PASS: concurrent shared growth preserves aliases, neighbor bytes and sparse page identity");
	return 0;
}
