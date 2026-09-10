// SPDX-License-Identifier: GPL-2.0
/* Reusing a completed thread's stack must initialize its native thread state
 * before it executes. An uncontended rwlock must never report self-deadlock. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef TEST_DETACHED
#define TEST_DETACHED 0
#endif

static pthread_rwlock_t lock = PTHREAD_RWLOCK_INITIALIZER;
static atomic_int started, finish;
static atomic_int request, completed;
static pthread_attr_t attr;
static atomic_int worker_tid;
static int detached;
static int error;
static void *worker(void *unused)
{
	(void)unused;
	int tid = syscall(SYS_gettid);
	atomic_store(&worker_tid, tid);
	int r = pthread_rwlock_wrlock(&lock);
	if (r) {
		fprintf(stderr, "FAIL: new thread tid=%d uncontended wrlock=%d\n", tid, r);
		error = r;
	} else if (pthread_rwlock_unlock(&lock)) error = 1;
	atomic_store(&started, 1);
	while (!atomic_load(&finish)) sched_yield();
	return NULL;
}

static int run_worker(void)
{
	pthread_t thread;
	atomic_store(&started, 0); atomic_store(&finish, 0);
	if (pthread_create(&thread, &attr, worker, NULL)) return 2;
	while (!atomic_load(&started)) sched_yield();
	/* Firefox also forks while these stacks are live. The completed child
	 * must not leave an old copy authoritative when this stack is reused. */
	pid_t child = fork();
	if (child < 0) return 2;
	if (!child) _exit(0);
	int status;
	if (waitpid(child, &status, 0) != child ||
	    !WIFEXITED(status) || WEXITSTATUS(status)) return 2;
	atomic_store(&finish, 1);
	if (detached) {
		/* Wait for native task disappearance without reading the join word.
		 * Its stack can then safely be reused by the next creator. */
		while (syscall(SYS_tgkill, getpid(), atomic_load(&worker_tid), 0) == 0)
			sched_yield();
		if (errno != ESRCH) return 2;
	} else if (pthread_join(thread, NULL)) return 2;
	return error ? 1 : 0;
}

static void *creator(void *unused)
{
	(void)unused;
	for (;;) {
		int r = atomic_load(&request);
		if (r < 0) return NULL;
		if (!r) { sched_yield(); continue; }
		if (run_worker()) error = 1;
		atomic_store(&request, 0);
		atomic_store(&completed, 1);
	}
}

int main(int argc, char **argv)
{
	(void)argv;
	detached = TEST_DETACHED || argc > 1;
	const size_t size = 1024 * 1024;
	void *stack = mmap(NULL, size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (stack == MAP_FAILED || pthread_attr_init(&attr) ||
	    pthread_attr_setstack(&attr, stack, size)) return 2;
	if (detached && pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED))
		return 2;
	pthread_t other;
	if (pthread_create(&other, NULL, creator, NULL)) return 2;
	for (unsigned i = 0; i < 64; i++) {
		if (i & 1) {
			atomic_store(&completed, 0);
			atomic_store(&request, 1);
			while (!atomic_load(&completed)) sched_yield();
		} else if (run_worker()) return 1;
		if (error) return 1;
	}
	atomic_store(&request, -1);
	if (pthread_join(other, NULL)) return 2;
	if (pthread_attr_destroy(&attr) || munmap(stack, size)) return 2;
	printf("PASS: two creators reuse one stack for 64 %s threads across forks and acquire an uncontended rwlock\n",
	       detached ? "detached" : "joined");
	return 0;
}
