/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_VMCTX_SYSCALL_GATE_H
#define _UAPI_VMCTX_SYSCALL_GATE_H
#include <linux/types.h>

/* Source-local ABI. Include after vmctx_syscall. BEGIN borrows the installed
 * architectural frame, performs native entry work, and parks before dispatch.
 * Only COMMIT may dispatch. QUERY never changes state. All operations are
 * nonblocking; ptrace stops therefore have no artificial syscall deadline.
 *
 * Tickets start at one and increase exactly once per call. Repeating BEGIN or
 * COMMIT with the same ticket reads the existing state without executing again,
 * including after a failed result copy. Earlier tickets return ESTALE. Register
 * writes and legacy assisted calls are excluded until completion. */
#define VMCTX_CTL_SYSCALL_GATE 45
#define VMCTX_SYSCALL_GATE_ABI 2
#define VMCTX_GATE_BEGIN 1
#define VMCTX_GATE_QUERY 2
#define VMCTX_GATE_COMMIT 3
#define VMCTX_GATE_CANCEL 4
#define VMCTX_GATE_ENTERING 1
#define VMCTX_GATE_ADMITTED 2
#define VMCTX_GATE_RUNNING 3
#define VMCTX_GATE_COMPLETE 4
struct vmctx_syscall_gate {
	__u32 version, size, op, state;
	__u64 ticket;
	/* A native child may exist before its parent's call returns (vfork or
	 * a ptrace fork stop). These identify committed child creation. */
	__u64 child_host_pid, child_pid;
	__u32 child_shared_mm, reserved;
	/* At ADMITTED: actual entry-work result in nr (-1 means skip), arguments
	 * from the canonical frame. At COMPLETE: final result and register set.
	 * Inputs are ignored; no register or argument rewrite follows admission. */
	struct vmctx_syscall call;
	/* COMPLETE only: dispatch result before tracing/restart/signal work.
	 * nr == -1 denotes a skipped call, without native side effects. */
	__s64 dispatch_ret;
};
#endif
