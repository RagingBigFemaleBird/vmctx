// SPDX-License-Identifier: GPL-2.0
/* A thread's first mapping of a private page after fork must preserve the
 * child's snapshot before granting that thread a write to the shared AS. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGES 32
#define WORDS (PAGES * 4096 / sizeof(uint64_t))
static volatile uint64_t *memory;
static int work[2], done[2], child_release[2], ready[2];

static void send_byte(int fd)
{
	char c = 'x';
	if (write(fd, &c, 1) != 1) _exit(2);
}

static void receive_byte(int fd)
{
	char c;
	ssize_t n;
	do { n = read(fd, &c, 1); } while (n < 0 && errno == EINTR);
	if (n != 1) _exit(2);
}

static uint64_t expected(size_t i) { return 0xa1725b83d496e0f1ULL ^ i; }

static void *late_writer(void *arg)
{
	(void)arg;
	send_byte(ready[1]);
	receive_byte(work[0]);
	/* Read first, then write: mapping a readable page must not silently
	 * grant a write that bypasses the snapshot's protection. */
	for (size_t i = 0; i < WORDS; i++) {
		uint64_t old = memory[i];
		if (old != expected(i)) _exit(3);
		memory[i] = 0xe5e5e5e5e5e5e5e5ULL;
	}
	send_byte(done[1]);
	return NULL;
}

int main(void)
{
	pthread_t thread;
	memory = mmap(NULL, PAGES * 4096, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED || pipe(work) || pipe(done) ||
	    pipe(child_release) || pipe(ready)) return 2;
	for (size_t i = 0; i < WORDS; i++) memory[i] = expected(i);
	if (pthread_create(&thread, NULL, late_writer, NULL)) return 2;
	receive_byte(ready[0]);
	pid_t child = fork();
	if (child < 0) return 2;
	if (!child) {
		receive_byte(child_release[0]);
		for (size_t i = 0; i < WORDS; i++) {
			uint64_t got = memory[i];
			if (got != expected(i)) {
				dprintf(2, "FAIL: late sibling changed fork snapshot word=%zu got=%016llx expected=%016llx\n",
					i, (unsigned long long)got, (unsigned long long)expected(i));
				_exit(1);
			}
		}
		_exit(0);
	}
	send_byte(work[1]);
	receive_byte(done[0]);
	send_byte(child_release[1]);
	int status;
	if (waitpid(child, &status, 0) != child || pthread_join(thread, NULL)) return 2;
	if (!WIFEXITED(status) || WEXITSTATUS(status)) return 1;
	for (size_t i = 0; i < WORDS; i++)
		if (memory[i] != 0xe5e5e5e5e5e5e5e5ULL) return 1;
	puts("PASS: fork snapshot survives a sibling's first read and write of 32 pages");
	return 0;
}
