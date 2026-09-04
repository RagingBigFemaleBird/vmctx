/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The budgets nest, and the outermost is the kernel's fault deadline
 * (vmctx_fault_deadline_ms, a live core_param): a guest fault the monitor has
 * not answered by then is killed with SIGBUS. Everything inside it must
 * finish first: the source's page GET socket (4 s) around the destination's
 * GET handler (pull_wait 500 + claim wait 1000 + take wait 1000 = 2.5 s worst
 * case), and the source's own 1 s take hold under that. At 2000 ms the
 * outermost budget was SMALLER than the middle one, and under pool load
 * (8-way, load 60 on 16 cores) a GET that merely ran long ended the context
 * -- "remote GET of ... after 2009ms" then "the page channel failed and did
 * not recover", exit 13, 2 of 256 hx2 runs (session 26). Each tool raises
 * the deadline to what its nesting needs at start-up, so the relation holds
 * whatever kernel default it meets; the kernel's own default is the same
 * number (patch 0070 / 0040).
 *
 * Its own header: vmhome.c carries private copies of the kernel structs and
 * cannot include common.h's vmctx_uapi.h.
 */
#ifndef _VMCTX_TOOL_DEADLINE_H
#define _VMCTX_TOOL_DEADLINE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static inline void vmctx_deadline_enforce(const char *who, unsigned want_ms)
{
	const char *path = "/sys/module/kernel/parameters/vmctx_fault_deadline_ms";
	char buf[32];
	unsigned have = 0;
	FILE *f = fopen(path, "r");

	if (!f)
		return;			/* a kernel without the parameter */
	if (fgets(buf, sizeof(buf), f))
		have = (unsigned)strtoul(buf, NULL, 10);
	fclose(f);
	if (have == 0 || have >= want_ms)
		return;			/* off (never), or already wide enough */
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%u\n", want_ms);
		fclose(f);
		fprintf(stderr, "[%s] the kernel's fault deadline was %u ms, "
			"inside this tool's own page-channel budgets; raised to "
			"%u ms so the budgets nest\n", who, have, want_ms);
	} else {
		fprintf(stderr, "[%s] the kernel's fault deadline is %u ms and "
			"could not be raised to %u ms: %s -- a slow page GET "
			"under load can end a context\n", who, have, want_ms,
			strerror(errno));
	}
}

#endif
