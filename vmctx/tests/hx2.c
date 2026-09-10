// SPDX-License-Identifier: GPL-2.0
/*
 * hx2 -- a reliable shared-page hand-over lost-write detector.
 *
 * hx1 proved the race exists but catches it only 35-50% of runs and stops on
 * the first loss, so it cannot measure a fix. hx2 is the oracle for the
 * two-phase hand-over: it is WALL-CLOCK bounded (always finishes with a
 * verdict), applies more pressure (more writers, several evacuators, no
 * throttle), and COUNTS every lost write rather than stopping -- so a run
 * reports a rate, and a fix is confirmed when that rate is zero across many
 * runs while the count was reliably non-zero before.
 *
 * The arrangement is hx1's, amplified:
 *   - ONE page. Writer slots and every evacuator's syscall buffer live in it,
 *     so a hand-over of that page contends with all of them at once.
 *   - N writer threads, each owning its own 8-byte slot, storing an increasing
 *     value and reading it straight back. Separate slots: a slot can only
 *     change if the SYSTEM changed it, so a read one-behind is a discarded
 *     write and a zero is an invented page. On a mismatch it records and
 *     re-syncs rather than stopping, so one run measures the whole rate.
 *   - several evacuator threads, each a forwarded read(2) into its own region
 *     of the SAME page, looping flat out -- every call makes the source want
 *     the page, which is a hand-over, so the window opens as fast as the LAN
 *     round trip allows while the writers fault on the very page it is about.
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
#include <time.h>
#include <sys/syscall.h>

/*
 * 4 writers + 2 evacuators, not 8 + 3. Amplifying hx1's 4 + 1 further tips the
 * page out of the silent lost-write this measures and into a DIFFERENT failure:
 * eleven contexts all faulting one page during startup thrash the hand-over
 * until a take times out and the watchdog ends a context (exit 131). That is a
 * real coherence break too, but it is not the store-quietly-discarded case the
 * two-phase hand-over is for, and it kills the run before any count. One extra
 * evacuator over hx1 is enough to raise the hand-over rate; more is a crash.
 */
#define PG		4096UL
#define NWRITERS	4
#define NEVAC		2
#define SLOT		8
#define WRITER_OFF	64		/* slots 64..127 (8 writers x 8) */
#define SYSBUF0		256		/* evacuator buffers, same page */
#define SYSBUF_LEN	64
#define RUN_SECS	20		/* wall clock -- always finishes */

static unsigned char *page;
static volatile int stop_now;
static volatile unsigned long lost[NWRITERS];
static volatile unsigned long behind[NWRITERS], zero[NWRITERS], other[NWRITERS];
static volatile unsigned long rounds[NWRITERS];
static volatile unsigned long lost_last_val[NWRITERS], lost_last_want[NWRITERS];
/*
 * WHEN each writer's first and last loss happened, on CLOCK_MONOTONIC in
 * microseconds -- the same clock and unit vmremote's trail (abs=...) and
 * vmhome's print, both halves run on this machine. A loss is a value
 * installed over the page by the machinery; the trail says which install
 * happened at that instant, so the producer is read off the log rather than
 * inferred from the shape of the loss. Read after the loss is detected, so it
 * costs the race nothing.
 */
static volatile unsigned long lost_first_us[NWRITERS], lost_last_us[NWRITERS];

static unsigned long mono_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long)ts.tv_sec * 1000000ul +
	       (unsigned long)ts.tv_nsec / 1000;
}

static void *writer(void *arg)
{
	unsigned long id = (unsigned long)arg;
	volatile unsigned long *slot =
		(volatile unsigned long *)(page + WRITER_OFF + id * SLOT);
	unsigned long i = 1;

	while (!stop_now) {
		unsigned long got;

		*slot = i;
		got = *slot;			/* the store's own read-back */
		if (got != i) {
			unsigned long t = mono_us();

			lost[id]++;
			if (!lost_first_us[id])
				lost_first_us[id] = t;
			lost_last_us[id] = t;
			lost_last_val[id]  = got;
			lost_last_want[id] = i;
			/*
			 * Tell the machinery, at the instant: a getppid(2)
			 * carrying a marker and the facts. The destination's
			 * monitor sees every syscall before forwarding it and
			 * answers this one with a photograph of who maps what
			 * (vmremote's LOSS-PROBE); natively it is a getppid.
			 * After the loss, so it cannot perturb the race.
			 */
			syscall(SYS_getppid, 0x4c4f5353UL, (unsigned long)page,
				id, got, i);
			if (got == i - 1)      behind[id]++;
			else if (got == 0)     zero[id]++;
			else                   other[id]++;
			i = got;		/* re-sync and keep measuring */
		}
		rounds[id] = i++;
	}
	return NULL;
}

static void *evacuator(void *arg)
{
	unsigned long id = (unsigned long)arg;
	unsigned char *buf = page + SYSBUF0 + id * SYSBUF_LEN;
	char src[SYSBUF_LEN];
	int fd[2];

	if (pipe(fd) != 0)
		return NULL;
	memset(src, 0x5a, sizeof(src));
	while (!stop_now) {
		if (write(fd[1], src, SYSBUF_LEN) != SYSBUF_LEN)
			break;
		if (read(fd[0], buf, SYSBUF_LEN) != SYSBUF_LEN)
			break;
	}
	close(fd[0]);
	close(fd[1]);
	return NULL;
}

int main(void)
{
	pthread_t w[NWRITERS], e[NEVAC];
	unsigned long i, total = 0, tb = 0, tz = 0, to = 0;
	struct timespec ts = { RUN_SECS, 0 };

	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	page = mmap(NULL, PG, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED) {
		perror("mmap");
		return 2;
	}
	memset(page, 0, PG);

	/*
	 * Writers first, and a beat to let their slots settle, before the
	 * evacuators start pulling the page. Standing all N up at once while the
	 * page is still being handed in for the first time is the startup thrash
	 * that ends a context before the run begins.
	 */
	for (i = 0; i < NWRITERS; i++)
		if (pthread_create(&w[i], NULL, writer, (void *)i) != 0) {
			fprintf(stderr, "hx2: pthread_create writer %lu failed\n", i);
			stop_now = 1;
			return 2;
		}
	usleep(200000);
	for (i = 0; i < NEVAC; i++)
		if (pthread_create(&e[i], NULL, evacuator, (void *)i) != 0) {
			fprintf(stderr, "hx2: pthread_create evacuator %lu failed\n", i);
			return 2;
		}

	nanosleep(&ts, NULL);
	stop_now = 1;
	for (i = 0; i < NEVAC; i++)
		pthread_join(e[i], NULL);
	for (i = 0; i < NWRITERS; i++)
		pthread_join(w[i], NULL);

	for (i = 0; i < NWRITERS; i++) {
		total += lost[i]; tb += behind[i]; tz += zero[i]; to += other[i];
		if (lost[i])
			fprintf(stderr, "hx2: writer %lu lost %lu store(s) "
				"(%lu behind, %lu zero, %lu other); last: stored "
				"%lu read %lu; did %lu rounds; first loss at "
				"abs=%lu, last at abs=%lu\n",
				i, lost[i], behind[i], zero[i], other[i],
				lost_last_want[i], lost_last_val[i], rounds[i],
				lost_first_us[i], lost_last_us[i]);
	}
	fprintf(stderr, "hx2: %d writers + %d evacuators for %ds on one page; "
		"writer 0 did %lu rounds\n",
		NWRITERS, NEVAC, RUN_SECS, rounds[0]);
	if (total) {
		fprintf(stderr, "hx2: FAIL -- %lu store(s) discarded by the "
			"machinery (%lu one-behind, %lu invented, %lu other)\n",
			total, tb, tz, to);
		printf("FAIL %lu\n", total);
		return 1;
	}
	fprintf(stderr, "hx2: PASS -- no store discarded\n");
	printf("PASS\n");
	return 0;
}
