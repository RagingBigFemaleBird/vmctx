// SPDX-License-Identifier: GPL-2.0
/* A native group exit of a threaded child must not end its parent monitor. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/futex.h>

static atomic_int ready;
static int gate;
static void *waiter(void *unused)
{
	(void)unused;
	atomic_fetch_add(&ready, 1);
	for (;;) syscall(SYS_futex, &gate, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
	return NULL;
}
int main(void)
{
	for (int round = 0; round < 8; round++) {
		pid_t child = fork();
		int status;
		if (child < 0) return 2;
		if (!child) {
			pthread_t threads[16];
			atomic_store(&ready, 0);
			for (int i = 0; i < 16; i++)
				if (pthread_create(&threads[i], NULL, waiter, NULL)) _exit(2);
			while (atomic_load(&ready) != 16) usleep(1000);
			usleep(20000);
			syscall(SYS_exit_group, 37);
			_exit(3);
		}
		if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
		    WEXITSTATUS(status) != 37) {
			printf("FAIL: group exit round %d status 0x%x\n", round, status);
			return 1;
		}
	}
	puts("PASS: 8 threaded child group exits preserve parent execution and status");
	return 0;
}
