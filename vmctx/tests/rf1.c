// SPDX-License-Identifier: GPL-2.0
/*
 * rf1 — a page whose current copy is on this side must not be refilled.
 *
 * The destination fetches a chunk of pages around a fault, not one page, because
 * programs touch memory in runs. That chunk covers neighbours the guest has
 * already written, and writing home's copy over them silently undoes the
 * guest's own stores. What used to stop it was a *lie*: the destination
 * recorded a shared loan to the source for pages the source had just handed
 * over, and the fetch paths skipped any page with a recorded holder. Dropping
 * the lie without putting the true statement somewhere -- "this side's copy is
 * the current one" -- is what made every earlier attempt measure worse.
 *
 * This is that statement, as a test. It is deliberately single-threaded and
 * deterministic: th9 sees the same defect but only sometimes, through forty
 * threads, and a case that fails a third of the time cannot tell anyone whether
 * a change helped.
 *
 *	1. map one chunk's worth of pages and write a first value into each;
 *	2. hand the whole region to the source, by passing it to a syscall the
 *	   source performs -- write(2) to /dev/null reads every byte of it in
 *	   the source's address space, so the source ends up holding a copy.
 *	   Without this step the pages are only ever present here, the chunk
 *	   fetch skips them, and no refill is even attempted: the first version
 *	   of this test passed against the defect it was written for, on both
 *	   arms, because of exactly that;
 *	3. overwrite every page with a second value. Now this side's copy is
 *	   the current one and the source's is stale -- the whole situation the
 *	   ownership record exists to describe;
 *	4. throw one page away with madvise(MADV_DONTNEED) and touch it, so it
 *	   faults and the fault fetches a chunk covering every neighbour;
 *	5. read them all back. They must hold the SECOND value.
 *
 * The verdict is the neighbours, and only the neighbours: every page except the
 * discarded one must still hold the second value. Reading back the first value
 * is the defect exactly -- the source's stale copy written over the guest's. What the victim reads
 * back is reported but not judged -- MADV_DONTNEED's zero is a promise about
 * the *guest's own* kernel, and which machine that is depends on how the run is
 * wired, so making it a pass condition would test the harness rather than the
 * defect. (Measured: it does not read back zero even natively here.)
 *
 * That split is what makes a failure legible: the page that was *asked* for is
 * fine and its neighbours are the casualties.
 *
 * Every access is volatile, and that is not decoration: without it the compiler
 * keeps step 1's store in a register and step 4 reads the register back. The
 * test then passes on a machine where the page was never reloaded at all, which
 * is the one result it must never give. It showed itself immediately -- the
 * discarded page "read" its old value straight through MADV_DONTNEED.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#define PGSZ  4096
#define PAGES 16		/* one FAULT_CHUNK; the blast radius of one fault */
#define VICTIM 7		/* somewhere in the middle, so it has neighbours
				 * on both sides */

/*
 * Distinct per page and per round, never zero, so "wrong", "blank" and "the
 * value the source still has" are three different answers.
 */
static unsigned long magic(int round, int i)
{
	return 0x00d1f1ed00000000UL + (unsigned long)round * 0x0100000000UL +
	       (unsigned long)(i + 1) * 0x1111;
}

int main(void)
{
	unsigned char *m;
	int i, bad = 0, blanked = 0;

	m = mmap(NULL, (size_t)PAGES * PGSZ, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m == MAP_FAILED) {
		printf("rf1: mmap failed\nFAIL\n");
		return 1;
	}

	for (i = 0; i < PAGES; i++)
		*(volatile unsigned long *)(m + (size_t)i * PGSZ) = magic(1, i);

	/*
	 * The source reads the whole region, in its own address space, and so
	 * comes to hold a copy of it. A syscall is the only way to make that
	 * happen from in here, and write(2) to /dev/null is the cheapest one
	 * that touches every byte.
	 */
	{
		int fd = open("/dev/null", O_WRONLY);

		if (fd < 0 || write(fd, m, (size_t)PAGES * PGSZ) !=
		    (ssize_t)((size_t)PAGES * PGSZ)) {
			printf("rf1: could not hand the region to the source\nFAIL\n");
			return 1;
		}
		close(fd);
	}

	/* Now this side's copy is the current one and the source's is stale. */
	for (i = 0; i < PAGES; i++)
		*(volatile unsigned long *)(m + (size_t)i * PGSZ) = magic(2, i);

	/* Genuinely absent, and only this one. */
	if (madvise(m + (size_t)VICTIM * PGSZ, PGSZ, MADV_DONTNEED) != 0) {
		printf("rf1: madvise failed\nFAIL\n");
		return 1;
	}

	/* The fault. Everything around it is what this test is about. */
	if (*(volatile unsigned long *)(m + (size_t)VICTIM * PGSZ) != 0)
		blanked = 1;

	for (i = 0; i < PAGES; i++) {
		unsigned long got = *(volatile unsigned long *)(m + (size_t)i * PGSZ);

		if (i == VICTIM)
			continue;
		if (got != magic(2, i)) {
			if (bad < 4)
				printf("rf1: page %d holds 0x%lx, wrote 0x%lx%s\n",
				       i, got, magic(2, i),
				       got == magic(1, i) ?
					 "  (the source's stale copy, refilled "
					 "over the guest's write)" :
				       got ? "" : "  (refilled blank)");
			bad++;
		}
	}

	printf("rf1: %d of %d neighbour(s) lost their contents (the discarded "
	       "page read back %s, which is not judged)\n", bad, PAGES - 1,
	       blanked ? "non-zero" : "zero");
	if (bad) {
		printf("FAIL\n");
		return 1;
	}
	printf("PASS\n");
	return 0;
}
