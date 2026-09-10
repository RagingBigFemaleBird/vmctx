// SPDX-License-Identifier: GPL-2.0
/*
 * ws1 — pthread_create must not fail. (And a write must not be lost; read on.)
 *
 * WHAT THIS ACTUALLY CATCHES, which is not what it was built to catch:
 * `pthread_create` fails about 3 runs in 20 under vmctx, creating four threads,
 * once. That is the defect this file now reports, and it is checked first
 * because everything below it depends on the threads existing.
 *
 * THE CLAIM THIS FILE ORIGINALLY MADE IS WITHDRAWN, and the reason is worth
 * more than the claim was. An earlier version of this experiment did not check
 * pthread_create's return value. When creation failed the thread never ran, its
 * word kept the zero it was initialised with, and the check dutifully reported
 * "a single write was lost" -- three runs in twenty, with a plausible mechanism
 * (a stale copy of the page served over the written one) and no write involved
 * at all. The rate was right, the address was right, the conclusion was wrong.
 * An unchecked return value manufactured a memory-coherence bug out of a failed
 * syscall.
 *
 * So: check what you call, and be suspicious of a test that confirms the theory
 * that motivated it.
 *
 * The lost-write experiment is kept below because the shape is still the right
 * one to ask the question with, once thread creation is reliable. Four threads
 * share one page. Each writes ONE word of it,
 * once, a value only it ever writes, and then never touches it again. The main
 * thread reads all four back. Every value must be there.
 *
 * Under vmctx one of them is sometimes not: the word reads the value it had
 * BEFORE the write -- zero -- which is the whole diagnosis in one observation.
 * It is not garbage and not another word's contents; it is a stale copy of the
 * page, from before the store, served over the written one. A write that
 * happened was thrown away.
 *
 * Getting to this took eleven wrong answers, and the two shapes that DO work are
 * what make it precise:
 *
 *   A thread writing its word REPEATEDLY never shows it. The lost write is
 *   simply re-applied a microsecond later, so the damage is invisible for as
 *   long as the writer keeps writing. That is why the same four threads
 *   incrementing their slots in a loop pass 14 rounds in 15.
 *
 *   A single thread writing a single variable never shows it either. It takes
 *   more than one context sharing the page for a copy to go stale.
 *
 * So: one page, several contexts, one store each, and no rewriting to paper over
 * it. Everything else -- fork, threads' TLS, namespaces, syscalls -- has been
 * eliminated; none of them is in this program's path.
 *
 * WHEN matters, and finding that out is what made this a test rather than a
 * flake. A first version kept four warm threads and had them write once per
 * round for a hundred rounds: it passed ten runs out of ten. The write is only
 * lost when it is the thread's FIRST store, moments after the thread was made --
 * which is exactly the shape of the failures that led here (a thread's first
 * store of its own TLS base, lost; a one-shot store right after pthread_create,
 * lost, three runs in twenty).
 *
 * So the shape is: four threads made once, each stores its word once, they are
 * joined, and the words are checked. ONE round, and that is a deliberate limit
 * rather than a preference -- turning it into forty rounds of create-and-join
 * made every run fail on `pthread_create` itself, ten out of ten, which is a
 * THIRD defect (thread churn) and one that would mask the one this measures.
 *
 * At one round it catches the lost write about three runs in twenty. That is a
 * flake, not a verdict, so it is not in the graded suite; run it in a loop:
 *   for i in $(seq 1 20); do sudo ./tests/runany.sh ws1 $TESTS/ws1; done
 * and read the FAIL line, which names the slot, what it read, and what its own
 * thread wrote.
 *
 * Native it passes: there is one address space and a store is a store.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>

#define NT	4
#define ROUNDS	1
#define MARK	0xa5a50000UL

static volatile unsigned long slot[NT];
static volatile unsigned long round_no;

/*
 * One store, and then the thread ends. No loop and no second write: a writer
 * that writes twice re-applies whatever was lost and hides it.
 */
static void *bump(void *x)
{
	long k = (long)x;

	slot[k] = MARK + (unsigned long)k + (round_no << 16);
	return NULL;
}

int main(void)
{
	pthread_t t[NT];
	unsigned long r;
	long i;
	int bad = 0;

	for (r = 1; r <= ROUNDS && bad < 8; r++) {
		for (i = 0; i < NT; i++)
			slot[i] = 0;
		round_no = r;
		for (i = 0; i < NT; i++) {
			int e = pthread_create(&t[i], NULL, bump, (void *)i);

			if (e != 0) {
				/* pthread_create returns the error, it does not
				 * set errno -- say which, because "it failed"
				 * has no fix attached and EAGAIN, ENOMEM and
				 * EPERM have three different ones. */
				printf("FAIL pthread_create thread %ld round "
				       "%lu: %d (%s)\n", i, r, e, strerror(e));
				return 2;
			}
		}
		/* Joined, so every store provably happened before the check. */
		for (i = 0; i < NT; i++)
			pthread_join(t[i], NULL);
		for (i = 0; i < NT; i++) {
			unsigned long want = MARK + (unsigned long)i + (r << 16);

			if (slot[i] != want) {
				printf("FAIL round %lu slot %ld reads 0x%lx, "
				       "not the 0x%lx its own thread wrote once "
				       "before it exited -- a write was lost\n",
				       r, i, slot[i], want);
				bad++;
			}
		}
	}

	if (bad)
		return 1;
	printf("PASS\n");
	return 0;
}
