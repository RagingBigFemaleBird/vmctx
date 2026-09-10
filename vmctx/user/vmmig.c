// SPDX-License-Identifier: GPL-2.0
/*
 * vmmig — checkpoint and restore a VM context (avm.md objective 4).
 *
 *   vmmig run     <image> <n-beacons>   start a guest, checkpoint it, stop
 *   vmmig restore <image> <n-beacons>   bring it back and let it continue
 *
 * The spec asks what extra tools migration needs beyond the previous
 * objectives. The answer turns out to be: almost none. A context's execution
 * state is its guest registers plus its memory, and objective 3 already exposes
 * both (GETREGS/SETREGS, PEEK/POKE). The one kernel addition is
 * VMCTX_FLAG_RESTORE, which lets a context start from a register set a monitor
 * installs instead of from a fresh entry point.
 *
 * To keep the demonstration honest and self-contained, the migrated guest here
 * keeps *all* of its mutable state — its counter and its stack — inside one
 * page-aligned region at a fixed address, so a checkpoint is exactly:
 *
 *     guest registers  +  that region
 *
 * Its code lives in this binary, which is static and identically mapped
 * wherever it runs, so the restored guest finds its instructions where it left
 * them. Migrating an *arbitrary* program (with libraries, an arbitrary mmap
 * layout, and open files) is the CRIU problem and needs address-space surgery
 * this tool does not attempt; see NOTES.md.
 *
 * The guest reports progress with a "beacon" syscall — write(BEACON_FD, &ctr, 8)
 * — which the monitor recognises, so we can watch the counter advance and
 * checkpoint at a known point.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "vmctx_uapi.h"	/* struct vmctx_run_config, shared with the kernel */

#ifndef __NR_vmctx_run
#define __NR_vmctx_run 470
#endif
#ifndef __NR_vmctx_ctl
#define __NR_vmctx_ctl 471
#endif

#define VMCTX_FLAG_USERCODE          1u
#define VMCTX_FLAG_EXIT_PROCESS      2u
#define VMCTX_FLAG_REDIRECT_SYSCALL  4u
#define VMCTX_FLAG_WAIT_MONITOR      16u
#define VMCTX_FLAG_RESTORE           32u

#define VMCTX_CTL_ATTACH  1
#define VMCTX_CTL_WAIT    3
#define VMCTX_CTL_RESUME  4
#define VMCTX_CTL_GETREGS 5
#define VMCTX_CTL_SETREGS 6
#define VMCTX_CTL_PEEK    7
#define VMCTX_CTL_POKE    8

#define VMCTX_EV_SYSCALL 1
#define VMCTX_ACT_SELF 0
#define VMCTX_ACT_DONE 1
#define VMCTX_ACT_KILL 2

struct vmctx_event {
	uint32_t type;
	int32_t  pid;
	uint64_t nr;
	uint64_t args[6];
	uint64_t fault_addr, fault_err;
	uint64_t rip, rsp, rflags;
};
/* struct vmctx_reply comes from vmctx_uapi.h -- see the note there. */
struct vmctx_uregs {
	uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
	uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
	uint64_t rip, rflags, orig_rax;
};
struct vmctx_mem { uint64_t addr, len, buf; };

static long ctl(pid_t pid, unsigned cmd, void *arg)
{
	return syscall(__NR_vmctx_ctl, pid, cmd, arg);
}

/* ---- the migratable guest --------------------------------------------- */

#define REGION_ADDR 0x200000000000UL	/* far from any binary mapping */
#define REGION_LEN  0x10000UL		/* 64 KiB: counter + guest stack */
#define STACK_OFF   0xf000UL
#define BEACON_FD   999			/* not a real fd: the monitor's cue */

static inline long gsys(long nr, long a, long b, long c)
{
	long ret;
	__asm__ volatile("syscall" : "=a"(ret)
			 : "a"(nr), "D"(a), "S"(b), "d"(c)
			 : "rcx", "r11", "memory");
	return ret;
}

static void guest_loop(void)
{
	volatile uint64_t *ctr = (volatile uint64_t *)REGION_ADDR;

	for (;;) {
		(*ctr)++;
		gsys(1 /*write*/, BEACON_FD, (long)ctr, 8);
		for (volatile int i = 0; i < 200000; i++)
			;	/* keep the beacon rate human-readable */
	}
}

/* ---- checkpoint image ------------------------------------------------- */

#define IMG_MAGIC 0x474d4d56ul	/* "VMMG" */

struct img_hdr {
	uint32_t magic;
	uint32_t _pad;
	uint64_t region_addr;
	uint64_t region_len;
	uint64_t counter;		/* informational */
	struct vmctx_uregs regs;
};

static void *map_region(void)
{
	void *p = mmap((void *)REGION_ADDR, REGION_LEN, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

	if (p == MAP_FAILED) {
		perror("mmap region");
		exit(1);
	}
	return p;
}

/* ---- monitor loop shared by run and restore --------------------------- */

/*
 * Drive the context, printing each beacon. Stops after @limit beacons: if
 * @image is set, checkpoint there first (registers + region) and kill the
 * context — that is the "migrate out" half.
 */
static int drive(pid_t pid, unsigned long limit, const char *image)
{
	unsigned long beacons = 0;

	for (;;) {
		struct vmctx_event ev;
		struct vmctx_reply rep = { .action = VMCTX_ACT_SELF };

		if (ctl(pid, VMCTX_CTL_WAIT, &ev) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (ev.type == VMCTX_EV_SYSCALL && ev.nr == 1 &&
		    ev.args[0] == BEACON_FD) {
			uint64_t ctr = 0;
			struct vmctx_mem m = { .addr = ev.args[1], .len = 8,
					       .buf = (uint64_t)(uintptr_t)&ctr };

			ctl(pid, VMCTX_CTL_PEEK, &m);
			beacons++;
			printf("  [beacon %lu] guest counter = %llu (rip=0x%llx)\n",
			       beacons, (unsigned long long)ctr,
			       (unsigned long long)ev.rip);
			fflush(stdout);

			rep.action = VMCTX_ACT_DONE;
			rep.retval = 8;

			if (beacons >= limit) {
				if (image) {
					struct img_hdr h;
					static char buf[REGION_LEN];
					struct vmctx_mem rm = {
						.addr = REGION_ADDR,
						.len  = REGION_LEN,
						.buf  = (uint64_t)(uintptr_t)buf,
					};
					int fd;
					long got = 0, off = 0;

					memset(&h, 0, sizeof(h));
					h.magic = IMG_MAGIC;
					h.region_addr = REGION_ADDR;
					h.region_len  = REGION_LEN;
					h.counter = ctr;
					if (ctl(pid, VMCTX_CTL_GETREGS, &h.regs) < 0) {
						perror("GETREGS");
						return 1;
					}
					/* PEEK is capped per call; walk the region. */
					while ((uint64_t)off < REGION_LEN) {
						rm.addr = REGION_ADDR + off;
						rm.len  = 4096;
						rm.buf  = (uint64_t)(uintptr_t)(buf + off);
						got = ctl(pid, VMCTX_CTL_PEEK, &rm);
						if (got <= 0)
							break;
						off += got;
					}
					fd = open(image, O_WRONLY | O_CREAT | O_TRUNC, 0600);
					if (fd < 0) {
						perror(image);
						return 1;
					}
					if (write(fd, &h, sizeof(h)) != sizeof(h) ||
					    write(fd, buf, REGION_LEN) != (ssize_t)REGION_LEN) {
						perror("write image");
						close(fd);
						return 1;
					}
					close(fd);
					printf("checkpointed: counter=%llu rip=0x%llx rsp=0x%llx"
					       " region=%luB -> %s\n",
					       (unsigned long long)h.counter,
					       (unsigned long long)h.regs.rip,
					       (unsigned long long)h.regs.rsp,
					       (unsigned long)REGION_LEN, image);
				}
				rep.action = VMCTX_ACT_KILL;
				ctl(pid, VMCTX_CTL_RESUME, &rep);
				break;
			}
		}
		if (ctl(pid, VMCTX_CTL_RESUME, &rep) < 0)
			break;
	}
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	return 0;
}

static pid_t start_context(uint64_t extra_flags, uint64_t entry, uint64_t stack)
{
	struct vmctx_run_config cfg;
	pid_t pid;

	memset(&cfg, 0, sizeof(cfg));
	cfg.flags = VMCTX_FLAG_USERCODE | VMCTX_FLAG_EXIT_PROCESS |
		    VMCTX_FLAG_REDIRECT_SYSCALL | VMCTX_FLAG_WAIT_MONITOR |
		    extra_flags;
	cfg.entry = entry;
	cfg.stack = stack;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		exit(1);
	}
	if (pid == 0) {
		syscall(__NR_vmctx_run, &cfg);
		_exit(126);
	}
	for (int i = 0; i < 500; i++) {
		if (ctl(pid, VMCTX_CTL_ATTACH, NULL) == 0)
			return pid;
		usleep(1000);
	}
	fprintf(stderr, "vmmig: could not attach: %s\n", strerror(errno));
	kill(pid, SIGKILL);
	exit(1);
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr,
			"usage: %s run     <image> <n-beacons>\n"
			"       %s restore <image> <n-beacons>\n",
			argv[0], argv[0]);
		return 2;
	}
	{
		const char *mode = argv[1];
		const char *image = argv[2];
		unsigned long n = strtoul(argv[3], NULL, 0);
		pid_t pid;

		if (!strcmp(mode, "run")) {
			uint64_t *ctr = map_region();

			*ctr = 0;
			printf("vmmig: starting a fresh guest; will checkpoint after %lu beacons\n", n);
			pid = start_context(0, (uint64_t)(uintptr_t)&guest_loop,
					    REGION_ADDR + STACK_OFF);
			printf("vmmig: context pid %d\n", pid);
			return drive(pid, n, image);
		}

		if (!strcmp(mode, "restore")) {
			struct img_hdr h;
			static char buf[REGION_LEN];
			int fd = open(image, O_RDONLY);
			struct vmctx_uregs regs;

			if (fd < 0) {
				perror(image);
				return 1;
			}
			if (read(fd, &h, sizeof(h)) != sizeof(h) ||
			    h.magic != IMG_MAGIC) {
				fprintf(stderr, "vmmig: %s is not a checkpoint\n", image);
				return 1;
			}
			if (read(fd, buf, REGION_LEN) != (ssize_t)REGION_LEN) {
				fprintf(stderr, "vmmig: short image\n");
				return 1;
			}
			close(fd);

			printf("vmmig: restoring counter=%llu rip=0x%llx rsp=0x%llx\n",
			       (unsigned long long)h.counter,
			       (unsigned long long)h.regs.rip,
			       (unsigned long long)h.regs.rsp);

			/* Rebuild the guest's memory, then its registers. */
			memcpy(map_region(), buf, REGION_LEN);

			pid = start_context(VMCTX_FLAG_RESTORE, h.regs.rip,
					    h.regs.rsp);
			regs = h.regs;
			if (ctl(pid, VMCTX_CTL_SETREGS, &regs) < 0) {
				perror("SETREGS");
				kill(pid, SIGKILL);
				return 1;
			}
			{
				struct vmctx_reply go = { .action = VMCTX_ACT_SELF };

				/* Registers are in place: let the guest start. */
				ctl(pid, VMCTX_CTL_RESUME, &go);
			}
			printf("vmmig: context pid %d resumed from the checkpoint\n", pid);
			return drive(pid, n, NULL);
		}

		fprintf(stderr, "vmmig: unknown mode '%s'\n", mode);
		return 2;
	}
}
