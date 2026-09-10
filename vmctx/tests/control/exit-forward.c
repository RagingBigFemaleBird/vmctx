// SPDX-License-Identifier: GPL-2.0
/* Native backend: opaque exit identifiers must be observable and resumable. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>

extern char exit_probe[], exit_probe_first[], exit_probe_second[], exit_probe_done[];
asm(".text\n.global exit_probe,exit_probe_first,exit_probe_second,exit_probe_done\n"
    "exit_probe: mov $60,%eax; mov $37,%edi; syscall\n"
    "exit_probe_first: cmp $101,%rax; jne exit_probe_bad\n"
    "mov $231,%eax; mov $38,%edi; syscall\n"
    "exit_probe_second: cmp $102,%rax; jne exit_probe_bad\n"
    "exit_probe_done: ud2\nexit_probe_bad: ud2\n");
static pid_t child;
static long ctl_nr;
static void deadline(int sig) { (void)sig; if (child > 0) kill(child, SIGKILL); }
static int control(unsigned op, void *arg)
{
	for (unsigned i = 0; i < 10000; i++) {
		int r = syscall(ctl_nr, child, op, arg);
		if (!r || errno != EAGAIN) return r;
		usleep(100);
	}
	errno = ETIMEDOUT; return -1;
}
#define CHECK(test) do { if (!(test)) { fprintf(stderr, "FAIL: exit-forward line=%d errno=%d\n", __LINE__, errno); goto done; } } while (0)
int main(int argc, char **argv)
{
	if (argc != 3) return 2;
	long run_nr = strtol(argv[1], NULL, 10); ctl_nr = strtol(argv[2], NULL, 10);
	signal(SIGALRM, deadline); alarm(15);
	struct vmctx_run_config cfg = {.flags = VMCTX_FLAG_USERCODE | VMCTX_FLAG_WAIT_MONITOR |
		VMCTX_FLAG_RESTORE | VMCTX_FLAG_REDIRECT_FAULT | VMCTX_FLAG_REDIRECT_SYSCALL,
		.backing_fd = -1, .shared_fd = -1, .max_exits = 128};
	void *stack = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (stack == MAP_FAILED) return 2;
	pid_t parent = getpid(); child = fork();
	if (child < 0) return 2;
	if (!child) {
		if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(125);
		syscall(run_nr, &cfg); _exit(126);
	}
	int passed = 0, status, attached = 0; unsigned calls = 0;
	for (unsigned i = 0; i < 2000; i++) {
		if (!control(VMCTX_CTL_ATTACH, NULL)) { attached = 1; break; }
		usleep(1000);
	}
	CHECK(attached);
	struct vmctx_cpu_model model;
	CHECK(!syscall(ctl_nr, 0, VMCTX_CTL_CPU_CAPS, &model));
	CHECK(!control(VMCTX_CTL_CPU_MODEL, &model));
	struct vmctx_cpu_state cpu;
	CHECK(!control(VMCTX_CTL_GETCPU, &cpu));
	cpu.regs.rip = (uintptr_t)exit_probe; cpu.regs.rsp = (uintptr_t)stack + 4096;
	cpu.regs.rflags = 0x202; cpu.regs.orig_rax = -1ULL;
	CHECK(!control(VMCTX_CTL_SETCPU, &cpu));
	struct vmctx_reply reply = {.action = VMCTX_ACT_SELF};
	CHECK(!control(VMCTX_CTL_RESUME, &reply));
	for (unsigned i = 0; i < 128; i++) {
		struct vmctx_event event;
		CHECK(!control(VMCTX_CTL_WAIT, &event));
		if (event.type == VMCTX_EV_FAULT && event.nr == 6) {
			CHECK(calls == 2 && event.rip == (uintptr_t)exit_probe_done);
			passed = 1; break;
		}
		reply = (struct vmctx_reply){.action = VMCTX_ACT_SELF};
		if (event.type == VMCTX_EV_SYSCALL) {
			CHECK(calls < 2 && event.nr == (calls ? 231 : 60) && event.args[0] == 37 + calls);
			CHECK(event.rip == (uintptr_t)(calls ? exit_probe_second : exit_probe_first));
			reply.action = VMCTX_ACT_DONE; reply.retval = 101 + calls++;
		} else CHECK(event.type == VMCTX_EV_FAULT && event.nr == 14);
		CHECK(!control(VMCTX_CTL_RESUME, &reply));
	}
done:
	kill(child, SIGKILL);
	if (waitpid(child, &status, 0) != child || !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) passed = 0;
	child = 0; alarm(0); munmap(stack, 4096);
	if (passed) puts("PASS: backend forwards both exit identifiers and resumes the monitor's returned CPU results");
	return passed ? 0 : 1;
}
