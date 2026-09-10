// SPDX-License-Identifier: GPL-2.0
/*
 * hx1 — a hand-over may not discard a write.
 *
 * This exists to FORCE a race the code names and no test had ever arranged.
 *
 * vmctx_ctl_takeobj() hands a page to the source under the folio lock: it locks
 * the folio, unmaps the page from every mm at once, copies it with every
 * would-be faulter parked in shmem_fault() on that lock, and then removes it
 * from the object. Its own comment names the window that survives:
 *
 *   "the folio lock has to be dropped before shmem_truncate_range(), which
 *    takes it itself. A context that was blocked in shmem_fault() can be woken
 *    in that gap, map the page and write it, and the truncate then discards
 *    the write."
 *
 * Instrumenting that window (folio_mapped() in the gap) read ZERO over 2124
 * hand-overs even with the gap widened to 2ms -- but that is not evidence,
 * because the race needs a context PARKED ON THAT FOLIO at that moment and no
 * existing test arranges one. An instrument never seen to fire proves nothing.
 * So this test's job is to make it fire.
 *
 * The arrangement, and every part of it is deliberate:
 *
 *   - ONE page. Every thread's data and the syscall's buffer live in it, so a
 *     hand-over of that page contends with everything at once. Separate pages
 *     is what sh2 and pg3 do, and it is why they never park anyone.
 *   - N writer threads, each owning its own 8-byte slot in that page, each
 *     storing an increasing value and reading it straight back. Separate slots,
 *     so nothing here is a data race: a slot can only change if the SYSTEM
 *     changed it. Reading back a value one behind is a discarded write.
 *   - one thread doing a forwarded read(2) into a slot of the SAME page, over
 *     and over. Every one of those makes the source want the page, which is an
 *     evacuation, which is a hand-over -- so the window opens thousands of
 *     times while N threads are faulting on the very page it is about.
 *
 * A writer that reads back anything other than what it just wrote has had a
 * store discarded by the machinery, and the value tells which: one behind is a
 * write lost to a hand-over, zero is a page invented.
 *
 * Native this passes trivially: one address space, one page, no hand-overs.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <stdint.h>
#include <fcntl.h>

#define PG		4096UL
#define NWRITERS	4
#define SLOT		8		/* bytes per writer, 8-byte aligned */
#define WRITER_OFF	64		/* slots start here */
#define SYSBUF_OFF	2048		/* the syscall's buffer, same page */
#define SYSBUF_LEN	64
/*
 * Forwarded syscalls the evacuator makes, which is what sets the length of the
 * run. 200000 was the first number and it was simply too many: at roughly half
 * a millisecond a forwarded call, with four writers faulting on the same page
 * throughout, the run outlived its limit and produced NO output at all in two
 * runs of four -- which is not a pass and not a fail, and it made an A/B
 * impossible to read. A test that does not finish measures nothing.
 */
#define ROUNDS		20000

static unsigned char *page;
static volatile int stop_now;
static volatile unsigned long lost[NWRITERS];
static volatile unsigned long lost_val[NWRITERS], lost_want[NWRITERS];
static volatile unsigned long rounds_done[NWRITERS];

static void *writer(void *arg)
{
	unsigned long id = (unsigned long)arg;
	volatile unsigned long *slot =
		(volatile unsigned long *)(page + WRITER_OFF + id * SLOT);
	unsigned long i;

	for (i = 1; !stop_now; i++) {
		unsigned long got;

		*slot = i;
		/*
		 * Read it straight back. No barrier and no lock on purpose:
		 * this slot belongs to this thread and nothing else in the
		 * program touches it, so on any correct machine the load that
		 * follows the store returns the store. Anything else was done
		 * to us.
		 */
		got = *slot;
		if (got != i) {
			lost_val[id] = got;
			lost_want[id] = i;
			lost[id]++;
			stop_now = 1;
			break;
		}
		rounds_done[id] = i;
	}
	return NULL;
}

/*
 * The evacuator: a forwarded read(2) whose destination buffer is in the page
 * the writers are hammering. Each call makes the source want that page.
 */
static void *evacuator(void *arg)
{
	int fd[2];
	char src[SYSBUF_LEN];
	unsigned long i;

	(void)arg;
	if (pipe(fd) != 0)
		return NULL;
	memset(src, 0x5a, sizeof(src));

	for (i = 0; i < ROUNDS && !stop_now; i++) {
		if (write(fd[1], src, SYSBUF_LEN) != SYSBUF_LEN)
			break;
		if (read(fd[0], page + SYSBUF_OFF, SYSBUF_LEN) != SYSBUF_LEN)
			break;
		/*
		 * Progress, on stderr, so a run that is killed still says how
		 * far it got. "No output at all" used to be the most common
		 * result of this test and it is not a verdict: it could not be
		 * told from a hang, from the harness limit expiring, or from a
		 * thread dying. Now the last line printed says which.
		 */
		if ((i % 100) == 0)
			fprintf(stderr, "hx1: evacuator at %lu/%lu\n",
				i, (unsigned long)ROUNDS);
	}
	fprintf(stderr, "hx1: evacuator finished %lu of %lu\n",
		i, (unsigned long)ROUNDS);
	close(fd[0]);
	close(fd[1]);
	return NULL;
}

int main(void)
{
	pthread_t w[NWRITERS], e;
	unsigned long i;
	int bad = 0;

	/*
	 * The verdict has to survive a kill. printf() to a pipe is block
	 * buffered, so a PASS written just before the harness stopped the run
	 * was simply lost -- which is why a passing run and a hung run looked
	 * identical. FAIL was always visible because it goes to stderr.
	 */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	page = mmap(NULL, PG, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED) {
		perror("mmap");
		return 2;
	}
	memset(page, 0, PG);		/* make it exist before anyone races */
	/* Named so an instrument (VMR_CLOBBER/VMHOME_CLOBBER) can be aimed at
	 * the one page this whole test is about. */
	fprintf(stderr, "hx1: page at %p\n", (void *)page);

	if (pthread_create(&e, NULL, evacuator, NULL) != 0) {
		perror("pthread_create evacuator");
		return 2;
	}
	for (i = 0; i < NWRITERS; i++)
		if (pthread_create(&w[i], NULL, writer, (void *)i) != 0) {
			perror("pthread_create writer");
			stop_now = 1;
			return 2;
		}

	/*
	 * The EVACUATOR sets the length of the run, and the writers hammer for
	 * exactly as long as it lasts. The first version had it the other way
	 * round -- writers counted to ROUNDS in a tight loop, finished in
	 * milliseconds, and main then stopped the evacuator -- so the whole run
	 * produced 87 hand-overs and asked almost nothing. The instrument caught
	 * that, which is the entire reason it exists.
	 */
	pthread_join(e, NULL);
	stop_now = 1;
	for (i = 0; i < NWRITERS; i++)
		pthread_join(w[i], NULL);

	for (i = 0; i < NWRITERS; i++) {
		if (lost[i]) {
			fprintf(stderr, "hx1: FAIL -- writer %lu stored %lu into "
				"its own slot and read back %lu at round %lu. "
				"Nothing else in this program touches that slot, "
				"so a hand-over of the page discarded the store%s\n",
				i, lost_want[i], lost_val[i], rounds_done[i] + 1,
				lost_val[i] == lost_want[i] - 1 ?
				" (exactly one behind: the page reverted to the "
				"copy the source was given)" :
				lost_val[i] == 0 ?
				" (zero: the page was invented)" : "");
			bad = 1;
		}
	}
	if (bad)
		return 1;
	printf("hx1: %d writers hammered one page through %lu forwarded "
	       "syscalls on it (writer 0 did %lu rounds)\n",
	       NWRITERS, (unsigned long)ROUNDS, rounds_done[0]);
	fprintf(stderr, "hx1: PASS\n");
	printf("PASS\n");
	return 0;
}
