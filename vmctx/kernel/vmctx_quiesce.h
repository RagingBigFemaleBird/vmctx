/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_VMCTX_QUIESCE_H
#define _UAPI_VMCTX_QUIESCE_H
#include <linux/types.h>

/* Local executor assist. A lease prevents hardware entry until released.
 * BEGIN returns only after prior hardware entries have completed. The owner
 * is the calling monitor task; owner exit releases every outstanding lease. */
#define VMCTX_CTL_QUIESCE 41
#define VMCTX_CTL_PROTECT_BACKING 42
#define VMCTX_QUIESCE_ABI 2
/* ABI 2 also guarantees PROTECT_BACKING(vmctx_mem): address is the offset
 * in the target's private backing object, len is PAGE_SIZE, buf must be zero.
 * The caller must own an execution lease on that target MM. No VMA is needed.
 * Returns PAGE_SIZE when a folio was protected, zero for absent/unmapped. */
#define VMCTX_QUIESCE_INFO 0
#define VMCTX_QUIESCE_BEGIN 1
#define VMCTX_QUIESCE_END 2
struct vmctx_quiesce {
	__u32 version, size, op, reserved;
	__u64 token, mm_id;
};
#endif
