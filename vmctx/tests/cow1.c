// SPDX-License-Identifier: GPL-2.0
/*
 * cow1 — fork is copy-on-write, and neither side may see the other's writes.
 *
 * The destination gives a forked child an object of its own, so parent and
 * child memory can only stay equal by being copied -- eagerly at the fork, or
 * one page at a time as sharing ends. Either way the observable rule is the
 * same and nothing tested it:
 *
 *   - the child must see the address space AS OF THE FORK, however much the
 *     parent writes afterwards;
 *   - the parent must never see the child's writes.
 *
 * A whole-object copy at the fork passes this trivially. A lazy scheme only
 * passes it if every way the parent's page can change or leave -- its own
 * store, a take by the other machine, a syscall result written into it, an
 * unmap -- hands the old page to the child first. That is what this is for.
 *
 * Both directions are checked at a page granularity across several pages, and
 * the pages are touched before the fork so they exist on both sides of it.
 * Exit status carries the verdict so the child's own failure is not lost.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <stdint.h>

#define PG	4096UL
#define NPG	8
#define A	0xA1
#define B	0xB2
#define C	0xC3

static int allbytes(const unsigned char *p, size_t n, unsigned char v)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (p[i] != v)
			return 0;
	return 1;
}

int main(void)
{
	unsigned char *m;
	unsigned p;
	pid_t kid;
	int st = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	m = mmap(NULL, PG * NPG, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m == MAP_FAILED) {
		printf("FAIL mmap\n");
		return 2;
	}
	/* Touch every page before the fork so both sides start from real memory. */
	memset(m, A, PG * NPG);

	kid = fork();
	if (kid < 0) {
		printf("FAIL fork\n");
		return 2;
	}
	if (kid == 0) {
		/*
		 * Give the parent time to overwrite everything. Its writes must
		 * not reach here: this is the whole test.
		 */
		usleep(300000);
		for (p = 0; p < NPG; p++) {
			if (!allbytes(m + p * PG, PG, A)) {
				printf("FAIL child saw the parent's write on "
				       "page %u (0x%02x, wanted 0x%02x)\n",
				       p, m[p * PG], A);
				_exit(3);
			}
		}
		/* The child's own writes, which the parent must never see. */
		memset(m, C, PG * NPG);
		for (p = 0; p < NPG; p++)
			if (!allbytes(m + p * PG, PG, C)) {
				printf("FAIL child could not keep its own "
				       "write on page %u\n", p);
				_exit(4);
			}
		_exit(0);
	}

	/* The parent overwrites everything while the child is still asleep. */
	memset(m, B, PG * NPG);
	if (waitpid(kid, &st, 0) != kid) {
		printf("FAIL waitpid\n");
		return 2;
	}
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
		printf("FAIL child exited %d (3 = it saw the parent's writes, "
		       "4 = it lost its own)\n",
		       WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		return 1;
	}
	for (p = 0; p < NPG; p++)
		if (!allbytes(m + p * PG, PG, B)) {
			printf("FAIL parent's page %u reads 0x%02x, wanted "
			       "0x%02x%s\n", p, m[p * PG], B,
			       m[p * PG] == C ? " -- the child's write reached "
					        "the parent" : "");
			return 1;
		}
	printf("PASS\n");
	return 0;
}
