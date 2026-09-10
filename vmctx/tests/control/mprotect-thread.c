// SPDX-License-Identifier: GPL-2.0
/* Permission changes must reach a sibling whose PTE is already writable,
 * including a sibling that makes no system calls while it waits. */
#define _GNU_SOURCE
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

static volatile unsigned char *page;
static atomic_int phase, completed;
static _Thread_local sigjmp_buf checkpoint;
static _Thread_local volatile sig_atomic_t expected_fault;
static volatile sig_atomic_t write_fault, read_fault;

static void denied(int sig, siginfo_t *info, void *context)
{
	(void)context;
	if (sig != SIGSEGV || info->si_code != SEGV_ACCERR ||
	    info->si_addr != (void *)page || !expected_fault)
		_exit(92);
	if (expected_fault == 1) write_fault = 1;
	else read_fault = 1;
	siglongjmp(checkpoint, 1);
}

static void *worker(void *unused)
{
	(void)unused;
	page[0] = 0x51; /* Install this sibling's writable translation. */
	atomic_store(&completed, 1);
	while (atomic_load(&phase) < 1) asm volatile("pause");
	expected_fault = 1;
	if (!sigsetjmp(checkpoint, 1)) page[0] = 0x63;
	expected_fault = 0;
	atomic_store(&completed, 2);
	while (atomic_load(&phase) < 2) asm volatile("pause");
	expected_fault = 2;
	if (!sigsetjmp(checkpoint, 1)) (void)page[0];
	expected_fault = 0;
	atomic_store(&completed, 3);
	while (atomic_load(&phase) < 3) asm volatile("pause");
	page[0] = 0x75;
	return NULL;
}

int main(void)
{
	long size = sysconf(_SC_PAGESIZE);
	struct sigaction action = {.sa_sigaction = denied, .sa_flags = SA_SIGINFO};
	pthread_t thread;
	if (size <= 0 || sigemptyset(&action.sa_mask) || sigaction(SIGSEGV, &action, NULL)) return 2;
	page = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED || pthread_create(&thread, NULL, worker, NULL)) return 2;
	while (atomic_load(&completed) < 1) asm volatile("pause");
	if (mprotect((void *)page, size, PROT_READ)) return 2;
	atomic_store(&phase, 1);
	while (atomic_load(&completed) < 2) asm volatile("pause");
	if (mprotect((void *)page, size, PROT_NONE)) return 2;
	atomic_store(&phase, 2);
	while (atomic_load(&completed) < 3) asm volatile("pause");
	if (mprotect((void *)page, size, PROT_READ | PROT_WRITE)) return 2;
	atomic_store(&phase, 3);
	if (pthread_join(thread, NULL)) return 2;
	/* A joined thread remains in protocol history, but no longer owns a
	 * translation that a subsequent permission change must update. */
	if (mprotect((void *)page, size, PROT_READ) ||
	    mprotect((void *)page, size, PROT_READ | PROT_WRITE)) return 2;
	int pass = write_fault && read_fault && page[0] == 0x75;
	printf("%s: sibling permissions write_fault=%d read_fault=%d restored=%d\n",
		pass ? "PASS" : "FAIL", write_fault, read_fault, page[0] == 0x75);
	if (munmap((void *)page, size)) return 2;
	return pass ? 0 : 1;
}
