// SPDX-License-Identifier: GPL-2.0
/* Both exit identifiers must reach native source entry policy. */
#define _GNU_SOURCE
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#define SKIPPED 0x19283746UL
static void tracee(void)
{
	if (ptrace(PTRACE_TRACEME, 0, 0, 0) || raise(SIGSTOP)) _exit(2);
	if (syscall(SYS_exit, 37) != SKIPPED) _exit(3);
	long expected = getpid();
	if (syscall(SYS_exit_group, 38) != expected) _exit(4);
	syscall(SYS_exit_group, 0);
	_exit(5);
}
static int trace_test(void)
{
	pid_t child = fork();
	if (child < 0) return 1;
	if (!child) tracee();
	int status = 0, entering = 1, counts[3] = {0};
	if (waitpid(child, &status, 0) != child || !WIFSTOPPED(status) ||
	    WSTOPSIG(status) != SIGSTOP || ptrace(PTRACE_SETOPTIONS, child, 0,
		(void *)(uintptr_t)(PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL))) goto fail;
	for (;;) {
		if (ptrace(PTRACE_SYSCALL, child, 0, 0) || waitpid(child, &status, 0) != child) goto fail;
		if (WIFEXITED(status)) break;
		if (!WIFSTOPPED(status) || WSTOPSIG(status) != (SIGTRAP | 0x80)) goto fail;
		struct user_regs_struct r;
		if (ptrace(PTRACE_GETREGS, child, 0, &r)) goto fail;
		if (entering) {
			if (r.orig_rax == SYS_exit && r.rdi == 37) {
				counts[0]++; r.orig_rax = -1UL; r.rax = SKIPPED;
			} else if (r.orig_rax == SYS_exit_group && r.rdi == 38) {
				counts[1]++; r.orig_rax = SYS_getpid;
			} else if (r.orig_rax == SYS_exit_group && r.rdi == 0) {
				counts[2]++; r.rdi = 73;
			}
			if (ptrace(PTRACE_SETREGS, child, 0, &r)) goto fail;
		}
		entering = !entering;
	}
	if (WEXITSTATUS(status) == 73 && counts[0] == 1 && counts[1] == 1 && counts[2] == 1) return 0;
fail:
	fprintf(stderr, "FAIL: exit tracing status=%x errno=%d counts=%d/%d/%d\n",
		status, errno, counts[0], counts[1], counts[2]);
	kill(child, SIGKILL);
	while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
	return 1;
}
static int filter_test(void)
{
	pid_t child = fork();
	if (child < 0) return 1;
	if (!child) {
		struct sock_filter code[] = {
			BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
			BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_exit, 1, 0),
			BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_exit_group, 0, 3),
			BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])),
			BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 73, 1, 0),
			BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EACCES),
			BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
		};
		struct sock_fprog program = {.len = sizeof(code) / sizeof(code[0]), .filter = code};
		if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
		    prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program)) _exit(2);
		if (syscall(SYS_exit, 37) != -1 || errno != EACCES ||
		    syscall(SYS_exit_group, 38) != -1 || errno != EACCES) {
			kill(getpid(), SIGKILL); for (;;) pause();
		}
		syscall(SYS_exit_group, 73);
		kill(getpid(), SIGKILL); for (;;) pause();
	}
	int status;
	if (waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 73) return 0;
	fprintf(stderr, "FAIL: seccomp exit status=%x errno=%d\n", status, errno);
	return 1;
}
int main(void)
{
	alarm(20);
	if (trace_test() || filter_test()) return 1;
	puts("PASS: source ptrace suppresses exit, rewrites exit_group and status; source seccomp rejects both exit identifiers");
	return 0;
}
