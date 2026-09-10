// SPDX-License-Identifier: GPL-2.0
/* The same source file page at two different mmap offsets is one object. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
	int fd = memfd_create("shared-alias", MFD_CLOEXEC);
	unsigned char initial[8192];
	volatile unsigned char *tail, *whole;
	if (fd < 0 || ftruncate(fd, sizeof(initial))) return 2;
	memset(initial, 0x5a, sizeof(initial));
	if (pwrite(fd, initial, sizeof(initial), 0) != sizeof(initial)) return 2;
	/* Query the nonzero offset first: its slot must still belong to the
	 * same object extent as a later mapping that begins at file offset 0. */
	tail = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 4096);
	whole = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (tail == MAP_FAILED || whole == MAP_FAILED) return 2;
	close(fd);
	if (whole[4096] != 0x5a || tail[0] != 0x5a) return 1;
	tail[17] = 0xa7;
	if (whole[4096 + 17] != 0xa7) {
		puts("FAIL: nonzero-offset alias did not update the whole mapping");
		return 1;
	}
	whole[4096 + 2048] = 0xc3;
	if (tail[2048] != 0xc3 || whole[17] != 0x5a) {
		puts("FAIL: full mapping did not preserve offset identity");
		return 1;
	}
	munmap((void *)whole, 8192);
	if (tail[17] != 0xa7 || tail[2048] != 0xc3) return 1;
	munmap((void *)tail, 4096);
	puts("PASS: shared aliases preserve object identity across different file offsets and unmap");
	return 0;
}
