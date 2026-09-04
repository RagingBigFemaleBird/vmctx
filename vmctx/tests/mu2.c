// SPDX-License-Identifier: GPL-2.0
/*
 * mu2 — a partial unmap must take exactly what it was given.
 *
 * mu1 unmaps a whole range and proves the memory does not come back. This is
 * the shape mu1 does not reach, and it is the one every allocator on the
 * machine actually uses: map more than you need, unmap the unaligned HEAD and
 * the unaligned TAIL, keep the aligned middle, and set the permissions you
 * wanted on what survives. netsurf's last four forwarded calls before it dies
 * are exactly that, and then it writes through a null pointer:
 *
 *   mmap(0, 0x8000000)                    -> A
 *   munmap(A, 0xe00000)                      the head
 *   munmap(A + 0x4e00000, 0x3200000)         the tail
 *   mprotect(A + 0xe00000, 0x21000, PROT_READ|PROT_WRITE)
 *
 * Two things can go wrong here that a whole-range unmap cannot show.
 *
 * The first is the SURVIVOR. A partial unmap splits one VMA into two and frees
 * pages either side; the middle is untouched and must still hold what the
 * program put there. The source performs the munmap and forgets its records for
 * the range it was given -- pgown_forget_range(), relro_forget(),
 * sh_file_forget() -- and if any of them forgets more than the range, or if the
 * destination drops object pages by range rather than by the exact bytes, the
 * survivor loses memory nobody asked to free. That is a silent data loss in the
 * middle of a live allocation, which is what a corrupted pointer looks like
 * from the outside.
 *
 * The second is REUSE, as in mu1 but now against a range that was only part of
 * a mapping: the head comes back as new memory and must read as zero.
 *
 * So this checks both, in the allocator's order, and it checks the survivor
 * BEFORE and AFTER the reuse -- because a remap of the head that overshoots
 * into the middle would show up nowhere else.
 *
 * Every page is written before anything is unmapped, deliberately: a page never
 * written is a hole in the destination's backing object, a hole reads as zeros,
 * and a test that never made the pages exist would pass without asking anything.
 *
 * Native this passes trivially: one address space, and the kernel's own
 * bookkeeping does all of it.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>

#define PG	4096UL
#define HEAD	16			/* pages unmapped off the front */
#define MID	32			/* pages that must survive */
#define TAIL	16			/* pages unmapped off the end   */
#define NPG	(HEAD + MID + TAIL)
#define LEN	(NPG * PG)

/* A per-page value, so a page that comes from the wrong offset is visible. */
static unsigned char want(unsigned long pg)
{
	return (unsigned char)(0x40 + (pg & 0x3f));
}

static int check_mid(const unsigned char *mid, const char *when)
{
	unsigned long i;

	for (i = 0; i < MID; i++) {
		unsigned char got = mid[i * PG];
		unsigned char exp = want(HEAD + i);

		if (got != exp) {
			fprintf(stderr, "mu2: FAIL -- the surviving middle lost "
				"memory %s: page %lu of the survivor reads "
				"0x%02x, wrote 0x%02x. Nothing unmapped this "
				"page; a partial unmap freed more than it was "
				"given.\n", when, i, got, exp);
			return 1;
		}
	}
	return 0;
}

int main(void)
{
	unsigned char *p, *mid, *q;
	unsigned long i;
	int bad = 0;

	p = mmap(NULL, LEN, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return 2;
	}
	for (i = 0; i < NPG; i++)
		memset(p + i * PG, want(i), PG);
	mid = p + HEAD * PG;

	/* The head, then the tail: two partial unmaps of one mapping. */
	if (munmap(p, HEAD * PG) != 0) {
		perror("munmap head");
		return 2;
	}
	if (munmap(p + (HEAD + MID) * PG, TAIL * PG) != 0) {
		perror("munmap tail");
		return 2;
	}

	/*
	 * The permission change on what survives. It must not disturb the
	 * contents -- the same protection it already had, asked for again,
	 * which is what an allocator does to the aligned middle.
	 */
	if (mprotect(mid, MID * PG, PROT_READ | PROT_WRITE) != 0) {
		perror("mprotect middle");
		return 2;
	}

	if (check_mid(mid, "after the unmaps"))
		return 1;

	/*
	 * Now take the head back. New memory at addresses that were just
	 * freed: it must read as zero, and it must not have disturbed the
	 * survivor next to it.
	 */
	q = mmap(p, HEAD * PG, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (q == MAP_FAILED) {
		perror("mmap head again");
		return 2;
	}
	if (q != p) {
		fprintf(stderr, "mu2: MAP_FIXED moved %p -> %p; the test cannot "
			"ask its question\n", (void *)p, (void *)q);
		return 2;
	}
	for (i = 0; i < HEAD * PG; i++)
		if (q[i] != 0) {
			fprintf(stderr, "mu2: FAIL -- the head was unmapped and "
				"mapped again, so it is new memory, but +%lu "
				"reads 0x%02x (wrote 0x%02x there before the "
				"unmap); freed memory came back\n",
				i, q[i], want(i / PG));
			bad = 1;
			break;
		}

	if (check_mid(mid, "after the head was mapped again"))
		bad = 1;

	munmap(q, HEAD * PG);
	munmap(mid, MID * PG);
	if (bad)
		return 1;
	printf("PASS\n");
	return 0;
}
