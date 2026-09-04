// SPDX-License-Identifier: GPL-2.0
/*
 * mf1 — a fork from a process that already has threads.
 *
 * The suite forks (ex5, pf3) and it threads (th9, th12), and ex4 does both --
 * but in the order that is easy: it forks FIRST, from a single-threaded main,
 * and creates its threads in the child. Nothing in it forks a process that is
 * ALREADY threaded, which is the order every real program uses and the one with
 * the hard rule attached: only the calling thread exists in the child. Every
 * other thread's stack is still mapped there, still holds whatever it held, and
 * no longer has anything running on it.
 *
 * That is a coherence question and not a scheduling one. Each thread here is a
 * context of its own, so at the moment of the fork the address space's pages are
 * spread across several of them -- and the child inherits the address space
 * while inheriting exactly one of the contexts. A page the FORKING thread needs,
 * held at that instant by a SIBLING, has to reach the child anyway.
 *
 * Why this shape: firefox's sandbox forks from a threaded process to ask whether
 * it may create a user namespace, and its child dies before its first
 * instruction -- an instruction fetch (err 0x14) at an address it popped off its
 * own stack, byte-identical across runs. Three explanations were built and
 * measured and all three were wrong: the child's object copy being stale, the
 * owner's pages not being settled before the fork, and the guest's pages not
 * being written into the object before it was copied (that one settles 492 pages
 * and changes nothing). What none of them tested is the order this test exists
 * for.
 *
 * The threads are kept genuinely busy on their own stacks, and deep in a call
 * chain, so that at the instant of the fork the sibling stacks are live and
 * recently written rather than parked at a single frame.
 *
 * The verdict is the child's ability to RUN, which is what firefox's child
 * fails: it returns through a call chain of its own, checks a canary written on
 * its stack before the fork, and reports. A child that dies by signal, returns
 * the wrong canary, or never prints is a failure; the parent says which. PASS
 * is printed only by the parent, and only when the child exited 0.
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
#define CANARY		0x5eed1234u

static volatile int stop;

/*
 * Deep enough that the thread's stack spans more than one page and is written
 * on every pass, so the sibling stacks are live memory at the fork rather than
 * a single cold frame.
 */
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

	while (!stop)
		acc = burn(8, acc);
	return NULL;
}

/*
 * The child's work, behind a call chain, so that returning from it exercises
 * the return addresses on the stack the child inherited -- the thing firefox's
 * child gets wrong.
 */
static unsigned child_check(const volatile unsigned *canary, int depth)
{
	if (depth > 0)
		return child_check(canary, depth - 1);
	return *canary;
}

int main(void)
{
	pthread_t th[NTHREADS];
	volatile unsigned canary = CANARY;
	int i, st = 0, made = 0;
	pid_t kid;

	for (i = 0; i < NTHREADS; i++)
		if (pthread_create(&th[i], NULL, spin,
				   (void *)(uintptr_t)(i + 1)) == 0)
			made++;
	if (made != NTHREADS) {
		printf("FAIL could only start %d of %d threads\n", made,
		       NTHREADS);
		return 2;
	}
	/* Let them get into the call chain and dirty their stacks. */
	usleep(50000);

	kid = fork();
	if (kid < 0) {
		printf("FAIL fork: %s\n", strerror(errno));
		return 2;
	}
	if (kid == 0) {
		/*
		 * The child. Exactly one thread exists here; the siblings'
		 * stacks are mapped and abandoned. Everything below runs on the
		 * stack this thread had at the fork.
		 */
		unsigned got = child_check(&canary, 12);

		if (got != CANARY)
			_exit(3);
		if (burn(6, 1) == 0)	/* touch a fresh stack range too */
			_exit(4);
		_exit(0);
	}

	if (waitpid(kid, &st, 0) != kid) {
		printf("FAIL waitpid\n");
		stop = 1;
		return 2;
	}
	stop = 1;
	for (i = 0; i < NTHREADS; i++)
		pthread_join(th[i], NULL);

	if (WIFSIGNALED(st)) {
		printf("FAIL child died by signal %d -- it could not run the "
		       "stack it inherited from a threaded parent\n",
		       WTERMSIG(st));
		return 1;
	}
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
		printf("FAIL child exited %d (3 = wrong canary on its own "
		       "stack, 4 = could not use a fresh stack range)\n",
		       WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
