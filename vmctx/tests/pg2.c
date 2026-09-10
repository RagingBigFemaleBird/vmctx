/*
 * pg2 — both sides writing one page in one call.
 *
 * This is the case the whole argument about coherence turns on, and nothing in
 * the tree provokes it. One page is written from both ends at once:
 *
 *   the source writes the low half — a read(2) whose buffer is at offset 0, so
 *   the bytes are produced by the kernel on the machine serving the syscall;
 *   the guest writes the high half — a second thread stamping a counter at
 *   offset 2048, in guest execution, with no syscall of its own.
 *
 * Neither half may damage the other. Under a protocol that moves whole pages
 * that means the page must come back to the guest between the two, and under
 * one that reconstructs the write set by diffing it means the diff must not
 * carry the source's idea of the high half. Both claims are checked here rather
 * than argued about:
 *
 *   - every byte the source delivered must be the byte the pipe carried, so a
 *     lost or stale low half shows as a wrong payload;
 *   - the guest's counter must never go backwards, so a high half overwritten
 *     from the source shows as a stamp that regresses.
 *
 * The counter is written twice within its half so a partially updated page is
 * distinguishable from a merely old one.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>

#define ROUNDS  300
#define PAYLOAD 64

static char *page;
static volatile int stop;

/*
 * A sequence number, and the reason this test needs one.
 *
 * The stamper writes the same value to two places in the page and the reader
 * compares them. Those are four separate accesses by two threads, so the reader
 * can be preempted between its two loads while the stamper runs on -- and then
 * the halves differ by however far the stamper got, with nothing wrong anywhere.
 * The check used to allow a difference of 64 to cover that, which does not: the
 * gap is a scheduling quantum, not a small number. Measured **natively, with no
 * vmctx involved at all**: 6 of 200 runs reported a difference over 64, the
 * largest being 133020. The test failed itself about as often as it failed
 * under vmctx, and every "torn" it ever reported was suspect.
 *
 * So the reader takes a consistent snapshot instead of a tolerance. The stamper
 * makes seq odd before it writes and even after; the reader keeps a sample only
 * if seq was even and unchanged across both loads, and then the two halves must
 * be *exactly* equal. A preempted reader now discards its sample rather than
 * reporting a tear, and a real tear -- the two halves of one page disagreeing
 * while nobody was writing -- is the only thing left that can fail.
 */
static volatile unsigned long seq;

static void *stamper(void *p)
{
	unsigned long v = 0;

	(void)p;
	while (!stop) {
		v++;
		__atomic_store_n(&seq, seq + 1, __ATOMIC_RELEASE);	/* odd */
		*(volatile unsigned long *)(page + 2048) = v;
		*(volatile unsigned long *)(page + 3072) = v;
		__atomic_store_n(&seq, seq + 1, __ATOMIC_RELEASE);	/* even */
	}
	return NULL;
}

int main(void)
{
	pthread_t t;
	int fd[2];
	unsigned long last = 0;
	int i, regressed = 0, torn = 0, payload_bad = 0, sampled = 0;
	/*
	 * Split out from payload_bad, which counted both and so could not tell
	 * them apart. "297 of 300 bad payloads" reads like the page is being
	 * corrupted wholesale; it is also what a read(2) that returns the wrong
	 * count looks like, and those are different defects on different sides
	 * of the machine. The first bad round is reported in full because a
	 * count says how often and never what.
	 */
	int short_read = 0, wrong_bytes = 0, first_bad = -1;
	ssize_t first_n = 0;
	int first_off = -1;
	unsigned char first_want = 0, first_got = 0;

	page = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED || pipe(fd) < 0) {
		printf("pg2: setup failed\n");
		return 1;
	}
	madvise(page, 4096, MADV_NOHUGEPAGE);
	memset(page, 0, 4096);
	/*
	 * Which page, so a trace can be pointed at it. VMR_PGLOG takes an
	 * address and this is the only one that matters here -- without it the
	 * page has to be guessed at from whichever address the log mentions
	 * most, which is an inference where a fact is available.
	 */
	printf("pg2: the shared page is at %p\n", (void *)page);
	fflush(stdout);

	if (pthread_create(&t, NULL, stamper, NULL) != 0) {
		printf("pg2: pthread_create failed\n");
		return 1;
	}

	for (i = 0; i < ROUNDS; i++) {
		char want[PAYLOAD];
		unsigned long a, b;
		ssize_t n;

		memset(want, 'A' + (i % 26), sizeof(want));
		if (write(fd[1], want, sizeof(want)) != (ssize_t)sizeof(want))
			break;
		/* The source fills the low half of the page. */
		n = read(fd[0], page, sizeof(want));
		if (n != (ssize_t)sizeof(want)) {
			payload_bad++;
			short_read++;
			if (first_bad < 0) {
				first_bad = i;
				first_n = n;
			}
			continue;
		}
		if (memcmp(page, want, sizeof(want)) != 0) {
			payload_bad++;
			wrong_bytes++;
			if (first_bad < 0) {
				int k;

				first_bad = i;
				first_n = n;
				for (k = 0; k < (int)sizeof(want); k++)
					if (page[k] != want[k]) {
						first_off  = k;
						first_want = (unsigned char)want[k];
						first_got  = (unsigned char)page[k];
						break;
					}
			}
		}

		/* And the guest has been filling the high half all along. */
		{
			unsigned long s1;
			int tries, stable = 0;

			/* Retried until it holds still; see pg1.c. */
			for (tries = 0; tries < 10000 && !stable; tries++) {
				s1 = __atomic_load_n(&seq, __ATOMIC_ACQUIRE);
				a = *(volatile unsigned long *)(page + 2048);
				b = *(volatile unsigned long *)(page + 3072);
				stable = !(s1 & 1UL) &&
					 __atomic_load_n(&seq, __ATOMIC_ACQUIRE) == s1;
			}
			if (stable) {
				/* A stable snapshot: no tolerance is needed. */
				sampled++;
				if (a != b) {
					torn++;
					fprintf(stderr, "pg2: TORN round %d: "
						"a=%lu b=%lu\n", i, a, b);
				}
				if (a < last)
					regressed++;
				last = a;
			}
		}
	}
	stop = 1;
	pthread_join(t, NULL);

	printf("pg2: %d rounds (%d stable samples), stamp reached %lu, %d "
	       "regressed, %d torn, %d bad payloads\n", ROUNDS, sampled, last,
	       regressed, torn, payload_bad);
	if (payload_bad)
		printf("pg2: of those, %d short reads and %d wrong bytes; "
		       "first at round %d, read returned %zd, byte %d was "
		       "0x%02x wanted 0x%02x\n", short_read, wrong_bytes,
		       first_bad, first_n, first_off, first_got, first_want);
	printf("%s\n", (regressed == 0 && torn == 0 && payload_bad == 0 && sampled > 0 &&
			last > 0) ? "PASS" : "FAIL");
	return (regressed == 0 && torn == 0 && payload_bad == 0 && sampled > 0 && last > 0)
	       ? 0 : 1;
}
