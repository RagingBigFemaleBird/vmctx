// SPDX-License-Identifier: GPL-2.0
/*
 * vmmon — monitor a VM context from another process (avm.md objectives 2/3).
 *
 *   vmmon [-e] <command> [args...]
 *
 * Forks a child that becomes a VM context with its syscalls and faults
 * REDIRECTED to this process. The monitor then loops:
 *
 *   vmctx_ctl(pid, WAIT)     -> next guest event (syscall number + args, or
 *                               fault address + error code, plus RIP/RSP)
 *   ... inspect: GETREGS / PEEK the context's memory ...
 *   vmctx_ctl(pid, RESUME)   -> SELF (let the kernel handle it), DONE (the
 *                               monitor handled it; supply the return value),
 *                               or KILL.
 *
 * This demonstrates that a context's faults and syscalls can be serviced by a
 * different process entirely, with the tools to read/write its registers and
 * memory — which is also the mechanism remote execution needs: a monitor is
 * free to forward an event over the network instead of answering it locally.
 *
 * Default behaviour is to trace and let the kernel handle everything. With -e
 * the monitor *emulates* write(2) to stdout/stderr itself, by PEEKing the
 * guest's buffer out of its address space and writing it from here — proof
 * that the servicing really happens in this process.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
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
#define VMCTX_FLAG_REDIRECT_FAULT    8u
#define VMCTX_FLAG_WAIT_MONITOR      16u

#define VMCTX_CTL_ATTACH  1
#define VMCTX_CTL_DETACH  2
#define VMCTX_CTL_WAIT    3
#define VMCTX_CTL_RESUME  4
#define VMCTX_CTL_GETREGS 5
#define VMCTX_CTL_SETREGS 6
#define VMCTX_CTL_PEEK    7
#define VMCTX_CTL_POKE    8

#define VMCTX_EV_NONE    0
#define VMCTX_EV_SYSCALL 1
#define VMCTX_EV_FAULT   2

#define VMCTX_ACT_SELF 0
#define VMCTX_ACT_DONE 1
#define VMCTX_ACT_KILL 2

/* Must match <uapi/linux/vmctx.h>. */
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

/* --- guest side: exec the command so the program runs in the context --- */
static char  guest_stack[262144] __attribute__((aligned(4096)));
static char *g_path;
static char **g_argv, **g_envp;

static inline long gsys(long nr, long a, long b, long c)
{
	long ret;
	__asm__ volatile("syscall" : "=a"(ret)
			 : "a"(nr), "D"(a), "S"(b), "d"(c)
			 : "rcx", "r11", "memory");
	return ret;
}

static void guest_exec(void)
{
	gsys(59 /*execve*/, (long)g_path, (long)g_argv, (long)g_envp);
	gsys(1 /*write*/, 2, (long)"vmmon: exec in guest failed\n", 28);
	gsys(231 /*exit_group*/, 127, 0, 0);
}

/*
 * Read len bytes from the context's address space at addr and write them to fd
 * from this process. Returns bytes written, or -1 on failure.
 */
static long peek_and_write(pid_t pid, int fd, uint64_t addr, uint64_t len)
{
	char buf[4096];
	uint64_t done = 0;

	while (done < len) {
		uint64_t chunk = len - done;
		struct vmctx_mem m;
		long got;
		ssize_t w;

		if (chunk > sizeof(buf))
			chunk = sizeof(buf);
		m.addr = addr + done;
		m.len  = chunk;
		m.buf  = (uint64_t)(void *)buf;
		got = ctl(pid, VMCTX_CTL_PEEK, &m);
		if (got <= 0)
			return done ? (long)done : -1;
		w = write(fd, buf, got);
		if (w < 0)
			return done ? (long)done : -1;
		done += w;
		if (w < got)
			break;
	}
	return (long)done;
}

static const char *sysname(uint64_t nr)
{
	switch (nr) {
	case 0:   return "read";      case 1:   return "write";
	case 2:   return "open";      case 3:   return "close";
	case 9:   return "mmap";      case 10:  return "mprotect";
	case 11:  return "munmap";    case 12:  return "brk";
	case 20:  return "writev";    case 39:  return "getpid";
	case 56:  return "clone";     case 57:  return "fork";
	case 59:  return "execve";    case 60:  return "exit";
	case 61:  return "wait4";     case 63:  return "uname";
	case 158: return "arch_prctl";case 231: return "exit_group";
	case 257: return "openat";    case 262: return "newfstatat";
	case 217: return "getdents64";
	default:  return "";
	}
}

int main(int argc, char **argv, char **envp)
{
	struct vmctx_run_config cfg;
	int emulate = 0, argi = 1;
	unsigned long n_sys = 0, n_flt = 0, n_emul = 0;
	pid_t pid;
	int st = 0;

	if (argc > 1 && !strcmp(argv[1], "-e")) {
		emulate = 1;
		argi = 2;
	}
	if (argc <= argi) {
		fprintf(stderr, "usage: %s [-e] <command> [args...]\n", argv[0]);
		return 2;
	}

	/* Resolve the binary here; execve(2) does not search PATH. */
	static char resolved[4096];
	g_path = argv[argi];
	if (!strchr(g_path, '/')) {
		const char *p = getenv("PATH");
		if (!p)
			p = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
		while (*p) {
			const char *e = strchr(p, ':');
			size_t l = e ? (size_t)(e - p) : strlen(p);

			if (l && l + 1 + strlen(argv[argi]) + 1 <= sizeof(resolved)) {
				memcpy(resolved, p, l);
				resolved[l] = '/';
				strcpy(resolved + l + 1, argv[argi]);
				if (access(resolved, X_OK) == 0) {
					g_path = resolved;
					break;
				}
			}
			p = e ? e + 1 : p + l;
		}
	}
	g_argv = &argv[argi];
	g_envp = envp;

	memset(&cfg, 0, sizeof(cfg));
	cfg.flags = VMCTX_FLAG_USERCODE | VMCTX_FLAG_EXIT_PROCESS |
		    VMCTX_FLAG_REDIRECT_SYSCALL | VMCTX_FLAG_REDIRECT_FAULT |
		    VMCTX_FLAG_WAIT_MONITOR;   /* don't start before we attach */
	cfg.entry = (uint64_t)(void *)&guest_exec;
	cfg.stack = (uint64_t)(guest_stack + sizeof(guest_stack) - 256);
	memset(guest_stack, 0, sizeof(guest_stack));

	printf("[vmmon] monitoring '%s' (%s)\n", g_path,
	       emulate ? "emulating write() in the monitor" : "tracing only");
	fflush(stdout);

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return 1;
	}
	if (pid == 0) {
		syscall(__NR_vmctx_run, &cfg);
		_exit(126);   /* only reached if the context could not start */
	}

	/* Attach as the monitor. The child may need a moment to become a
	 * context, so retry briefly. */
	for (int i = 0; i < 200; i++) {
		if (ctl(pid, VMCTX_CTL_ATTACH, NULL) == 0)
			goto attached;
		usleep(1000);
	}
	fprintf(stderr, "[vmmon] could not attach: %s\n", strerror(errno));
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	return 1;
attached:
	printf("[vmmon] attached to context pid %d\n", pid);
	fflush(stdout);

	for (;;) {
		struct vmctx_event ev;
		struct vmctx_reply rep = { .action = VMCTX_ACT_SELF };

		if (ctl(pid, VMCTX_CTL_WAIT, &ev) < 0) {
			if (errno == EINTR)
				continue;
			break;    /* context gone (ESRCH/EINVAL) */
		}

		if (ev.type == VMCTX_EV_SYSCALL) {
			n_sys++;
			/*
			 * Service the guest's terminal output in this process:
			 * pull the bytes out of the context's address space with
			 * PEEK and write them from here. Both write(2) and
			 * writev(2) are covered — libc uses either.
			 */
			if (emulate && (ev.nr == 1 || ev.nr == 20) &&
			    (ev.args[0] == 1 || ev.args[0] == 2)) {
				long total = 0;
				int fd = (int)ev.args[0];

				if (ev.nr == 1) {
					total = peek_and_write(pid, fd, ev.args[1], ev.args[2]);
				} else {
					/* writev: fetch the iovec array first. */
					struct { uint64_t base, len; } iov[16];
					uint64_t n = ev.args[2] < 16 ? ev.args[2] : 16;
					struct vmctx_mem m = {
						.addr = ev.args[1],
						.len  = n * sizeof(iov[0]),
						.buf  = (uint64_t)(void *)iov,
					};

					if (n && ctl(pid, VMCTX_CTL_PEEK, &m) > 0) {
						for (uint64_t i = 0; i < n; i++) {
							long w;

							if (!iov[i].len)
								continue;
							w = peek_and_write(pid, fd, iov[i].base,
									   iov[i].len);
							if (w < 0) {
								total = w;
								break;
							}
							total += w;
						}
					} else {
						total = -1;
					}
				}
				if (total >= 0) {
					rep.action = VMCTX_ACT_DONE;
					rep.retval = (uint64_t)total;
					n_emul++;
				}
			}
			if (n_sys <= 12 || (n_sys % 50) == 0)
				fprintf(stderr, "[vmmon] syscall %-3llu %-11s rip=0x%llx%s\n",
					(unsigned long long)ev.nr, sysname(ev.nr),
					(unsigned long long)ev.rip,
					rep.action == VMCTX_ACT_DONE ? "  [handled by monitor]" : "");
		} else if (ev.type == VMCTX_EV_FAULT) {
			n_flt++;
			if (n_flt <= 5)
				fprintf(stderr, "[vmmon] fault at 0x%llx err 0x%llx rip=0x%llx\n",
					(unsigned long long)ev.fault_addr,
					(unsigned long long)ev.fault_err,
					(unsigned long long)ev.rip);
		}

		if (ctl(pid, VMCTX_CTL_RESUME, &rep) < 0)
			break;
	}

	waitpid(pid, &st, 0);
	printf("[vmmon] context exited: %d ; events: %lu syscalls, %lu faults",
	       WIFEXITED(st) ? WEXITSTATUS(st) : -1, n_sys, n_flt);
	if (emulate)
		printf(", %lu writes serviced by the monitor", n_emul);
	printf("\n");
	return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}
