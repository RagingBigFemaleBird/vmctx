/* SPDX-License-Identifier: GPL-2.0 */
#ifndef VMCTX_CPU_WIRE_H
#define VMCTX_CPU_WIRE_H
#include <errno.h>
#include <stddef.h>
#include <string.h>
#include "vmrproto.h"

/* The native adapter accepts a fixed-capacity buffer. Only its architectural
 * prefix is transmitted; unused capacity is never state. Component records
 * remain complete and are validated against the negotiated CPU by the adapter.
 */
static inline size_t vmr_cpu_wire_size(const struct vmr_cpu_state *cpu)
{
	if (cpu->reserved || cpu->xstate_size < 576 ||
	    cpu->xstate_size > VMR_XSTATE_MAX) {
		errno = EPROTO;
		return 0;
	}
	return offsetof(struct vmr_cpu_state, xstate) + cpu->xstate_size;
}

/* cpu has full native capacity, but only len bytes have been received. Check
 * framing before inspecting any field, then clear untransmitted native padding.
 * Failure must never reach SETCPU. In particular an oversized home_call reply
 * may have been copied only up to the receiver's capacity.
 */
static inline int vmr_cpu_wire_decode(struct vmr_cpu_state *cpu, size_t len)
{
	if (len < offsetof(struct vmr_cpu_state, xstate) || len > sizeof(*cpu) ||
	    len != vmr_cpu_wire_size(cpu)) {
		errno = EPROTO;
		return -1;
	}
	memset((unsigned char *)cpu + len, 0, sizeof(*cpu) - len);
	return 0;
}
#endif
