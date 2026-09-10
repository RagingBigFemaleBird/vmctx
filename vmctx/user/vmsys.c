// SPDX-License-Identifier: GPL-2.0
/*
 * vmsys — does a syscall a monitor asks for run in the *context's* address
 * space, or in the monitor's?
 *
 * This is the property VMCTX_CTL_SYSCALL exists for, and the reason a monitor
 * may not simply make the call itself. A monitor is a different process with a
 * different address space: the same address is different memory in each of
 * them. So a monitor that performs a guest's syscall answers with its own
 * memory, and is wrong whenever an argument is a pointer -- which is most
 * syscalls worth forwarding.
 *
 * The test makes the disagreement explicit. Both processes map a page at the
 * *same* fixed address and put different bytes in it:
 *
 *      context  0x50000000: "CONTEXT!"
 *      monitor  0x50000000: "MONITOR!"
 *
 * The monitor then asks the context to write(pipe, 0x50000000, 8). Whichever
 * string comes out of the pipe names the address space the call was made in.
 * "CONTEXT!" is the pass; "MONITOR!" means the call was made by the wrong
 * process and every forwarded pointer argument in the system is suspect.
 *
 * write(2) is used only because it carries bytes out where they can be
 * compared. Nothing here is specific to it, and nothing in the kernel path is:
 * the number is dispatched through the kernel's own table.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sched.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include "vmctx_uapi.h"

#ifndef __NR_vmctx_run
#define __NR_vmctx_run 470
#endif
#ifndef __NR_vmctx_ctl
#define __NR_vmctx_ctl 471
#endif

#define VMCTX_CTL_ATTACH  1
#define VMCTX_CTL_SYSCALL 11

#define VMCTX_FLAG_USERCODE     (1u << 0)
#define VMCTX_FLAG_EXIT_PROCESS (1u << 1)
#define VMCTX_FLAG_WAIT_MONITOR (1u << 4)
#define VMCTX_FLAG_SERVICE      (1u << 6)

/* The address both processes map, with different contents. */
#define SHARED_ADDR 0x50000000UL

struct vmctx_uregs {
	uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
	uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
	uint64_t rip, rflags, orig_rax;
	uint64_t fs_base, gs_base;
};

#define VMCTX_REG_RAX (1u << 0)

struct vmctx_syscall {
	uint64_t nr;
	uint64_t args[6];
	int64_t  ret;
	uint32_t changed;
	uint32_t _pad;
	struct vmctx_uregs regs;
};

static long ctl(pid_t pid, unsigned cmd, void *arg)
{
	return syscall(__NR_vmctx_ctl, pid, cmd, arg);
}

static int child(void *arg)
{
	struct vmctx_run_config cfg;
	void *p;

	(void)arg;

	p = mmap((void *)SHARED_ADDR, 4096, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (p == MAP_FAILED)
		_exit(2);
	memcpy(p, "CONTEXT!", 8);

	memset(&cfg, 0, sizeof(cfg));
	/*
	 * A service context: it holds this address space and makes syscalls in
	 * it, and never executes anything. No entry, no stack, no backend --
	 * this is what the machine that owns a program has to be, and it is why
	 * that machine needs no virtualisation hardware.
	 */
	cfg.flags      = VMCTX_FLAG_SERVICE | VMCTX_FLAG_WAIT_MONITOR;
	cfg.backing_fd = -1;
	cfg.shared_fd  = -1;

	syscall(__NR_vmctx_run, &cfg);

	/*
	 * vmctx_run returns once a monitor has attached; the task stays a
	 * context until it exits, so wait here and keep serving.
	 */
	for (;;)
		pause();
	_exit(4);
}

int main(void)
{
	static char stack[1 << 20];
	struct vmctx_syscall rq;
	char got[16] = { 0 };
	void *p;
	int fd[2];
	pid_t kid;
	int i, ok = 0;

	if (pipe(fd) != 0) {
		perror("pipe");
		return 1;
	}

	/* The monitor's own page, at the same address, with different bytes. */
	p = mmap((void *)SHARED_ADDR, 4096, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (p == MAP_FAILED) {
		perror("monitor mmap");
		return 1;
	}
	memcpy(p, "MONITOR!", 8);

	/*
	 * No CLONE_VM: the context gets an address space of its own, which is
	 * the arrangement being tested. With it shared there would be one page
	 * and nothing to tell apart.
	 */
	kid = clone(child, stack + sizeof(stack), SIGCHLD, NULL);
	if (kid < 0) {
		perror("clone");
		return 1;
	}

	for (i = 0; i < 500; i++) {
		if (ctl(kid, VMCTX_CTL_ATTACH, NULL) == 0) {
			ok = 1;
			break;
		}
		usleep(1000);
	}
	if (!ok) {
		printf("vmsys: could not attach to %d: %s\nFAIL\n",
		       kid, strerror(errno));
		kill(kid, SIGKILL);
		return 1;
	}

	memset(&rq, 0, sizeof(rq));
	rq.nr      = SYS_write;
	rq.args[0] = (uint64_t)fd[1];
	rq.args[1] = SHARED_ADDR;
	rq.args[2] = 8;

	if (ctl(kid, VMCTX_CTL_SYSCALL, &rq) != 0) {
		printf("vmsys: VMCTX_CTL_SYSCALL: %s\nFAIL\n", strerror(errno));
		kill(kid, SIGKILL);
		return 1;
	}
	if (rq.ret != 8) {
		printf("vmsys: the context's write returned %lld, wanted 8\nFAIL\n",
		       (long long)rq.ret);
		kill(kid, SIGKILL);
		return 1;
	}
	if (read(fd[0], got, 8) != 8) {
		printf("vmsys: nothing came out of the pipe\nFAIL\n");
		kill(kid, SIGKILL);
		return 1;
	}

	kill(kid, SIGKILL);
	waitpid(kid, NULL, 0);

	printf("vmsys: the syscall wrote |%.8s| from address 0x%lx\n",
	       got, SHARED_ADDR);
	/*
	 * And what it did to the machine state. A write(2) touches the return
	 * register and nothing else, so the mask should say exactly that -- the
	 * monitor learns it without reading the register set twice and diffing
	 * it across a machine boundary.
	 */
	printf("vmsys: registers the call changed: 0x%x (rax=0x%llx)\n",
	       rq.changed, (unsigned long long)rq.regs.rax);
	if (rq.changed != VMCTX_REG_RAX) {
		printf("vmsys: expected only RAX to change\nFAIL\n");
		return 1;
	}
	if (!memcmp(got, "CONTEXT!", 8)) {
		printf("vmsys: it ran in the context's address space\nPASS\n");
		return 0;
	}
	if (!memcmp(got, "MONITOR!", 8))
		printf("vmsys: it ran in the MONITOR's address space -- every "
		       "forwarded pointer argument is wrong\n");
	printf("FAIL\n");
	return 1;
}
