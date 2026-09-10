/*
 * A thread of a forked process, arming a descriptor its process created.
 *
 * This is the smallest shape that tells the two halves of the guest apart.
 * The parent forks; the child creates a timerfd and then makes a thread, and
 * the thread arms it. Every ingredient matters:
 *
 *   - the timerfd is created by the *child*, so its fd number means something
 *     only in the child's context;
 *   - it is armed by a *thread*, so the request travels the path that asks a
 *     context for another serving thread;
 *   - timerfd_settime answers EINVAL, not EBADF, when handed an fd that is
 *     open but is not a timerfd — so getting the wrong process's fd table
 *     looks like a bad argument rather than a bad descriptor.
 *
 * With the thread served by the first shadow instead of the child's, that fd
 * number is some unrelated object and this prints EINVAL for ever, which is
 * what a GLib main loop does with it.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/timerfd.h>
#include <sys/wait.h>

static int tfd = -1;

static void *arm(void *p)
{
	struct itimerspec its;
	int i, bad = 0;

	(void)p;
	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = 1;
	its.it_interval.tv_sec = 1;

	for (i = 0; i < 5; i++) {
		if (timerfd_settime(tfd, 0, &its, NULL) < 0) {
			printf("THREAD-FAIL timerfd_settime(fd=%d): %s\n",
			       tfd, strerror(errno));
			bad = 1;
			break;
		}
	}
	if (!bad)
		printf("THREAD-OK armed fd=%d five times\n", tfd);
	fflush(stdout);
	return NULL;
}

int main(void)
{
	pid_t kid = fork();

	if (kid < 0) {
		perror("fork");
		return 1;
	}
	if (kid == 0) {
		pthread_t t;

		tfd = timerfd_create(CLOCK_MONOTONIC, 0);
		if (tfd < 0) {
			printf("CHILD-FAIL timerfd_create: %s\n", strerror(errno));
			fflush(stdout);
			_exit(2);
		}
		printf("child made timerfd fd=%d\n", tfd);
		fflush(stdout);
		if (pthread_create(&t, NULL, arm, NULL) != 0) {
			printf("CHILD-FAIL pthread_create\n");
			fflush(stdout);
			_exit(3);
		}
		pthread_join(t, NULL);
		_exit(0);
	}
	int st = 0;
	waitpid(kid, &st, 0);
	printf("child exited %d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
	fflush(stdout);
	return 0;
}
