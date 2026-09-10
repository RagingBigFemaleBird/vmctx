// SPDX-License-Identifier: GPL-2.0
/* Linux native MM semantics, observable without trusting a monitor's lineage.
 * Child exec detaches from the parent's MM; parent exec also leaves a live
 * CLONE_VM, non-CLONE_THREAD child using the old MM. Raw vfork never calls C
 * or changes the shared stack in its child before execve. */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define INITIAL 0x12345678
#define SHARED 0x23456789
static _Atomic int canary = INITIAL;
static int waited(pid_t child, int expected)
{
	int status; pid_t got;
	do { got = waitpid(child, &status, 0); } while (got < 0 && errno == EINTR);
	return got == child && WIFEXITED(status) && WEXITSTATUS(status) == expected;
}
static int byte(int fd, int write_it)
{
	char c = 'x'; ssize_t n;
	do { n = write_it ? write(fd, &c, 1) : read(fd, &c, 1); } while (n < 0 && errno == EINTR);
	return n == 1 && c == 'x';
}
static int child_exec(void *unused)
{
	(void)unused;
	atomic_store_explicit(&canary, SHARED, memory_order_seq_cst);
	char *args[] = {"exec-shared-mm", "after-child", NULL};
	execv("/proc/self/exe", args);
	_exit(127);
}
struct survivor { int ready, release, done; };
static int survives_parent(void *opaque)
{
	struct survivor *s = opaque;
	atomic_store_explicit(&canary, SHARED, memory_order_seq_cst);
	if (!byte(s->ready, 1) || !byte(s->release, 0)) _exit(71);
	if (atomic_load_explicit(&canary, memory_order_seq_cst) != SHARED) _exit(72);
	atomic_store_explicit(&canary, SHARED + 1, memory_order_seq_cst);
	if (!byte(s->done, 1)) _exit(73);
	_exit(41);
}
static int one(unsigned kind)
{
	atomic_store_explicit(&canary, INITIAL, memory_order_seq_cst);
	void *stack = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (stack == MAP_FAILED) return 1;
	pid_t child;
	if (kind < 2) {
		child = clone(child_exec, (char *)stack + 65536,
			CLONE_VM | SIGCHLD | (kind ? CLONE_VFORK : 0), NULL);
	} else if (kind == 2) {
		char *args[] = {"exec-shared-mm", "after-child", NULL};
		const char *path = "/proc/self/exe";
		long result;
		asm volatile("mov $58,%%eax; syscall; test %%rax,%%rax; jne 1f\n"
			"movl $0x23456789,(%[slot]); mov $59,%%eax; xor %%edx,%%edx; syscall\n"
			"mov $127,%%edi; mov $60,%%eax; syscall; ud2\n1:"
			: "=&a"(result), "+D"(path)
			: "S"(args), [slot]"r"(&canary)
			: "rcx", "r11", "rdx", "memory", "cc");
		if (result < 0) { errno = -result; child = -1; }
		else child = result;
	} else {
		int ready[2], release[2], done[2];
		if (pipe(ready) || pipe(release) || pipe(done)) return 1;
		struct survivor s = {ready[1], release[0], done[1]};
		child = clone(survives_parent, (char *)stack + 65536, CLONE_VM | SIGCHLD, &s);
		if (child < 0 || !byte(ready[0], 0)) return 1;
		char release_fd[32], done_fd[32], child_pid[32];
		snprintf(release_fd, sizeof(release_fd), "%d", release[1]);
		snprintf(done_fd, sizeof(done_fd), "%d", done[0]);
		snprintf(child_pid, sizeof(child_pid), "%d", child);
		char *args[] = {"exec-shared-mm", "after-parent", release_fd, done_fd, child_pid, NULL};
		execv("/proc/self/exe", args);
		kill(child, SIGKILL); (void)waited(child, 0);
		return 1;
	}
	int good = child > 0 && waited(child, 0) &&
		atomic_load_explicit(&canary, memory_order_seq_cst) == SHARED;
	munmap(stack, 65536);
	return good ? 0 : 1;
}
int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "after-child"))
		return atomic_load_explicit(&canary, memory_order_seq_cst) == INITIAL ? 0 : 81;
	if (argc == 5 && !strcmp(argv[1], "after-parent")) {
		int good = atomic_load_explicit(&canary, memory_order_seq_cst) == INITIAL;
		good &= byte(atoi(argv[2]), 1) && byte(atoi(argv[3]), 0);
		good &= waited((pid_t)atoi(argv[4]), 41);
		good &= atomic_load_explicit(&canary, memory_order_seq_cst) == INITIAL;
		return good ? 0 : 82;
	}
	if (argc != 1) return 2;
	setvbuf(stdout, NULL, _IONBF, 0);
	for (unsigned kind = 0; kind < 4; kind++) {
		pid_t pilot = fork();
		if (!pilot) _exit(one(kind));
		if (pilot < 0 || !waited(pilot, 0)) {
			fprintf(stderr, "FAIL: exec MM shape %u\n", kind); return 1;
		}
		printf("exec MM shape %u preserved both address spaces\n", kind);
	}
	puts("PASS: child exec, clone-vfork, raw vfork and parent exec preserve distinct MM lifetimes");
	return 0;
}
