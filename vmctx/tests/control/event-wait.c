// SPDX-License-Identifier: GPL-2.0
/* A fatal cancellation is not a fault deadline. The deadline positive control
 * also checks real elapsed time, so an inert counter cannot pass cancellation. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/vmctx.h>
#include "vmctx_runtime.h"

extern char wait_trap[];
asm(".text\n.global wait_trap\nwait_trap: ud2\n");
extern char wait_spin[];
asm(".text\n.global wait_spin\nwait_spin: pause; jmp wait_spin\n");
static volatile sig_atomic_t child;
static long ctl_nr;

static void alarm_handler(int sig)
{
	(void)sig;
	if (child > 0) kill(child, SIGKILL);
}

static long control(unsigned op, void *arg)
{
	return syscall(ctl_nr, child, op, arg);
}

static long parameter(const char *name)
{
	char path[160];
	long value = -1;
	snprintf(path, sizeof(path), "/sys/module/kernel/parameters/%s", name);
	FILE *file = fopen(path, "r");
	if (!file) return -1;
	if (fscanf(file, "%ld", &value) != 1) value = -1;
	fclose(file);
	return value;
}

static uint64_t monotonic_ns(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts)) abort();
	return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

#define CHECK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL line %d: %s errno=%d\n", __LINE__, #x, errno); \
	goto done; } } while (0)

int main(int argc, char **argv)
{
	if (argc != 4 || (strcmp(argv[3], "cancel") && strcmp(argv[3], "deadline") && strcmp(argv[3], "runtime-cancel")))
		return 2;
	long run_nr = strtol(argv[1], NULL, 10);
	ctl_nr = strtol(argv[2], NULL, 10);
	int runtime = !strcmp(argv[3], "runtime-cancel");
	int cancel = runtime || !strcmp(argv[3], "cancel"), pass = 0;
	long timeout_ms = parameter("vmctx_fault_deadline_ms");
	long before = parameter("vmctx_fault_deadline_hits");
	void *stack = MAP_FAILED;
	struct sigaction sa = {.sa_handler = alarm_handler};
	CHECK(timeout_ms > 0 && timeout_ms < 30000 && before >= 0);
	CHECK(!sigaction(SIGALRM, &sa, NULL));
	alarm((unsigned)(timeout_ms / 1000) + 10);
	stack = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(stack != MAP_FAILED);
	struct vmctx_run_config cfg = {
		.flags = VMCTX_FLAG_USERCODE | VMCTX_FLAG_RESTORE |
			 VMCTX_FLAG_WAIT_MONITOR | VMCTX_FLAG_EXIT_PROCESS |
			 VMCTX_FLAG_REDIRECT_FAULT | VMCTX_FLAG_REDIRECT_SYSCALL,
		.backing_fd = -1, .shared_fd = -1,
	};
	pid_t parent = getpid();
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(125);
		syscall(run_nr, &cfg);
		_exit(126);
	}
	for (unsigned i = 0; control(VMCTX_CTL_ATTACH, NULL); i++) {
		CHECK(i < 2000);
		usleep(1000);
	}
	struct vmctx_cpu_model model;
	struct vmctx_cpu_state cpu;
	CHECK(!syscall(ctl_nr, 0, VMCTX_CTL_CPU_CAPS, &model));
	CHECK(!control(VMCTX_CTL_CPU_MODEL, &model));
	CHECK(!control(VMCTX_CTL_GETCPU, &cpu));
	cpu.regs.rip = (uintptr_t)(runtime ? wait_spin : wait_trap);
	cpu.regs.rsp = (uintptr_t)stack + 4096;
	cpu.regs.rflags = 0x202;
	cpu.regs.orig_rax = ~0ULL;
	CHECK(!control(VMCTX_CTL_SETCPU, &cpu));
	if (runtime) {
		struct vmctx_runtime q = {.version = VMCTX_RUNTIME_ABI, .size = sizeof(q),
			.op = VMCTX_RUNTIME_ARM, .quantum_ns = 1000000};
		CHECK(!control(VMCTX_CTL_RUNTIME, &q));
	}
	struct vmctx_reply reply = {.action = VMCTX_ACT_SELF};
	CHECK(!control(VMCTX_CTL_RESUME, &reply));
	uint64_t start = monotonic_ns();
	for (unsigned i = 0;; i++) {
		struct vmctx_event ev;
		CHECK(i < 32 && !control(VMCTX_CTL_WAIT, &ev));
		if (runtime && ev.type == VMCTX_EV_RUNTIME) {
			start = monotonic_ns();
			break;
		}
		CHECK(ev.type == VMCTX_EV_FAULT);
		if (!runtime && ev.nr == 6) {
			CHECK(ev.rip == (uintptr_t)wait_trap);
			break;
		}
		CHECK(ev.nr == 14 && !control(VMCTX_CTL_RESUME, &reply));
		start = monotonic_ns();
	}
	int status = 0;
	pid_t waited = 0;
	if (cancel) {
		CHECK(!kill(child, SIGKILL));
		do {
			waited = waitpid(child, &status, WNOHANG);
			if (waited) break;
			usleep(1000);
		} while (monotonic_ns() - start < 1000000000);
		if (!waited) {
			char path[64], line[256];
			snprintf(path, sizeof(path), "/proc/%d/stack", child);
			FILE *f = fopen(path, "r");
			while (f && fgets(line, sizeof(line), f)) fputs(line, stderr);
			if (f) fclose(f);
			/* Revoke the taken event for cleanup after recording the
			 * failed kill. Cleanup cannot turn this case into a pass. */
			control(VMCTX_CTL_DETACH, NULL);
			fprintf(stderr, "FAIL: %s event blocked SIGKILL for one second\n", argv[3]);
			goto done;
		}
	} else {
		do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
	}
	CHECK(waited == child);
	child = 0;
	uint64_t elapsed_ms = (monotonic_ns() - start) / 1000000;
	long delta = parameter("vmctx_fault_deadline_hits") - before;
	printf("event-wait: mode=%s status=%#x elapsed_ms=%llu deadline_ms=%ld deadline_delta=%ld\n",
	       argv[3], status, (unsigned long long)elapsed_ms, timeout_ms, delta);
	if (cancel) {
		CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
		CHECK(delta == 0 && elapsed_ms < 1000);
	} else {
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == ((-ECANCELED) & 255));
		CHECK(delta == 1 && elapsed_ms + 100 >= (uint64_t)timeout_ms);
		CHECK(elapsed_ms < (uint64_t)timeout_ms + 3000);
	}
	pass = 1;
	puts("PASS: fatal cancellation and elapsed fault deadline remain distinct");
done:
	if (child > 0) {
		int status;
		kill(child, SIGKILL);
		while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
	}
	alarm(0);
	if (stack != MAP_FAILED) munmap(stack, 4096);
	return pass ? 0 : 1;
}
