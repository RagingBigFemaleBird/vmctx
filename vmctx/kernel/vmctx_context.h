/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_VMCTX_CONTEXT_H
#define _UAPI_VMCTX_CONTEXT_H
#include <linux/types.h>

/* Native descriptors retain one exact task/context lifetime. OPEN uses
 * a native PID once. Subsequent ctl calls select the descriptor as -(fd + 1).
 * The descriptor is usable only by threads of its opening monitor process.
 * INFO and gate QUERY remain available after task teardown. CHILD exports the
 * committed birth for a gate ticket, including after parent/child death.
 * Live operations recheck native ptrace permissions. After teardown the owned
 * descriptor grants access to the retained record without a new ptrace attach;
 * state/memory operations and gate mutations fail with ESRCH.
 * A failed copyout installs no descriptor. Each successful OPEN/CHILD yields
 * an independently owned close-on-exec descriptor; the caller must close it. */
#define VMCTX_CTL_CONTEXT 46
#define VMCTX_CONTEXT_ABI 2
#define VMCTX_CONTEXT_OPEN 1
#define VMCTX_CONTEXT_INFO 2
#define VMCTX_CONTEXT_CHILD 3
#define VMCTX_CONTEXT_MMINFO 4
#define VMCTX_CONTEXT_SIGNAL 5
#define VMCTX_CONTEXT_CAPS 6
/* CAPS flags, distinct from INFO/MMINFO result flags below. */
#define VMCTX_CONTEXT_CAP_REFERENCE 1
#define VMCTX_CONTEXT_CAP_SIGNAL 2
#define VMCTX_CONTEXT_CAP_MMINFO 4
#define VMCTX_CONTEXT_CAP_SOURCE 8
#define VMCTX_CONTEXT_CAP_EXECUTOR 16
#define VMCTX_CONTEXT_READY 1
#define VMCTX_CONTEXT_ENDED 2
#define VMCTX_CONTEXT_MM_LIVE 4
#define VMCTX_CONTEXT_MM_ENDED 8
/* ABI 2: OPEN/INFO/CHILD return native_pid/native_tgid in the calling
 * monitor's PID namespace. They are current lookup candidates, never lifetime
 * identities; either can become stale immediately. ABI 1 keeps its original
 * saved global native_pid and zero reserved field.
 *
 * MMINFO uses selector 0, requires CAP_SYS_PTRACE in the initial user namespace,
 * and takes only mm_identity. It reports whether that non-reused MM record
 * still has native context/operation references. A missing identity that was
 * previously allocated is ENDED; zero or a never-allocated identity is invalid.
 * Disabling native MM reference accounting makes this query unsupported.
 *
 * SIGNAL requires a retained descriptor and takes a native signal number in
 * exit_status (including zero for permission/liveness checking). It targets
 * that exact task, with the normal native signal-group consequences, and
 * fails ESRCH after context teardown. No PID lookup follows the descriptor. */
/* CAPS uses selector zero and all-zero input fields except version/size/op.
 * SOURCE advertises OPEN/INFO/CHILD for native service contexts; EXECUTOR
 * advertises OPEN/INFO for execution contexts. CHILD is source-only. INFO's
 * READY bit is source-only; execution availability is established by its
 * native state controls. ENDED always describes this context's lifetime.
 * exit_status is a native wait status only for an ended SOURCE context;
 * execution contexts return zero here (use native wait for task termination).
 * MM identities on the two machines are local to each kernel, not wire IDs. */
struct vmctx_context {
	__u32 version, size, op, flags;
	__u64 ticket, identity, mm_identity;
	__s32 fd;
	__u32 native_pid, exit_status;
	union { __u32 reserved; __u32 native_tgid; };
};
#endif
