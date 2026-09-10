// SPDX-License-Identifier: GPL-2.0
/* Shrink/regrow changes page contents without changing object identity.
 * Check both surviving aliases and a later remap after the last alias ends. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#define PAGE 4096
int main(void)
{
	int fd = memfd_create("resize-object", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, 4 * PAGE)) return 2;
	for (unsigned live = 0; live < 2; live++) {
		uint64_t *a = mmap(NULL, 4 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		uint64_t *b = mmap(NULL, 3 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PAGE);
		if (a == MAP_FAILED || b == MAP_FAILED) return 2;
		for (unsigned i = 0; i < 4 * PAGE / 8; i++) a[i] = UINT64_C(0xaabbccdd00000000) + i;
		if (!live && (munmap(a, 4 * PAGE) || munmap(b, 3 * PAGE))) return 2;
		if (ftruncate(fd, PAGE) || ftruncate(fd, 4 * PAGE)) return 2;
		if (!live) {
			a = mmap(NULL, 4 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			b = mmap(NULL, 3 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, PAGE);
			if (a == MAP_FAILED || b == MAP_FAILED) return 2;
		}
		for (unsigned i = 0; i < 4 * PAGE / 8; i++) {
			uint64_t want = i < PAGE / 8 ? UINT64_C(0xaabbccdd00000000) + i : 0;
			if (a[i] != want || (i >= PAGE / 8 && b[i - PAGE / 8] != want)) {
				fprintf(stderr, "FAIL: shared resize live=%u offset=%u got=%llx want=%llx\n",
					live, i * 8, (unsigned long long)a[i], (unsigned long long)want);
				return 1;
			}
		}
		b[1] = UINT64_C(0x123456789abcdef);
		if (a[PAGE / 8 + 1] != b[1]) return 1;
		if (munmap(a, 4 * PAGE) || munmap(b, 3 * PAGE)) return 2;
	}
	close(fd);
	puts("PASS: shared shrink/regrow retires old bytes while preserving alias identity");
	return 0;
}
