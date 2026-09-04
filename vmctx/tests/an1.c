// SPDX-License-Identifier: GPL-2.0
/*
 * an1 — a NAMED anonymous mapping is still anonymous, and a page of it that the
 * guest keeps writing must not go stale on the owner.
 *
 * A modern kernel lets a program tag an anonymous mapping with a name via
 * prctl(PR_SET_VMA_ANON_NAME); it shows in /proc/pid/maps as "[anon:NAME]"
 * instead of blank. jemalloc's arenas, glibc's and others use it.
 *
 * vmhome once classified a mapping by the shape of its name: any "[...]" that
 * was not "[heap]"/"[stack]" was read as a file, and a "[" name was then dropped
 * from the movable set, so a named anonymous mapping was served to the guest's
 * machine BY COPY rather than handed over. Two machines then held it at once,
 * and a page the guest kept writing drifted from the copy the owner kept -- so a
 * forwarded syscall that read the page in the owner's address space saw an old
 * value. That is how firefox died: its library search path lived in a
 * "[anon:jemalloc]" arena and a forwarded openat() read the stale bytes ->
 * ENOENT for a library that exists. The fix classifies a mapping by its inode
 * (0 == anonymous), not its name. The arm that proved it is gone with the proof
 * so both arms measure on one boot.
 *
 * The defect only bites when both sides hold the page at once, so this is a race
 * like pg1/pg3 rather than a straight line: a stamper thread writes an
 * ever-increasing counter into a NAMED anon page, and the main thread reads that
 * counter back THROUGH THE OWNER -- write(2) the page into a pipe (which reads
 * it in the owner's address space) and read(2) it back. If the owner is serving
 * a stale copy the counter the main thread sees stops advancing while the
 * stamper is still running; if the page is handed over properly it keeps up.
 *
 * The verdict: the counter read back through the owner must reach most of what
 * the stamper wrote and must not freeze. A frozen counter is the owner's stale
 * copy, exactly the firefox failure. Native, and with the fix, it tracks; under
 * the name bug it stalls. Reads are volatile so the compiler cannot fold the
 * loop away.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

#ifndef PR_SET_VMA
#define PR_SET_VMA 0x53564d41
#endif
#ifndef PR_SET_VMA_ANON_NAME
#define PR_SET_VMA_ANON_NAME 0
#endif

#define PGSZ   4096
#define ROUNDS 20000

static char *page;			/* the named anon page */
static volatile unsigned long *stamp;	/* counter at offset 0 */
static volatile int stop;

static void *stamper(void *p)
{
	unsigned long v = 0;

	(void)p;
	while (!stop) {
		v++;
		__atomic_store_n(stamp, v, __ATOMIC_RELEASE);
	}
	return NULL;
}

static int is_named(const void *addr)
{
	char line[512];
	FILE *f = fopen("/proc/self/maps", "r");
	unsigned long a = (unsigned long)addr;
	int named = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		unsigned long lo, hi;

		if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 &&
		    a >= lo && a < hi && strstr(line, "[anon:"))
			named = 1;
	}
	fclose(f);
	return named;
}

int main(void)
{
	pthread_t t;
	int pfd[2], i, named, frozen_run = 0, worst_frozen = 0;
	unsigned long last = 0, top = 0, stamper_top;

	page = mmap(NULL, PGSZ, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED) {
		printf("an1: mmap failed\nFAIL\n");
		return 1;
	}
	prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, (unsigned long)page, PGSZ,
	      (unsigned long)"vmctx-an1");
	madvise(page, PGSZ, MADV_NOHUGEPAGE);
	named = is_named(page);
	stamp = (volatile unsigned long *)page;
	*stamp = 0;

	if (pipe(pfd) != 0) {
		printf("an1: pipe failed\nFAIL\n");
		return 1;
	}
	if (pthread_create(&t, NULL, stamper, NULL) != 0) {
		printf("an1: pthread_create failed\nFAIL\n");
		return 1;
	}

	for (i = 0; i < ROUNDS; i++) {
		unsigned long got = 0;

		/*
		 * Read the counter back THROUGH THE OWNER: the write reads the
		 * page in the owner's address space and puts the first 8 bytes
		 * into the pipe; the read hands them back here.
		 */
		if (write(pfd[1], page, sizeof(got)) != (ssize_t)sizeof(got) ||
		    read(pfd[0], &got, sizeof(got)) != (ssize_t)sizeof(got)) {
			printf("an1: pipe round-trip failed\nFAIL\n");
			stop = 1;
			pthread_join(t, NULL);
			return 1;
		}
		if (got > top)
			top = got;
		if (got == last) {
			if (++frozen_run > worst_frozen)
				worst_frozen = frozen_run;
		} else {
			frozen_run = 0;
		}
		last = got;
	}
	stop = 1;
	pthread_join(t, NULL);
	stamper_top = *stamp;

	close(pfd[0]);
	close(pfd[1]);

	/*
	 * The counter the owner reported must have advanced with the stamper --
	 * within an order of magnitude, not frozen. A stale copy shows as `top`
	 * far below the stamper and a long frozen run.
	 */
	printf("an1: owner saw the counter reach %lu while the stamper reached "
	       "%lu; longest frozen run %d of %d (NAMED=%s)\n",
	       top, stamper_top, worst_frozen, ROUNDS, named ? "yes" : "no");
	if (top == 0 || worst_frozen > ROUNDS / 4 ||
	    (stamper_top > 1000 && top < stamper_top / 100)) {
		printf("an1: the owner served a stale copy of the named anon "
		       "page -- its counter did not keep up with the guest's "
		       "writes\nFAIL\n");
		return 1;
	}
	printf("PASS\n");
	return 0;
}
