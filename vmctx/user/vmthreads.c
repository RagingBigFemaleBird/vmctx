// SPDX-License-Identifier: GPL-2.0
/*
 * vmthreads — a threaded program, for running inside a VM context.
 *
 * Threads were the last thing the context refused. They turn out to need no
 * mechanism of their own: a thread is a task sharing an address space, which is
 * exactly what the guest's CR3 already expresses, and the kernel's copy_thread()
 * hands the child the right registers (RAX 0, RSP = the new thread's stack).
 * The VM context is copied to it like any other clone.
 *
 * Each thread bumps a shared counter under a mutex and prints from inside the
 * guest, so a correct run shows every thread making progress and a final total
 * that matches — i.e. the threads really ran, really shared memory, and really
 * synchronised.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define NTHREADS 4
#define NITER    2000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile unsigned long shared_counter;
static unsigned long per_thread[NTHREADS];

static void *worker(void *arg)
{
	long id = (long)arg;

	for (int i = 0; i < NITER; i++) {
		pthread_mutex_lock(&lock);
		shared_counter++;
		per_thread[id]++;
		pthread_mutex_unlock(&lock);
	}
	printf("  thread %ld finished, its own count = %lu\n", id, per_thread[id]);
	fflush(stdout);
	return NULL;
}

int main(void)
{
	pthread_t t[NTHREADS];
	unsigned long total = 0;

	printf("vmthreads: starting %d threads x %d increments\n",
	       NTHREADS, NITER);
	fflush(stdout);

	for (long i = 0; i < NTHREADS; i++) {
		if (pthread_create(&t[i], NULL, worker, (void *)i) != 0) {
			perror("pthread_create");
			return 1;
		}
	}
	for (int i = 0; i < NTHREADS; i++)
		pthread_join(t[i], NULL);

	for (int i = 0; i < NTHREADS; i++)
		total += per_thread[i];

	printf("vmthreads: shared counter = %lu, sum of per-thread = %lu, expected %d\n",
	       shared_counter, total, NTHREADS * NITER);
	if (shared_counter == (unsigned long)(NTHREADS * NITER) &&
	    total == shared_counter) {
		printf("vmthreads: PASS\n");
		return 0;
	}
	printf("vmthreads: FAIL\n");
	return 1;
}
