// SPDX-License-Identifier: GPL-2.0
/* Initial child ptrace work may outlive five seconds. It must precede guest
 * entry, and the final edited frame must reach the child before it executes. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t worker, child;
static void deadline(int sig)
{
	(void)sig;
	if (worker > 0) kill(worker, SIGKILL);
	if (child > 0) kill(child, SIGKILL);
	_exit(124);
}
static long raw_fork(void)
{
	long nr = SYS_fork;
	asm volatile("syscall" : "+a"(nr) : : "rcx", "r11", "memory");
	return nr;
}
static uint64_t now_ms(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t)) _exit(2);
	return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s errno=%d status=%#x\n", __LINE__, #x, errno, status); goto done; } } while (0)
int main(void)
{
	int pipefd[2], status = 0, pass = 0;
	const uint64_t marker = UINT64_C(0x76435210abcdef98);
	signal(SIGALRM, deadline); alarm(35);
	if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK)) return 2;
	worker = fork(); CHECK(worker >= 0);
	if (!worker) {
		if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) || raise(SIGSTOP)) _exit(3);
		pid_t kid = raw_fork();
		if (kid < 0) _exit(4);
		if (!kid) {
			uint64_t actual;
			asm volatile("mov %%r13,%0" : "=r"(actual));
			_exit(write(pipefd[1], &actual, sizeof(actual)) == sizeof(actual) ? 0 : 5);
		}
		if (waitpid(kid, &status, 0) != kid || !WIFEXITED(status) || WEXITSTATUS(status)) _exit(6);
		_exit(0);
	}
	CHECK(waitpid(worker, &status, 0) == worker && WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
	CHECK(!ptrace(PTRACE_SETOPTIONS, worker, NULL, PTRACE_O_TRACEFORK | PTRACE_O_EXITKILL));
	CHECK(!ptrace(PTRACE_CONT, worker, NULL, NULL));
	CHECK(waitpid(worker, &status, 0) == worker && WIFSTOPPED(status) && (unsigned)status >> 16 == PTRACE_EVENT_FORK);
	unsigned long message = 0;
	CHECK(!ptrace(PTRACE_GETEVENTMSG, worker, NULL, &message) && message > 0);
	child = (pid_t)message;
	CHECK(waitpid(child, &status, __WALL) == child && WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
	struct user_regs_struct regs;
	CHECK(!ptrace(PTRACE_GETREGS, child, NULL, &regs));
	regs.r13 = marker;
	CHECK(!ptrace(PTRACE_SETREGS, child, NULL, &regs));
	CHECK(!ptrace(PTRACE_CONT, worker, NULL, NULL));
	uint64_t start = now_ms(), value = 0;
	while (now_ms() - start < 7000) {
		CHECK(read(pipefd[0], &value, sizeof(value)) == -1 && errno == EAGAIN);
		struct timespec delay = {.tv_nsec = 10000000};
		nanosleep(&delay, NULL);
	}
	CHECK(!ptrace(PTRACE_CONT, child, NULL, NULL));
	CHECK(waitpid(child, &status, __WALL) == child && WIFEXITED(status) && !WEXITSTATUS(status));
	child = 0;
	CHECK(read(pipefd[0], &value, sizeof(value)) == sizeof(value) && value == marker);
	for (;;) {
		CHECK(waitpid(worker, &status, __WALL) == worker);
		if (WIFEXITED(status)) break;
		CHECK(WIFSTOPPED(status) && WSTOPSIG(status) == SIGCHLD && !(status >> 16));
		CHECK(!ptrace(PTRACE_CONT, worker, NULL, SIGCHLD));
	}
	worker = 0;
	CHECK(!WEXITSTATUS(status));
	printf("PASS: child stayed stopped for %llu ms and entered with its final ptrace-edited CPU state\n",
		(unsigned long long)(now_ms() - start));
	pass = 1;
done:
	if (worker > 0) kill(worker, SIGKILL);
	if (child > 0) kill(child, SIGKILL);
	while (waitpid(-1, &status, __WALL) > 0 || errno == EINTR) {}
	close(pipefd[0]); close(pipefd[1]); alarm(0);
	return pass ? 0 : 1;
}
