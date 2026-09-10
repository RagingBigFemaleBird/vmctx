// SPDX-License-Identifier: GPL-2.0
/* A child rejected after vmctx_copy_task must release its context and mm.
 * No guest or network: a service performs clone3 with an invalid pidfd output.
 * cc -O2 -Wall -Wextra -static -D__EXPORTED_HEADERS__ \
 *    -I src/linux-7.0.14/include/uapi fork-failure.c -o fork-failure
 * Run: fork-failure <vmctx_run nr> <vmctx_ctl nr>
 * One failed fork only: old kernels leak the abandoned child's mm record.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/sched.h>
#include <linux/vmctx.h>

static volatile sig_atomic_t child, timed_out;
static void timeout_handler(int sig)
{
	(void)sig;
	timed_out = 1;
	if (child > 0)
		kill(child, SIGKILL);
}

static long mm_live(void)
{
	long n = -1;
	FILE *f = fopen("/sys/module/kernel/parameters/vmctx_mm_live", "r");
	if (f) {
		if (fscanf(f, "%ld", &n) != 1)
			n = -1;
		fclose(f);
	}
	return n;
}

int main(int argc, char **argv)
{
	struct clone_args clone_args = {
		.flags = CLONE_PIDFD, .pidfd = 1, .exit_signal = SIGCHLD,
	};
	struct vmctx_run_config cfg = {
		.flags = VMCTX_FLAG_SERVICE | VMCTX_FLAG_WAIT_MONITOR,
		.backing_fd = -1, .shared_fd = -1,
	};
	struct vmctx_syscall call = {
		.nr = SYS_clone3,
		.args = {(uintptr_t)&clone_args, sizeof(clone_args)},
	};
	struct sigaction sa = {.sa_handler = timeout_handler};
	int attached = 0, ctl_status = -1, status = 0;
	long before, during = -1, after_call = -1, after;
	pid_t reaped = 0;
	pid_t parent = getpid();
	if (argc != 3)
		return 2;
	long run_nr = strtol(argv[1], NULL, 10);
	long ctl_nr = strtol(argv[2], NULL, 10);
	if (run_nr <= 0 || ctl_nr <= 0)
		return 2;
	/* Confirm that the ordinary kernel reaches the intended error. */
	errno = 0;
	if (syscall(SYS_clone3, &clone_args, sizeof(clone_args)) != -1 || errno != EFAULT) {
		perror("native clone3 did not return EFAULT");
		return 2;
	}
	before = mm_live();
	if (before < 0)
		return 2;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGALRM, &sa, NULL))
		return 2;
	child = fork();
	if (child < 0)
		return 2;
	if (!child) {
		if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent)
			_exit(125);
		syscall(run_nr, &cfg);
		_exit(126);
	}
	alarm(15);
	for (int i = 0; i < 2000 && !timed_out; i++) {
		if (!syscall(ctl_nr, child, VMCTX_CTL_ATTACH, NULL)) {
			attached = 1;
			break;
		}
		usleep(1000);
	}
	if (attached && !timed_out) {
		during = mm_live();
		ctl_status = syscall(ctl_nr, child, VMCTX_CTL_SYSCALL, &call);
		after_call = mm_live();
	}
	kill(child, SIGKILL);
	for (int i = 0; i < 300; i++) {
		reaped = waitpid(child, &status, WNOHANG);
		if (reaped == child || (reaped < 0 && errno == ECHILD))
			break;
		usleep(10000);
	}
	alarm(0);
	after = mm_live();
	int pass = attached && !timed_out && !ctl_status && call.ret == -EFAULT &&
		   reaped == child && during == before + 1 && after_call == during &&
		   after == before;
	printf("%s attached=%d ctl=%d ret=%lld reaped=%d timeout=%d "
	       "mm_live=%ld/%ld/%ld/%ld\n", pass ? "PASS" : "FAIL", attached,
	       ctl_status, (long long)call.ret, reaped == child, (int)timed_out,
	       before, during, after_call, after);
	return pass ? 0 : 1;
}
