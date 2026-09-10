/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_VMCTX_ACCESS_H
#define _UAPI_VMCTX_ACCESS_H
#include <linux/types.h>

/* Source-local committed access journal. Native PROT_* values are translated
 * by the source adapter; this structure is never sent to an executor. Reads
 * are repeatable until ACK. Identity is an mm lifetime, not a PID or pointer.
 * An overflow/allocation failure is terminal for this journal, never a hole
 * that may be acknowledged and ignored. INFO with pid=0 queries the ABI. */
#define VMCTX_CTL_ACCESS_LOG 40
#define VMCTX_ACCESS_ABI 1
#define VMCTX_ACCESS_INFO 0
#define VMCTX_ACCESS_READ 1
#define VMCTX_ACCESS_ACK 2
#define VMCTX_ACCESS_MAX 16
#define VMCTX_ACCESS_UNMAP 1
#define VMCTX_ACCESS_DISCARD 2
#define VMCTX_ACCESS_PROTECT 3
#define VMCTX_ACCESS_DENY 4
#define VMCTX_ACCESS_ALLOW 5
#define VMCTX_ACCESS_CONSTRUCT 6
struct vmctx_access_event {
	__u64 seq, start, end;
	__u32 kind, prot;
};
struct vmctx_access_log {
	__u32 version, size, op, n;
	__u64 mm_id, cursor, head, acked, construction;
	__u32 error, reserved;
	struct vmctx_access_event event[VMCTX_ACCESS_MAX];
};
#endif
