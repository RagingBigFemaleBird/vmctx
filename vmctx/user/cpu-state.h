/* SPDX-License-Identifier: GPL-2.0 */
#ifndef VMCTX_CPU_STATE_H
#define VMCTX_CPU_STATE_H
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "cpu-wire.h"
#include "vmctx_runtime.h"

#define VMCTX_CTL_GETCPU 29
#define VMCTX_CTL_SETCPU 30
#define VMCTX_CTL_EXCEPTION 31
#define VMCTX_CTL_SOURCE_EXEC 32
/* EAGAIN is a scheduler transition, never evidence that state is dispensable. */
#ifndef VMCTX_CPU_CONTEXT
#define VMCTX_CPU_CONTEXT pid_t
#endif
static int cpu_state_ctl(VMCTX_CPU_CONTEXT pid, int op, void *state)
{
	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		if (ctl(pid, op, state) == 0)
			return 0;
		if (errno != EAGAIN)
			return -1;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - start.tv_sec >= 5) {
			errno = ETIMEDOUT;
			return -1;
		}
		usleep(50);
	}
}
#endif
