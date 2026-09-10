/* SPDX-License-Identifier: GPL-2.0 */
/* Linux source adapter. The caller captures an explicit MM identity at the
 * operation boundary and retains it through callbacks and replies. A failed
 * call never refreshes that identity or retries on a context's replacement MM.
 * The native envelope pins the exact MM and metadata before reading arguments.
 * No descriptor, native command or argument pointer crosses the wire. */
#ifndef VMR_SOURCE_MEMORY_H
#define VMR_SOURCE_MEMORY_H
#include "source-context.h"
#include "vmctx_memory.h"

static inline int source_memory_capabilities(void)
{
	struct vmctx_memory q={.version=VMCTX_MEMORY_ABI,.size=sizeof(q),
		.op=VMCTX_MEMORY_CAPS};
	if(source_control_raw(0,VMCTX_CTL_MEMORY,&q))return -1;
	if(q.version!=VMCTX_MEMORY_ABI || q.size!=sizeof(q) ||
	   q.op!=VMCTX_MEMORY_CAPS || q.command || q.mm_identity || q.argument ||
	   q.reserved || (q.features&(VMCTX_MEMORY_EXPECTED_MM|
		VMCTX_MEMORY_RETAINED_METADATA))!=(VMCTX_MEMORY_EXPECTED_MM|
		VMCTX_MEMORY_RETAINED_METADATA)) {errno=EPROTO;return -1;}
	return 0;
}

static inline long source_memory_call(const struct source_context *context,
		uint64_t expected_mm,unsigned command,void *argument)
{
	if(context->fd<0 || !context->identity) {errno=EBADF;return -1;}
	if(!expected_mm) {errno=EINVAL;return -1;}
	struct vmctx_memory q={.version=VMCTX_MEMORY_ABI,.size=sizeof(q),
		.op=VMCTX_MEMORY_CALL,.command=command,.mm_identity=expected_mm,
		.argument=(uintptr_t)argument};
	return source_control_raw(source_context_selector(context),VMCTX_CTL_MEMORY,&q);
}
#endif
