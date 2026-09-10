// SPDX-License-Identifier: GPL-2.0
/* Guard PTEs deny access even though the VMA remains readable/writable.
 * cc -O2 -Wall -Wextra -static guard-page.c -o guard-page
 */
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MADV_GUARD_INSTALL
#define MADV_GUARD_INSTALL 102
#define MADV_GUARD_REMOVE 103
#endif

static sigjmp_buf checkpoint;
static volatile sig_atomic_t phase, read_code, write_code;
static volatile unsigned char *mapping;
static volatile unsigned char observed;

static void denied(int sig, siginfo_t *info, void *context)
{
	(void)context;
	if (sig != SIGSEGV || info->si_code <= 0 ||
	    info->si_addr != (void *)mapping || (phase != 1 && phase != 2))
		_exit(92);
	if (phase == 1)
		read_code = info->si_code;
	else
		write_code = info->si_code;
	siglongjmp(checkpoint, 1);
}

int main(void)
{
	long page = sysconf(_SC_PAGESIZE);
	struct sigaction action = {.sa_sigaction = denied, .sa_flags = SA_SIGINFO};
	if (page <= 0 || sigemptyset(&action.sa_mask) ||
	    sigaction(SIGSEGV, &action, NULL))
		return 2;
	mapping = mmap(NULL, page, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mapping == MAP_FAILED)
		return 2;
	mapping[0] = 0x5a;
	if (madvise((void *)mapping, page, MADV_GUARD_INSTALL)) {
		perror("MADV_GUARD_INSTALL");
		return 2;
	}
	phase = 1;
	if (!sigsetjmp(checkpoint, 1))
		observed = mapping[0];
	phase = 2;
	if (!sigsetjmp(checkpoint, 1))
		mapping[0] = 0xa5;
	phase = 0;
	if (madvise((void *)mapping, page, MADV_GUARD_REMOVE)) {
		perror("MADV_GUARD_REMOVE");
		return 2;
	}
	int zero = mapping[0] == 0;
	mapping[0] = 0xc3;
	int writable = mapping[0] == 0xc3;
	if (munmap((void *)mapping, page))
		return 2;
	int pass = read_code > 0 && write_code > 0 && zero && writable;
	printf("%s guard read_code=%d write_code=%d removal_zero=%d writable=%d\n",
	       pass ? "PASS" : "FAIL", read_code, write_code, zero, writable);
	return pass ? 0 : 1;
}
