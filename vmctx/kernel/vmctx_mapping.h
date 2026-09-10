/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_VMCTX_MAPPING_H
#define _UAPI_VMCTX_MAPPING_H
#include <linux/types.h>

/* Source-adapter operation, never a wire descriptor. Query an actual VMA
 * without injecting metadata syscalls or buffers into the program. On success
 * fd is a CLOEXEC reference in the monitor's file table, or -1 for anonymous
 * memory. The caller owns that reference even after the mapping disappears. */
#define VMCTX_CTL_MAPPING 36
#define VMCTX_MAPPING_SHARED 1U
struct vmctx_mapping {
	__u64 addr;                  /* input: address within the mapping */
	__u64 start, len, offset;    /* output: VMA and underlying file offset */
	__u32 prot, flags;           /* output: source PROT_* and mapping flags */
	__s32 fd;                   /* output: monitor-owned file reference */
	__u32 reserved;             /* input: zero */
};
#endif
