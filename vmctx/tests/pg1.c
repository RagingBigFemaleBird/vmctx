/*
 * pg1 — one page, two guest threads, and the source holding it.
 *
 * th9 and its relatives never exercise the case the coherence protocol is
 * actually about: each of their threads writes a buffer on its own stack, so no
 * page is ever wanted by two parties at once. Here both threads use the *same*
 * page, and one of them repeatedly hands it to the source.
 *
 *   the stamper   writes an increasing counter into the page and nothing else.
 *                 It makes no syscall, so it is pure guest execution.
 *   the shower    calls write(2) with a pointer into that same page, which
 *                 makes the source read it — so the source takes the page while
 *                 the stamper is still running.
 *
 * The stamp only ever increases. So every value the source is shown must be at
 * least the previous one: a stamp that goes *backwards* is the source reading a
 * copy of the page from before the stamper's last write, which is precisely the
 * failure this test exists to catch. A stamp that stops increasing for a long
 * run is the other half of it — the source holding a page the guest can no
 * longer reach.
 *
 * Nothing here is timing-dependent for correctness: whatever the interleaving,
 * a monotonic counter read through a coherent page is monotonic.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>

#define ROUNDS 400

/* One page, shared by both threads, page-aligned so "the page" is unambiguous. */
static volatile unsigned long *stamp;	/* at offset 0 */
static char *shared_page;
static volatile int stop;

/*
 * A sequence number, for the reason spelled out in pg2.c: the two values below
 * are written by one thread and read by another, so a reader preempted between
 * its two loads sees them differ by however far the stamper got. That is not a
 * torn page, and allowing a fixed difference of 64 did not cover it -- the gap
 * is a scheduling quantum. Measured **natively, no vmctx anywhere**: 6 of 200
 * runs exceeded 64, the largest gap 50369.
 *
 * The reader keeps a sample only when seq was even and unchanged across both
 * loads, and then requires the two to be exactly equal.
 */
static volatile unsigned long seq;

static void *stamper(void *p)
{
	unsigned long v = 0;

	(void)p;
	while (!stop) {
		/*
		 * Write the counter twice, at two offsets of the same page, so
		 * a page that arrives half-updated is visible as the two copies
		 * disagreeing rather than as a value that merely looks odd.
		 */
		v++;
		__atomic_store_n(&seq, seq + 1, __ATOMIC_RELEASE);	/* odd */
		stamp[0] = v;
		*(volatile unsigned long *)(shared_page + 2048) = v;
		__atomic_store_n(&seq, seq + 1, __ATOMIC_RELEASE);	/* even */
	}
	return NULL;
}

int main(void)
{
	pthread_t t;
	unsigned long last = 0;
	int i, backwards = 0, torn = 0, sampled = 0, stable, tries;
	unsigned long s1;

	shared_page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (shared_page == MAP_FAILED) {
		printf("pg1: mmap failed\n");
		return 1;
	}
	madvise(shared_page, 4096, MADV_NOHUGEPAGE);
	stamp = (volatile unsigned long *)shared_page;
	stamp[0] = 0;

	if (pthread_create(&t, NULL, stamper, NULL) != 0) {
		printf("pg1: pthread_create failed\n");
		return 1;
	}

	for (i = 0; i < ROUNDS; i++) {
		char line[64];
		unsigned long a, b;
		int n;

		/*
		 * The snapshot is taken tightly -- two loads and nothing else
		 * between the seq reads. The formatting and the write(2) come
		 * after: a syscall inside the window makes the sequence almost
		 * never hold still, and the first version of this took 25
		 * usable samples out of 400 rounds.
		 */
		/*
		 * Retried until the snapshot holds still, which is what a
		 * seqlock reader does. Discarding the round instead was wrong
		 * here for a reason worth keeping: under vmctx the page moves
		 * on nearly every round, so one of these two loads usually
		 * faults and the stamper runs while it is serviced. Taking a
		 * single shot gave **4 usable samples out of 400** and the test
		 * failed every run for want of data rather than for tearing.
		 */
		stable = 0;
		for (tries = 0; tries < 10000 && !stable; tries++) {
			s1 = __atomic_load_n(&seq, __ATOMIC_ACQUIRE);
			a = stamp[0];
			b = *(volatile unsigned long *)(shared_page + 2048);
			stable = !(s1 & 1UL) &&
				 __atomic_load_n(&seq, __ATOMIC_ACQUIRE) == s1;
		}

		/*
		 * Hand the page to the source: write(2) makes it read these
		 * bytes, which is what takes the page across.
		 */
		n = snprintf(line, sizeof(line), "%lu %lu\n", a, b);
		if (write(1, line, (size_t)n) < 0)
			break;

		if (stable) {
			sampled++;
			if (a < last)
				backwards++;
			if (a != b) {
				torn++;
				fprintf(stderr, "pg1: TORN round %d: a=%lu b=%lu\n",
					i, a, b);
			}
			last = a;
		}
	}
	stop = 1;
	pthread_join(t, NULL);

	printf("pg1: %d rounds (%d stable samples), stamp reached %lu, %d went "
	       "backwards, %d torn\n", ROUNDS, sampled, last, backwards, torn);
	printf("%s\n", (backwards == 0 && torn == 0 && sampled > 0 && last > 0) ? "PASS" : "FAIL");
	return (backwards == 0 && torn == 0 && sampled > 0 && last > 0) ? 0 : 1;
}
