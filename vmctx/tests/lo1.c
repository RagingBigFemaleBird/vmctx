// SPDX-License-Identifier: GPL-2.0
/*
 * lo1 — a page lent to a thread must outlive that thread.
 *
 * A loan is keyed by (address space, page) but its `owner` is a single shadow
 * pid, and the recall channel is looked up by that pid alone. So when the
 * shadow that happened to take a page exits, its channel goes and the page
 * becomes unrecallable -- while the source's mm is alive and every sibling
 * thread still maps it. The page is then alive at NEITHER site, and what the
 * program reads back is whatever the local kernel invents: zeros.
 *
 * That is what ws1 and netsurf both die of, on the same page (main's stack):
 *
 *   LOST: 0x7fffffffe000 was lent to shadow N (lent whole, its channel is gone)
 *   FATAL: A PAGE OF THE GUEST'S MEMORY IS ON NEITHER MACHINE
 *
 * and in netsurf the zeros come back as a freed tcache pointer, as a NULL
 * string in g_str_hash, and as -0x148 off a NULL base -- one page, three faces.
 *
 * This makes that sequence deliberate and small:
 *
 *   1. main writes a known pattern into a page it owns
 *   2. a THREAD makes a syscall that reads that page, which forwards to the
 *      source and pulls the page over to the thread's shadow -- the loan
 *   3. the thread exits, taking its shadow's channel with it
 *   4. main reads its own page back
 *
 * Step 4 must see the pattern. If the loan cannot be recalled once its shadow
 * is gone, main reads zeros instead and this prints which word went and what it
 * became -- a lost page named at the byte, rather than a browser dying of it
 * three abstraction layers later.
 *
 * The buffer is page-aligned and a whole page so the loan is unambiguous, and
 * the pattern is position-dependent so a partial or shifted copy is not
 * mistaken for a good one.
 *
 * AND IT IS ON MAIN'S STACK, which is the point. The same test over an mmap'd
 * page passes five times in five: an anonymous page of its own recalls without
 * trouble. Every observed failure is main's stack instead --
 *
 *   LOST: 0x7fffffffe000 was lent to shadow N (lent whole, its channel is gone)
 *
 * -- because that is the page the source touches on the thread's behalf while
 * setting the thread up, so it is lent to the NEW thread's shadow rather than
 * to a shadow of main's own. When that thread exits, the loan's owner is gone
 * and the page main is standing on cannot be recalled. Borrowing an unrelated
 * page does not reproduce that; borrowing main's own does.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdint.h>

#define PGSZ 4096
#define NW   (PGSZ / sizeof(unsigned long))

static unsigned long *page;	/* points into main's stack frame */
static int devnull;

static unsigned long pat(unsigned i) { return 0x101000000UL + i * 7919UL; }

/*
 * The thread's job is only to make the SOURCE touch the page, which is what
 * creates the loan. write(2) of the buffer does it: the call is forwarded, and
 * the source reads those bytes out of the program's own address space, so the
 * page moves to the source's side and is recorded as lent to THIS thread's
 * shadow. Then the thread exits.
 */
static void *borrow(void *arg)
{
	ssize_t n;

	(void)arg;
	n = write(devnull, page, PGSZ);
	if (n != PGSZ)
		fprintf(stderr, "lo1: write returned %zd\n", n);
	return NULL;
}

int main(void)
{
	pthread_t t;
	unsigned i, bad = 0, zero = 0;
	/*
	 * Two pages on main's stack so a whole aligned page of it can be lent,
	 * and so the frame this function is standing in is the thing at risk.
	 */
	unsigned char frame[2 * PGSZ];

	devnull = open("/dev/null", O_WRONLY);
	if (devnull < 0) { printf("lo1: open /dev/null failed\nFAIL\n"); return 1; }

	page = (unsigned long *)(((uintptr_t)frame + PGSZ - 1) & ~(uintptr_t)(PGSZ - 1));

	for (i = 0; i < NW; i++)
		page[i] = pat(i);

	if (pthread_create(&t, NULL, borrow, NULL) != 0) {
		printf("lo1: pthread_create failed\nFAIL\n");
		return 1;
	}
	pthread_join(t, NULL);
	/* The thread is gone, and with it the shadow that holds the loan. */

	for (i = 0; i < NW; i++) {
		if (page[i] == pat(i))
			continue;
		if (!bad++)
			printf("lo1: word %u reads 0x%lx, expected 0x%lx\n",
			       i, page[i], pat(i));
		if (page[i] == 0)
			zero++;
	}
	if (bad) {
		printf("lo1: %u of %zu words wrong (%u of them zero) -- the page "
		       "was lent to a thread that exited and could not be "
		       "recalled\nFAIL\n", bad, NW, zero);
		return 1;
	}
	printf("lo1: the page survived the thread that borrowed it\nPASS\n");
	return 0;
}
