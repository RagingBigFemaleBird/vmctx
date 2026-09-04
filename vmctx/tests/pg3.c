/*
 * pg3 — is the source ever shown a buffer it has already been shown?
 *
 * What is left after the recall window was closed is a *stale read*: a guest
 * thread fills a buffer, hands it to the source with a syscall, and the source
 * reads the contents from before that fill. th9 shows it as a line printed
 * twice, but it only samples the question forty times a run, so it takes a
 * hundred runs to see once.
 *
 * Here the guest checks the answer itself, thousands of times a run. The buffer
 * carries a counter that only ever increases; it is written to a pipe and read
 * straight back, so the bytes that come round are exactly the bytes the source
 * saw. Anything but the current counter is the failure, and it says which kind:
 *
 *   behind   the source read the buffer as it was before this round's fill —
 *            the stale read this test exists for;
 *   ahead    impossible for a monotonic counter, so it would mean the transport
 *            rather than the memory.
 *
 * Both threads do this at once, each with its own pipe and its own counter,
 * because that is the condition th9's own comment names: "two contexts making
 * forwarded syscalls at the same time". A sibling that merely spins is not
 * enough — 20000 rounds against one pass without a single stale read — so what
 * matters is not that a second context exists but that it is also inside a
 * syscall, with the source serving both.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define ROUNDS 20000

/*
 * Where the two buffers live, for arming VMHOME_PGLOG / VMR_PGLOG before a run.
 *
 * Measured rather than guessed: a print of `(void *)((unsigned long)buf &
 * ~0xfffUL)` at the top of each thread said A 0x7fffffffe000 (main's stack) and
 * B 0x7ffff7ff6000 (the created thread's). It is written here rather than left
 * in the code because this test is a race and printing inside the loop changes
 * it -- with the print in, the committed sources failed 6 runs of 6; the
 * addresses are stable for a static binary and cost nothing here.
 */

static volatile int stop;

/* One round-trip: fill a buffer, let the source read it, check what came back. */
/*
 * Where this thread's two buffers were, recorded once and printed after the
 * loop rather than inside it.
 *
 * The trace that finds this test's failure has to be armed with a page address
 * before the run, and the addresses were guessed at until now. Printing them
 * from inside the loop is not available: this is a race, and a run with one
 * extra write(2) per round failed 6 times in 6 where the same sources passed
 * 4 of 6 without it. Recording a pointer costs nothing and says the same thing.
 *
 * Both buffers matter and only one has ever been traced. They are 64 bytes each
 * in one frame, so a frame that straddles a page boundary puts the bytes the
 * guest writes and the bytes the source writes on two different pages -- and
 * "A is behind" is a statement about the second one.
 */
static unsigned long roundtrip(int rd, int wr, unsigned long rounds,
			       unsigned long *behind, unsigned long *ahead,
			       unsigned long *shortread, const char *who,
			       const void **wbuf, const void **rbuf)
{
	unsigned long i;

	for (i = 1; i <= rounds && !stop; i++) {
		char buf[64], got[64];
		int n;

		if (i == 1) {
			*wbuf = buf;
			*rbuf = got;
		}
		n = snprintf(buf, sizeof(buf), "%s%08lu-%08lu", who, i, i);
		if (write(wr, buf, (size_t)n) != n)
			break;
		if (read(rd, got, (size_t)n) != n) {
			(*shortread)++;
			continue;
		}
		got[n] = '\0';
		if (memcmp(got, buf, (size_t)n) != 0) {
			unsigned long a = 0, b = 0;

			if (sscanf(got + strlen(who), "%lu-%lu", &a, &b) == 2 &&
			    a < i)
				(*behind)++;
			else
				(*ahead)++;
			if (*behind + *ahead <= 4)
				printf("%s round %lu: wanted |%s| got |%s|\n",
				       who, i, buf, got);
		}
	}
	return i - 1;
}

struct side { int rd, wr; unsigned long behind, ahead, shortread; const char *who;
	      const void *buf, *got; };

static void *other(void *p)
{
	struct side *s = p;

	roundtrip(s->rd, s->wr, ROUNDS, &s->behind, &s->ahead, &s->shortread,
		  s->who, &s->buf, &s->got);
	return NULL;
}

int main(void)
{
	static struct side a = { .who = "A" }, b = { .who = "B" };
	unsigned long done;
	pthread_t t;
	int fa[2], fb[2];

	if (pipe(fa) < 0 || pipe(fb) < 0) {
		printf("pg3: pipe failed\n");
		return 1;
	}
	a.rd = fa[0]; a.wr = fa[1];
	b.rd = fb[0]; b.wr = fb[1];

	if (pthread_create(&t, NULL, other, &b) != 0) {
		printf("pg3: pthread_create failed\n");
		return 1;
	}
	done = roundtrip(a.rd, a.wr, ROUNDS, &a.behind, &a.ahead, &a.shortread,
			 a.who, &a.buf, &a.got);
	stop = 1;
	pthread_join(t, NULL);

	printf("pg3: A wrote at %p (page %p) and read at %p (page %p); "
	       "B wrote at %p (page %p) and read at %p (page %p)\n",
	       a.buf, (void *)((unsigned long)a.buf & ~0xfffUL),
	       a.got, (void *)((unsigned long)a.got & ~0xfffUL),
	       b.buf, (void *)((unsigned long)b.buf & ~0xfffUL),
	       b.got, (void *)((unsigned long)b.got & ~0xfffUL));
	printf("pg3: %lu rounds each, A %lu behind %lu ahead %lu short, "
	       "B %lu behind %lu ahead %lu short\n", done,
	       a.behind, a.ahead, a.shortread, b.behind, b.ahead, b.shortread);
	printf("%s\n", (a.behind + a.ahead + a.shortread +
			 b.behind + b.ahead + b.shortread) == 0 ? "PASS" : "FAIL");
	return (a.behind + a.ahead + a.shortread +
		b.behind + b.ahead + b.shortread) == 0 ? 0 : 1;
}
