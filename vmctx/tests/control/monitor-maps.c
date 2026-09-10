// SPDX-License-Identifier: GPL-2.0
/* Exercise real mappings, including two distinct objects with the same name. */
#define _GNU_SOURCE
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../../user/monitor-maps.h"
static int object(const char *name)
{
	int fd = memfd_create(name, MFD_CLOEXEC);
	assert(fd >= 0 && !ftruncate(fd, 4096));
	return fd;
}
int main(void)
{
	int metadata = object("vmctx-ownership"), guest = object("vmctx-ownership");
	assert(monitor_guest_maps(metadata, NULL) == 0);
	void *table = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, metadata, 0);
	assert(table != MAP_FAILED && monitor_guest_maps(metadata, NULL) == 0);
	void *data = mmap(NULL, 4096, PROT_READ, MAP_SHARED, guest, 0);
	assert(data != MAP_FAILED && monitor_guest_maps(metadata, NULL) == 1);
	int alias = dup(metadata);
	assert(alias >= 0 && monitor_guest_maps(alias, NULL) == 1);
	assert(!munmap(data, 4096) && monitor_guest_maps(metadata, NULL) == 0);
	assert(!munmap(table, 4096));
	close(alias); close(guest); close(metadata);
	assert(monitor_guest_maps(metadata, NULL) == -1 && errno == EBADF);
	puts("PASS: monitor mapping guard allows the metadata object, rejects another object with the same name, and fails on an invalid descriptor");
	return 0;
}
