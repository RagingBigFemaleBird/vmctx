/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_VMCTX_SOURCE_EXIT_H
#define _UAPI_VMCTX_SOURCE_EXIT_H
#include <linux/types.h>

/* Local source-adapter capability; no native exit ABI crosses the wire. */
#define VMCTX_CTL_SOURCE_EXIT_CAPS 39
#define VMCTX_SOURCE_EXIT_ABI 1U
#define VMCTX_SOURCE_EXIT_NATIVE_MM_RELEASE (1ULL << 0)
struct vmctx_source_exit_caps {
	__u32 version, size;
	__u64 features;
};
#endif
