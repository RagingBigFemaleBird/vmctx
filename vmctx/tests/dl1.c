/*
 * dl1 — the dynamic loader's boundary page, reduced to its mechanism.
 *
 * netsurf under the #146 pair dies at 7-9s inside ld.so's relocation of a
 * freshly dlopened library: the guest's store into the library's data page
 * becomes an unrecoverable #PF because the source's TAKE of that page is
 * claim-refused even AFTER the zap — refs=3 want=2 mapcount=1 anon=0. A
 * file-backed folio still mapped ONCE after this context's mapping was
 * zapped, which is exactly what an ELF boundary page looks like: the same
 * file page sits at the tail of one PT_LOAD and the head of the next, so
 * ld.so maps it TWICE in one mm, and unmapping the written alias leaves the
 * other alias holding the page-cache folio. The kernel's break at the
 * refusal site (vmctx_break_cow_share) is gated on the ANONYMOUS share
 * do_wp_page can copy; a clean private FILE page is not that, so nothing
 * breaks the impasse and the fault deadline ends the guest (242).
 *
 * This test builds that state hermetically, no dlopen and no timing race:
 *
 *   pages 0..2 of a real file are mapped read-only at A (the "text" alias),
 *   pages 1..3 are mapped read-write MAP_PRIVATE at B (the "data" alias),
 *   so the file's page 1 is mapped twice — the boundary page;
 *   the guest then stores into B's first page (the relocation), reads the
 *   store back, and pumps the page through forwarded syscalls so the source
 *   must take it.
 *
 * Everything is checked: the store must read back, the bytes around it must
 * still be the file's, and the read-only alias must go on showing the
 * PRISTINE file page (a private write must never bleed through the page
 * cache). Death by 242 before the prints is the netsurf shape itself.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define PG 4096
#define ROUNDS 200

int main(void)
{
	char path[] = "/tmp/dl1.XXXXXX";
	static char pattern[3 * PG];
	char *a, *b;
	int fd, i, r, bad = 0;
	int devnull, pfd[2];

	for (i = 0; i < (int)sizeof(pattern); i++)
		pattern[i] = (char)('A' + (i / PG)) ^ (char)(i & 0x3f);

	fd = mkstemp(path);
	if (fd < 0 || write(fd, pattern, sizeof(pattern)) !=
	    (ssize_t)sizeof(pattern)) {
		printf("dl1: cannot build the file\n");
		return 1;
	}
	/* The file's pages are the loader's segments from here on. */
	a = mmap(NULL, 2 * PG, PROT_READ, MAP_PRIVATE, fd, 0);
	b = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, PG);
	if (a == MAP_FAILED || b == MAP_FAILED) {
		printf("dl1: mmap failed\n");
		return 1;
	}
	printf("dl1: text alias %p..%p, data alias %p..%p; the boundary "
	       "page is file page 1 (%p and %p)\n",
	       (void *)a, (void *)(a + 2 * PG), (void *)b,
	       (void *)(b + 2 * PG), (void *)(a + PG), (void *)b);
	fflush(stdout);

	if (pipe(pfd) < 0) {
		printf("dl1: pipe failed\n");
		return 1;
	}
	devnull = open("/dev/null", O_WRONLY);

	/*
	 * Touch the read-only alias THROUGH A SYSCALL once: the source's
	 * write(2) reads a+PG, which faults the page-cache folio into the
	 * adopted mm at a's address. From here on the folio is mapped
	 * source-side at an address no take of b's alias will ever zap —
	 * the boundary-page state exactly.
	 */
	if (write(devnull, a + PG, 64) != 64)
		printf("dl1: the alias prime write failed\n");

	for (r = 1; r <= ROUNDS; r++) {
		char got[64];
		char tag[64];
		volatile char sink;
		int n;

		/*
		 * A fresh data alias each round — every dlopen maps NEW
		 * addresses, and the state that kills netsurf is built at
		 * first touch, not carried over.
		 */
		if (r > 1) {
			munmap(b, 2 * PG);
			b = mmap(NULL, 2 * PG, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE, fd, PG);
			if (b == MAP_FAILED) {
				printf("dl1: round %d remap failed\n", r);
				bad++;
				break;
			}
		}

		/* The loader reads the addend BEFORE it stores: the first
		 * touch of the boundary page is a READ, which is what pulls
		 * the page-cache folio rather than an anonymous copy. */
		sink = b[8];
		(void)sink;

		/* The relocation: a store into the data alias's first page —
		 * the boundary page, whose folio the text alias still maps. */
		n = snprintf(tag, sizeof(tag), "reloc-%08d", r);
		memcpy(b + 16, tag, (size_t)n);

		/* The store must read back through this mapping... */
		if (memcmp(b + 16, tag, (size_t)n) != 0) {
			bad++;
			if (bad <= 4)
				printf("dl1 round %d: the store did not read "
				       "back\n", r);
		}
		/* ...the bytes beside it must still be the file's... */
		if (memcmp(b + 16 + n, pattern + PG + 16 + n, 64) != 0) {
			bad++;
			if (bad <= 4)
				printf("dl1 round %d: bytes beside the store "
				       "are not the file's\n", r);
		}
		/* ...and the read-only alias must stay PRISTINE: a private
		 * write that shows through the other alias went into the
		 * page cache, which is corruption of every other user of
		 * the file. */
		if (memcmp(a + PG + 16, pattern + PG + 16, 64) != 0) {
			bad++;
			if (bad <= 4)
				printf("dl1 round %d: the read-only alias no "
				       "longer shows the file (the private "
				       "store bled through)\n", r);
		}

		/* Pump the boundary page through forwarded syscalls so the
		 * source has to keep taking it: write it out, and read fresh
		 * bytes back INTO it (the loader keeps relocating). */
		if (write(devnull, b, 128) != 128) {
			bad++;
			if (bad <= 4)
				printf("dl1 round %d: write of the mapped "
				       "page failed\n", r);
		}
		n = snprintf(tag, sizeof(tag), "pipe-%08d", r);
		if (write(pfd[1], tag, (size_t)n) != n ||
		    read(pfd[0], b + 256, (size_t)n) != n) {
			bad++;
			if (bad <= 4)
				printf("dl1 round %d: pipe round trip through "
				       "the mapped page failed\n", r);
		} else if (memcmp(b + 256, tag, (size_t)n) != 0) {
			bad++;
			if (bad <= 4)
				printf("dl1 round %d: the pipe's bytes read "
				       "back wrong from the mapped page\n", r);
			memcpy(got, b + 256, (size_t)n);
		}
	}

	printf("dl1: %d rounds on the boundary page, %d bad\n", ROUNDS, bad);
	printf("%s\n", bad == 0 ? "PASS" : "FAIL");
	unlink(path);
	return bad == 0 ? 0 : 1;
}
