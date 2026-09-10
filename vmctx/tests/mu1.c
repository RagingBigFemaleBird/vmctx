// SPDX-License-Identifier: GPL-2.0
/*
 * mu1 — memory that was unmapped must not come back.
 *
 * An munmap ends a range. A later mmap that returns the same addresses is NEW
 * memory, and new anonymous memory reads as zero: that is the guarantee every
 * allocator on earth is built on, and the one this system can break in a way
 * the program cannot see.
 *
 * The source performs the munmap and forgets its own records for the range --
 * pgown_forget_range(), relro_forget(), sh_file_forget() -- under a comment
 * that says exactly why: "the ownership record must not outlive the mapping it
 * described". Nothing in that path tells the DESTINATION, whose backing object
 * holds the bytes. If the object keeps the page, the next fault at that address
 * is answered from the object -- "a page this context's own object already
 * holds is the context's memory" -- and the program is handed the contents of
 * memory it freed.
 *
 * Found by instrumenting netsurf rather than by counting its deaths. Its last
 * four forwarded calls before dying were an allocator's alignment dance:
 *
 *   mmap(0, 0x8000000)        -> 0x7fffeb200000
 *   munmap(0x7fffeb200000, 0xe00000)      <- the unaligned head
 *   munmap(0x7ffff0000000, 0x3200000)     <- the unaligned tail
 *   mprotect(0x7fffec000000, 0x21000, PROT_READ|PROT_WRITE)
 *
 * and then a write through a NULL pointer. An allocator that reads a stale
 * non-zero word where it expects zeroed memory produces exactly that: a pointer
 * field that was never written because the code believed it already held zero.
 *
 * The shape is the allocator's, not a minimal one, and deliberately: the bytes
 * have to be WRITTEN before the unmap (so the object holds a page at that
 * offset), the range has to be big enough that the destination faults several
 * pages of it, and the second mapping has to land on the same addresses. MAP_FIXED
 * over the same span is how an allocator gets its alignment, and it is the case
 * that matters.
 *
 * Native this passes trivially: one address space, and the kernel zero-fills.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>

#define PG	4096UL
#define NPG	64			/* 256 KiB: several fault chunks */
#define LEN	(NPG * PG)
#define STAMP	0xa5

int main(void)
{
	unsigned char *p, *q;
	size_t i;
	int bad = 0;
	unsigned long first_bad = 0;

	/*
	 * A mapping, written through so the destination really holds pages for
	 * it. Touching every page matters: a page never written is a hole in
	 * the object, and a hole reads as zeros, which would let this test pass
	 * for the wrong reason.
	 */
	p = mmap(NULL, LEN, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return 2;
	}
	memset(p, STAMP, LEN);
	for (i = 0; i < LEN; i += PG)
		if (p[i] != STAMP) {
			fprintf(stderr, "mu1: the stamp did not stick at +%zu\n", i);
			return 2;
		}

	if (munmap(p, LEN) != 0) {
		perror("munmap");
		return 2;
	}

	/*
	 * The same addresses again. MAP_FIXED, because "the same addresses" is
	 * the whole question and letting the kernel choose would ask a
	 * different one. Over a range just unmapped this is not clobbering
	 * anything.
	 */
	q = mmap(p, LEN, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (q == MAP_FAILED) {
		perror("mmap fixed");
		return 2;
	}
	if (q != p) {
		fprintf(stderr, "mu1: MAP_FIXED moved %p -> %p; the test cannot "
			"ask its question\n", (void *)p, (void *)q);
		return 2;
	}

	for (i = 0; i < LEN; i++)
		if (q[i] != 0) {
			if (!bad)
				first_bad = (unsigned long)i;
			bad++;
		}

	if (bad) {
		fprintf(stderr, "mu1: FAIL -- %d byte(s) of freshly mapped "
			"anonymous memory were not zero (first at +%lu, value "
			"0x%02x; 0x%02x is this program's own stamp from before "
			"the munmap, so the memory it freed came back)\n",
			bad, first_bad, q[first_bad], STAMP);
		munmap(q, LEN);
		return 1;
	}
	munmap(q, LEN);
	printf("PASS\n");
	return 0;
}
