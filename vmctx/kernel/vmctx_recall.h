/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef VMCTX_RECALL_H
#define VMCTX_RECALL_H
#include <linux/types.h>

/* Source-adapter operation. A ticket pins one mm and excludes another pull
 * of this virtual page. It is invalidated when that mapping is removed or
 * discarded. No lock is held between calls. The creating monitor thread owns
 * the ticket; closing that thread cancels its outstanding tickets. */
#define VMCTX_CTL_RECALL 35
#define VMCTX_RECALL_BEGIN 0
#define VMCTX_RECALL_COMMIT 1
#define VMCTX_RECALL_CANCEL 2
struct vmctx_recall {
	__u64 addr;
	__u64 ticket;
	__u64 buf;
	__u32 op;
	__u32 gen;
};
/* BEGIN uses a target PID: 0 = claimed (ticket returned), 1 = already local,
 * -EAGAIN = a transfer is in flight. COMMIT/CANCEL use PID 0 and the ticket.
 * COMMIT copies exactly one page and consumes the ticket even on failure.
 * -ESTALE means that its original mapping was removed/discarded; no bytes
 * were installed. A caller must never retry those bytes at the reused VA. */
#endif
