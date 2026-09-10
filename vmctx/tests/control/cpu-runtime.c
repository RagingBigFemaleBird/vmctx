// SPDX-License-Identifier: GPL-2.0
/* A CPU clock measures execution of this task, including on another host.
 * Keep the compute interval free of calls so syscall servicing cannot stand
 * in for charging the work. Sleeping must not be charged as execution. */
#define _GNU_SOURCE
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static volatile uint64_t sink;

static uint64_t ns(clockid_t id)
{
	struct timespec t;
	if (clock_gettime(id, &t)) return UINT64_MAX;
	return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}

int main(void)
{
	uint64_t wall0 = ns(CLOCK_MONOTONIC);
	uint64_t thread0 = ns(CLOCK_THREAD_CPUTIME_ID);
	uint64_t process0 = ns(CLOCK_PROCESS_CPUTIME_ID);
	uint64_t x = 0x1827364554637281ULL;
	for (uint64_t i = 0; i < 50000000; i++) {
		x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
		x *= 0x2545f4914f6cdd1dULL;
	}
	sink = x;
	uint64_t thread1 = ns(CLOCK_THREAD_CPUTIME_ID);
	uint64_t process1 = ns(CLOCK_PROCESS_CPUTIME_ID);
	uint64_t wall1 = ns(CLOCK_MONOTONIC);
	struct timespec sleep = {.tv_nsec = 200000000};
	if (nanosleep(&sleep, NULL)) return 2;
	uint64_t thread2 = ns(CLOCK_THREAD_CPUTIME_ID);
	uint64_t process2 = ns(CLOCK_PROCESS_CPUTIME_ID);
	uint64_t wall2 = ns(CLOCK_MONOTONIC);
	int valid = wall0 != UINT64_MAX && wall1 != UINT64_MAX && wall2 != UINT64_MAX &&
		thread0 != UINT64_MAX && thread1 != UINT64_MAX && thread2 != UINT64_MAX &&
		process0 != UINT64_MAX && process1 != UINT64_MAX && process2 != UINT64_MAX &&
		wall1 > wall0 && wall2 > wall1 && thread1 >= thread0 && thread2 >= thread1 &&
		process1 >= process0 && process2 >= process1;
	uint64_t elapsed = wall1 - wall0, tcpu = thread1 - thread0, pcpu = process1 - process0;
	/* Deliberately broad tolerance for descheduling and transport overhead.
	 * Missing remote execution is orders of magnitude below this floor. */
	int ok = valid && elapsed > 1000000 && tcpu > elapsed / 20 && pcpu > elapsed / 20 &&
		thread2 - thread1 < (wall2 - wall1) / 2 && process2 - process1 < (wall2 - wall1) / 2;
	printf("%s: CPU runtime compute wall=%" PRIu64 " thread=%" PRIu64 " process=%" PRIu64
		" ns; sleep wall=%" PRIu64 " thread=%" PRIu64 " process=%" PRIu64 " ns; sink=%" PRIx64 "\n",
		ok ? "PASS" : "FAIL", elapsed, tcpu, pcpu, wall2 - wall1,
		thread2 - thread1, process2 - process1, sink);
	return ok ? 0 : 1;
}
