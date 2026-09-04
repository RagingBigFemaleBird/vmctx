/* SPDX-License-Identifier: GPL-2.0 */
/* Shared helpers for the vmctx tools. */
#ifndef _VMCTX_TOOL_COMMON_H
#define _VMCTX_TOOL_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "vmctx_uapi.h"

#define VMCTX_MAYBE_UNUSED __attribute__((unused))

static VMCTX_MAYBE_UNUSED const char *vendor_name(unsigned v)
{
	switch (v) {
	case VMCTX_VENDOR_INTEL: return "Intel VT-x (VMX)";
	case VMCTX_VENDOR_AMD:   return "AMD-V (SVM)";
	default:                 return "none";
	}
}

static VMCTX_MAYBE_UNUSED void print_caps(unsigned c)
{
	printf("  caps        : 0x%x%s%s%s%s%s\n", c,
	       (c & VMCTX_CAP_PRESENT)      ? " present"      : "",
	       (c & VMCTX_CAP_ENABLED)      ? " enabled"      : "",
	       (c & VMCTX_CAP_EPT_NPT)      ? " ept/npt"      : "",
	       (c & VMCTX_CAP_UNRESTRICTED) ? " unrestricted" : "",
	       (c & VMCTX_CAP_BROUGHT_UP)   ? " brought-up"   : "");
}

static VMCTX_MAYBE_UNUSED const char *state_name(unsigned s)
{
	switch (s) {
	case VMCTX_STATE_CREATED:   return "created";
	case VMCTX_STATE_BROUGHTUP: return "brought-up (root op verified)";
	case VMCTX_STATE_ENTERED:   return "entered";
	case VMCTX_STATE_EXITED:    return "exited";
	case VMCTX_STATE_DEAD:      return "dead";
	default:                    return "?";
	}
}

static VMCTX_MAYBE_UNUSED int vmctx_open(void)
{
	int fd = open(VMCTX_DEV_PATH, O_RDWR);
	if (fd < 0)
		fprintf(stderr, "open %s: %s\n", VMCTX_DEV_PATH, strerror(errno));
	return fd;
}

#endif
