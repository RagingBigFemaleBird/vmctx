// SPDX-License-Identifier: GPL-2.0
/*
 * mf2 -- a fork made BY A WORKER THREAD of a threaded process.
 *
 * mf1 forks from main while worker threads run. Real programs fork from the
 * workers: glib's thread pool, glycin's sandboxed image loader (the forks
 * netsurf makes), firefox's sandbox probe. The child then runs on the
 * FORKING THREAD's stack -- a page the parent's thread context holds on the
 * destination, not main's -- and the first thing it reads is that stack and
 * the heap the parent wrote before the fork.
 *
 * Photographed on netsurf (session 25): both children forked by worker
 * threads died within their first few instructions -- one on an instruction
 * fetch at 0x7c1d0 (an offset popped from a zero-filled stack), one killed --
 * after the destination answered ABSENT for the forking thread's stack/TCB
 * page and the source filled it with zeros as "fresh memory", while the child
 * forked by MAIN in the same run got its pages by copy-on-write break and ran.
 *
 * The child checks three things it inherited: a canary on the forking
 * thread's own stack (deep in a call chain, so the page is live), a canary in
 * heap memory the parent wrote just before the fork, and a value in a
 * sibling thread's stack frame published through a global pointer. Any of
 * them reading zero or wrong is the defect; the child reports through its
 * exit status and the parent prints the verdict.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdint.h>

#define NTHREADS	4
#define NFORKS		8
#define CANARY		0x5eed1234u
#define HEAPVAL		0x4ea9f00du

static volatile int stop;
static volatile unsigned *sibling_word;	/* a word on a spinning sibling's stack */
static unsigned *heap;

static unsigned burn(int depth, unsigned acc)
{
	volatile unsigned pad[64];
	unsigned i;

	for (i = 0; i < 64; i++)
		pad[i] = acc + i;
	if (depth > 0)
		acc = burn(depth - 1, acc + pad[depth & 63]);
	return acc + pad[0];
}

static void *spin(void *arg)
{
	unsigned acc = (unsigned)(uintptr_t)arg;
	volatile unsigned mine = CANARY;

	if ((uintptr_t)arg == 1)
		sibling_word = &mine;
	while (!stop)
		acc = burn(8, acc);
	return NULL;
}

/* The child's checks, deep in the forking thread's own call chain. */
static int child_check(const volatile unsigned *canary, int depth)
{
	if (depth > 0)
		return child_check(canary, depth - 1);
	if (*canary != CANARY)
		return 10;		/* own stack page wrong/zero */
	if (heap[0] != HEAPVAL || heap[511] != HEAPVAL)
		return 20;		/* heap page wrong/zero */
	if (sibling_word && *sibling_word != CANARY)
		return 30;		/* a sibling's stack page wrong/zero */
	return 0;
}

static int fork_from_here(int depth, int round)
{
	volatile unsigned canary = CANARY;
	pid_t kid;
	int st = 0;

	if (depth > 0)
		return fork_from_here(depth - 1, round);
	heap[0] = HEAPVAL;
	heap[511] = HEAPVAL;
	kid = fork();
	if (kid < 0)
		return 99;
	if (kid == 0) {
		int rc = child_check(&canary, 6);

		_exit(rc);
	}
	if (waitpid(kid, &st, 0) != kid)
		return 98;
	if (WIFSIGNALED(st)) {
		printf("mf2: round %d: child died by signal %d\n", round,
		       WTERMSIG(st));
		return 100 + WTERMSIG(st);
	}
	return WEXITSTATUS(st);
}

struct forker { int bad; int codes[NFORKS]; };

static void *forker(void *arg)
{
	struct forker *f = arg;
	int r;

	for (r = 0; r < NFORKS; r++) {
		f->codes[r] = fork_from_here(4, r);
		if (f->codes[r])
			f->bad++;
	}
	return NULL;
}

int main(void)
{
	pthread_t th[NTHREADS], ft;
	struct forker f;
	int i, made = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	heap = malloc(4096 * 2);
	if (!heap) {
		printf("FAIL no heap\n");
		return 2;
	}
	memset(heap, 0xa5, 4096 * 2);
	memset(&f, 0, sizeof(f));
	for (i = 0; i < NTHREADS; i++)
		if (pthread_create(&th[i], NULL, spin,
				   (void *)(uintptr_t)(i + 1)) == 0)
			made++;
	if (made != NTHREADS) {
		printf("FAIL could only start %d of %d threads\n", made, NTHREADS);
		return 2;
	}
	usleep(50000);
	/* The forks come from a WORKER thread, never from main. */
	if (pthread_create(&ft, NULL, forker, &f) != 0) {
		printf("FAIL could not start the forking thread\n");
		return 2;
	}
	pthread_join(ft, NULL);
	stop = 1;
	for (i = 0; i < NTHREADS; i++)
		pthread_join(th[i], NULL);
	printf("mf2: %d forks from a worker thread, %d bad; codes:", NFORKS, f.bad);
	for (i = 0; i < NFORKS; i++)
		printf(" %d", f.codes[i]);
	printf("\n%s\n", f.bad ? "FAIL" : "PASS");
	return f.bad ? 1 : 0;
}
