/* SPDX-License-Identifier: GPL-2.0 */
#ifndef VMCTX_MAP_TRACE_H
#define VMCTX_MAP_TRACE_H
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>

/* Diagnostic only: no guest reads, page faults, or ordering decisions. Clock
 * values order records on one host only; they are not a distributed clock. */
static int map_trace_enabled;
static inline void map_trace_init(void)
{
	const char *value = getenv("VMCTX_MAP_TRACE");
	map_trace_enabled = value && *value && *value != '0';
}
static inline __attribute__((format(printf,2,3)))
void map_trace(const char *side, const char *fmt, ...)
{
	struct timespec ts;
	va_list ap;
	if (!map_trace_enabled)
		return;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	flockfile(stderr);
	fprintf(stderr, "[maptrace %s %lld.%09ld worker=%lx] ", side,
		(long long)ts.tv_sec, ts.tv_nsec, (unsigned long)pthread_self());
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	funlockfile(stderr);
}
#endif
