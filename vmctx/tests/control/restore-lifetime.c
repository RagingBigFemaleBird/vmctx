// SPDX-License-Identifier: GPL-2.0
/* restore-lifetime <run nr> <ctl nr> <hold|release|detach|owner-exit|no-attach> */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/vmctx.h>
#include "vmctx_runtime.h"
extern char lifetime_guest[];
asm(".text\n.global lifetime_guest\nlifetime_guest: lock incq (%rdi); jmp lifetime_guest\n");
static volatile sig_atomic_t child, owner, expired;
static long ctl_nr;
static void deadline(int sig)
{
	(void)sig; expired = 1;
	if (child > 0) kill(child, SIGKILL);
	if (owner > 0) kill(owner, SIGKILL);
}
static uint64_t millis(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
	return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int control(unsigned op, void *arg)
{
	uint64_t until = millis() + 2000;
	do {
		int r = syscall(ctl_nr, child, op, arg);
		if (!r || errno != EAGAIN) return r;
		usleep(100);
	} while (!expired && millis() < until);
	errno = ETIMEDOUT; return -1;
}
static int install(uint64_t *counter)
{
	struct vmctx_cpu_model model;
	struct vmctx_cpu_state cpu;
	uint64_t until = millis() + 2000;
	do {
		if (!control(VMCTX_CTL_ATTACH, NULL)) break;
		usleep(1000);
	} while (!expired && millis() < until);
	if (syscall(ctl_nr, 0, VMCTX_CTL_CPU_CAPS, &model) ||
	    control(VMCTX_CTL_GETCPU, &cpu)) return -1;
	cpu.regs.rip = (uintptr_t)lifetime_guest;
	cpu.regs.rdi = (uintptr_t)counter;
	cpu.regs.rsp = (uintptr_t)counter + 65536 - 256;
	cpu.regs.rflags = 0x202; cpu.regs.orig_rax = ~0ULL;
	return control(VMCTX_CTL_CPU_MODEL, &model) || control(VMCTX_CTL_SETCPU, &cpu);
}
static pid_t reap_for(int *status, unsigned ms)
{
	uint64_t until = millis() + ms;
	do {
		pid_t p = waitpid(child, status, WNOHANG);
		if (p == child) child = 0;
		if (p) return p;
		usleep(1000);
	} while (!expired && millis() < until);
	return 0;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s errno=%d expired=%d status=%#x child=%d\n", __LINE__, #x, errno, expired, status, child); goto done; } } while (0)
int main(int argc, char **argv)
{
	int pass = 0, status = 0, channel[2] = {-1, -1};
	uint64_t *counter = MAP_FAILED, held = 0;
	struct sigaction sa = {.sa_handler = deadline};
	if (argc != 4 || sigaction(SIGALRM, &sa, NULL)) return 2;
	const char *mode = argv[3];
	if (strcmp(mode, "hold") && strcmp(mode, "release") && strcmp(mode, "detach") &&
	    strcmp(mode, "owner-exit") && strcmp(mode, "no-attach")) return 2;
	long run_nr = strtol(argv[1], NULL, 10); ctl_nr = strtol(argv[2], NULL, 10);
	alarm(25);
	counter = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	CHECK(counter != MAP_FAILED);
	struct vmctx_run_config cfg = {.flags = VMCTX_FLAG_USERCODE | VMCTX_FLAG_WAIT_MONITOR |
		VMCTX_FLAG_RESTORE | VMCTX_FLAG_REDIRECT_FAULT | VMCTX_FLAG_REDIRECT_SYSCALL,
		.backing_fd = -1, .shared_fd = -1};
	pid_t parent = getpid(); child = fork(); CHECK(child >= 0);
	if (!child) {
		if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(125);
		syscall(run_nr, &cfg); _exit(126);
	}
	if (!strcmp(mode, "no-attach")) {
		uint64_t started = millis();
		CHECK(reap_for(&status, 8000) > 0);
		held = millis() - started;
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == ETIMEDOUT && !counter[0] && held >= 4500);
	} else {
		if (!strcmp(mode, "owner-exit")) {
			CHECK(!pipe(channel)); owner = fork(); CHECK(owner >= 0);
			if (!owner) {
				close(channel[0]);
				if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent || install(counter)) _exit(2);
				if (write(channel[1], "R", 1) != 1) _exit(3);
				for (;;) pause();
			}
			close(channel[1]); channel[1] = -1;
			char ready; CHECK(read(channel[0], &ready, 1) == 1 && ready == 'R');
		} else CHECK(!install(counter));
		uint64_t started = millis();
		while (millis() - started < 6200 && !expired) {
			CHECK(reap_for(&status, 1) == 0 && !counter[0]);
			usleep(1000);
		}
		held = millis() - started; CHECK(!expired && held >= 6200);
		if (!strcmp(mode, "release")) {
			struct vmctx_runtime runtime = {.version = VMCTX_RUNTIME_ABI, .size = sizeof(runtime),
				.op = VMCTX_RUNTIME_ARM, .quantum_ns = 1000000};
			struct vmctx_reply reply = {.action = VMCTX_ACT_SELF};
			CHECK(!control(VMCTX_CTL_RUNTIME, &runtime) && !control(VMCTX_CTL_RESUME, &reply));
			for (unsigned faults = 0;; faults++) {
				struct vmctx_event ev; CHECK(faults < 16 && !control(VMCTX_CTL_WAIT, &ev));
				if (ev.type == VMCTX_EV_RUNTIME) {
					reply.action = VMCTX_ACT_DONE;
					CHECK(counter[0] && !control(VMCTX_CTL_RESUME, &reply));
					break;
				}
				CHECK(ev.type == VMCTX_EV_FAULT && ev.nr == 14);
				CHECK(!control(VMCTX_CTL_RESUME, &reply));
			}
			CHECK(counter[0]);
		}
		if (!strcmp(mode, "detach")) {
			CHECK(!control(VMCTX_CTL_DETACH, NULL));
			CHECK(reap_for(&status, 3500) > 0 && WIFEXITED(status) && WEXITSTATUS(status) == ENOTCONN && !counter[0]);
		} else if (!strcmp(mode, "owner-exit")) {
			CHECK(!kill(owner, SIGKILL));
			CHECK(waitpid(owner, &status, 0) == owner && WIFSIGNALED(status)); owner = 0;
			CHECK(reap_for(&status, 3500) > 0 && WIFEXITED(status) && WEXITSTATUS(status) == ENOTCONN && !counter[0]);
		} else {
			CHECK(!kill(child, SIGKILL));
			CHECK(reap_for(&status, 3500) > 0 && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
		}
	}
	pass = 1;
	printf("PASS: restore %s held_ms=%llu counter=%llu status=%#x\n", mode,
		(unsigned long long)held, (unsigned long long)counter[0], status);
done:
	if (child > 0) { kill(child, SIGKILL); while (waitpid(child, &status, 0) < 0 && errno == EINTR) {} child = 0; }
	if (owner > 0) { kill(owner, SIGKILL); while (waitpid(owner, &status, 0) < 0 && errno == EINTR) {} owner = 0; }
	alarm(0);
	for (int i = 0; i < 2; i++) if (channel[i] >= 0) close(channel[i]);
	if (counter != MAP_FAILED) munmap(counter, 65536);
	return pass ? 0 : 1;
}
