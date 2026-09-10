/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* Source-local memory control envelope. No native descriptor, command number
 * or argument pointer belongs on the source/executor wire. */
#ifndef VMCTX_MEMORY_H
#define VMCTX_MEMORY_H
#include <linux/types.h>
#define VMCTX_CTL_MEMORY 48
#define VMCTX_MEMORY_ABI 1
#define VMCTX_MEMORY_CAPS 0
#define VMCTX_MEMORY_CALL 1
#define VMCTX_MEMORY_EXPECTED_MM 1U
#define VMCTX_MEMORY_RETAINED_METADATA 2U
struct vmctx_memory {
	__u32 version,size,op,command;
	__u64 mm_identity,argument;
	__u32 features,reserved;
};
/* CAPS uses selector zero and zero fields except version/size/op. CALL uses
 * a monitor-owned source context selector, nonzero expected MM identity and
 * the inner control's argument pointer. It returns the inner control's result.
 * Before any inner argument is consumed, the kernel captures the task's MM,
 * verifies its exact identity, and pins its metadata for the entire operation.
 * Exec after capture cannot redirect an operation into the replacement MM.
 * -ESTALE means the captured MM did not match; no inner operation took place.
 * RECALL BEGIN transfers its own pin to the returned ticket; COMMIT/CANCEL
 * continue to use their exact existing ticket with ordinary selector zero.
 * Source CPU, execution, and lifecycle controls cannot be wrapped here. */
#endif
