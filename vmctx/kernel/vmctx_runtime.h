/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_VMCTX_RUNTIME_H
#define _UAPI_VMCTX_RUNTIME_H
#include <linux/types.h>

/* Local adapters measure execution CPU time and credit it to a source task.
 * Counts are cumulative nanoseconds, excluding descheduling and service waits.
 * An epoch identifies one executor task lifetime. Repeating a credit is safe;
 * changing its epoch or reversing its count is an error. */
#define VMCTX_CTL_RUNTIME 43
#define VMCTX_CTL_BOUNDARY 44
#define VMCTX_RUNTIME_ABI 1
#define VMCTX_RUNTIME_INFO 0
#define VMCTX_RUNTIME_READ 1
#define VMCTX_RUNTIME_CREDIT 2
#define VMCTX_RUNTIME_ARM 3
#define VMCTX_EV_RUNTIME 3
#define VMCTX_RUNTIME_QUANTUM_NS 10000000ULL
struct vmctx_runtime {
	__u32 version, size, op, reserved;
	__u64 epoch, total_ns, quantum_ns;
};
#endif
