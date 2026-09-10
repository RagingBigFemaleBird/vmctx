// SPDX-License-Identifier: GPL-2.0
/* A terminating thread's native robust-list walk must see its last stores. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>

static pthread_mutex_t *mutex;
static void *owner(void *unused)
{
	(void)unused;
	return (void *)(long)pthread_mutex_lock(mutex);
}

int main(void)
{
	pthread_mutexattr_t attr;
	pthread_t thread;
	void *result;
	mutex = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mutex == MAP_FAILED || pthread_mutexattr_init(&attr) ||
	    pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) ||
	    pthread_mutex_init(mutex, &attr) || pthread_mutexattr_destroy(&attr))
		return 2;
	for (unsigned i = 0; i < 32; i++) {
		if (pthread_create(&thread, NULL, owner, NULL) ||
		    pthread_join(thread, &result) || result)
			return 2;
		int status = pthread_mutex_trylock(mutex);
		if (status != EOWNERDEAD) {
			fprintf(stderr, "FAIL: robust owner exit round=%u status=%d expected=%d\n",
				i, status, EOWNERDEAD);
			return 1;
		}
		if (pthread_mutex_consistent(mutex) || pthread_mutex_unlock(mutex))
			return 2;
	}
	if (pthread_mutex_destroy(mutex) || munmap(mutex, 4096)) return 2;
	puts("PASS: 32 native robust owner deaths preserve mutex recovery");
	return 0;
}
