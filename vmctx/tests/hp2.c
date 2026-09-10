/*
 * hp2 — heap-metadata coherence under concurrent forwarded syscalls.
 *
 * netsurf on kernel #147 clears the dlopen boundary-page wall (tests/dl1) and
 * then dies in glibc with `malloc(): mismatching next->prev_size (unsorted)`
 * -- chunk metadata one machine wrote read back as another's, the
 * stale-page-over-live-memory class. That death fires deep inside malloc, far
 * from the damage, and names no page. This provokes the same condition in the
 * small and says WHERE.
 *
 * The pattern the browser has and the page tests do not: several threads doing
 * heavy malloc/free churn -- which is what writes glibc's chunk headers, moves
 * the arena, and consolidates -- WHILE each also hands heap buffers to
 * forwarded syscalls (a write(2) reads the buffer at the source; a pipe read
 * has the source WRITE into it). Every forwarded call that touches a heap page
 * makes that page change machines, and a churning allocator is writing the
 * chunk headers on the same pages at the same time. A lost store or a stale
 * serve then lands on a header, which is exactly what the browser hit.
 *
 * Two oracles, because the two failures leave different residues:
 *   - the payload check: each 8-byte word of an allocation holds
 *     (its own address ^ MAGIC); a word that reads back wrong is a lost or
 *     stale store, and its residue says which (0 = invented page, another
 *     addr^MAGIC = aliased page, else = stale bytes) -- hp1's diagnostic.
 *   - glibc itself: if a chunk HEADER is corrupted the churn aborts the
 *     program (malloc's own consistency checks). That is caught as a death,
 *     not a payload miss, and is the browser's exact signature.
 *
 * Sizes straddle page boundaries on purpose (a chunk header then lives on a
 * different page from its body, the boundary-page geometry) and vary so the
 * arena grows and consolidates rather than recycling one bin.
 *
 * WHAT IT REPRODUCES (2026-08-20, kernel #147). A single hp2 is CLEAN (the
 * one-machine coherence is sound). Under torture.sh's pool -- eight at once,
 * the pg3 stress condition -- it dies ~6/64, and the deaths name the browser's
 * corruption family: glibc's multi-arena growth does a large munmap (a VACATE
 * of ~18MB ending at the 0x7ffff0000000 arena base) and then faults on a page
 * inside that re-used range, which the source answers ABSENT ("the take found
 * nothing and the pagemap agrees") and the destination fills with fresh ZEROS
 * (DECLINED-BY one-page-written-into-the-object). Zeros over live arena memory
 * is exactly what turns a chunk header into `mismatching next->prev_size`. It
 * is the same stale/invented class as the died-242 and pg2 startup ABSENT-fill
 * residuals (NOTES 2.15-2.16, 6.3) -- hp2 is the fast, dense reproducer of it.
 * Run it pooled: `sudo CASE_OK='^PASS$' ./tests/torture.sh hp2 64 8`.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>

#define THREADS 4
#define ROUNDS  4000
#define LIVE    32		/* allocations kept live at once, per thread */
#define MAGIC   0x686032c0ffeeULL

static volatile int stop;
static unsigned long total_bad;

struct slot { unsigned char *p; size_t n; };

/* Fill an allocation so every word names its own address. */
static void paint(unsigned char *p, size_t n)
{
	size_t i;

	for (i = 0; i + 8 <= n; i += 8) {
		unsigned long w = (unsigned long)(p + i) ^ MAGIC;

		memcpy(p + i, &w, 8);
	}
}

/* Verify it, and classify any word that is wrong. Returns bad-word count. */
static int check(unsigned char *p, size_t n, int tid, int round)
{
	size_t i;
	int bad = 0;

	for (i = 0; i + 8 <= n; i += 8) {
		unsigned long want = (unsigned long)(p + i) ^ MAGIC;
		unsigned long got;

		memcpy(&got, p + i, 8);
		if (got == want)
			continue;
		if (__atomic_fetch_add(&total_bad, 1, __ATOMIC_RELAXED) < 8) {
			unsigned long other = got ^ MAGIC;
			const char *kind = got == 0 ? "INVENTED (zeros)" :
				(other & 7) == 0 ? "ALIASED (another page)" :
				"STALE (earlier bytes)";

			fprintf(stderr, "hp2 tid%d round%d: word at %p reads "
				"0x%lx, wanted 0x%lx -- %s%s%+ld\n",
				tid, round, (void *)(p + i), got, want, kind,
				got && (other & 7) == 0 ? ", delta " : "",
				got && (other & 7) == 0 ?
				(long)(other - (unsigned long)(p + i)) : 0);
		}
		bad++;
	}
	return bad;
}

static void *worker(void *arg)
{
	int tid = (int)(long)arg;
	struct slot live[LIVE];
	int devnull = open("/dev/null", O_WRONLY);
	int pfd[2], round;
	unsigned seed = (unsigned)(tid * 2654435761u + 1);

	memset(live, 0, sizeof(live));
	if (pipe(pfd) < 0)
		return NULL;

	for (round = 1; round <= ROUNDS && !stop; round++) {
		int k = (int)((seed = seed * 1103515245u + 12345u) % LIVE);
		size_t n;

		/* Free whatever occupied this slot, verifying it first. */
		if (live[k].p) {
			check(live[k].p, live[k].n, tid, round);
			free(live[k].p);
			live[k].p = NULL;
		}
		/* A size that straddles pages and varies across bins. */
		n = 48 + ((seed >> 8) % 8192);
		live[k].p = malloc(n);
		if (!live[k].p)
			continue;
		live[k].n = n;
		paint(live[k].p, n);

		/* Hand the buffer to forwarded syscalls: the source READS it
		 * (write to /dev/null) and WRITES into it (pipe round trip). */
		if (write(devnull, live[k].p, n < 256 ? n : 256) < 0)
			break;
		{
			unsigned long tag = ((unsigned long)live[k].p) ^ MAGIC;

			if (write(pfd[1], &tag, 8) == 8) {
				unsigned long back = 0;

				if (read(pfd[0], live[k].p, 8) == 8) {
					memcpy(&back, live[k].p, 8);
					if (back != tag &&
					    __atomic_fetch_add(&total_bad, 1,
						 __ATOMIC_RELAXED) < 8)
						fprintf(stderr, "hp2 tid%d "
							"round%d: the pipe wrote "
							"0x%lx into the heap but "
							"it read back 0x%lx\n",
							tid, round, tag, back);
					/* restore the painted word we clobbered */
					paint(live[k].p, live[k].n);
				}
			}
		}
	}

	/* Drain and verify everything still live. */
	for (round = 0; round < LIVE; round++)
		if (live[round].p) {
			check(live[round].p, live[round].n, tid, -1);
			free(live[round].p);
		}
	close(devnull);
	close(pfd[0]);
	close(pfd[1]);
	return NULL;
}

int main(void)
{
	pthread_t t[THREADS];
	int i;

	for (i = 0; i < THREADS; i++)
		if (pthread_create(&t[i], NULL, worker, (void *)(long)i) != 0) {
			printf("hp2: pthread_create failed\n");
			return 1;
		}
	for (i = 0; i < THREADS; i++)
		pthread_join(t[i], NULL);

	printf("hp2: %d threads x %d rounds, %lu bad word(s)\n",
	       THREADS, ROUNDS, total_bad);
	printf("%s\n", total_bad == 0 ? "PASS" : "FAIL");
	return total_bad == 0 ? 0 : 1;
}
