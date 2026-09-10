/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef VMCTX_TRANSFER_H
#define VMCTX_TRANSFER_H
#include <linux/types.h>
/* Source-local custody. BEGIN allocates a known ticket before any page moves.
 * CAPTURE executes at most once; READ repeats the retained immutable result.
 * ACK releases custody only for that ticket. FORGET follows a confirmed ACK.
 * CANCEL refuses a captured write grant; abandoning one poisons its MM.
 * ABANDON releases all calling-thread custody before native exit waits.
 * Except BEGIN (expected-MM MEMORY envelope), operations use selector zero.
 * Tickets belong to the creating monitor thread, not a reusable PID number. */
#define VMCTX_CTL_TRANSFER 49
#define VMCTX_TRANSFER_ABI 1
#define VMCTX_TRANSFER_CAPS 0
#define VMCTX_TRANSFER_BEGIN 1
#define VMCTX_TRANSFER_CAPTURE 2
#define VMCTX_TRANSFER_READ 3
#define VMCTX_TRANSFER_ACK 4
#define VMCTX_TRANSFER_CANCEL 5
#define VMCTX_TRANSFER_FORGET 6
#define VMCTX_TRANSFER_ABANDON 7
#define VMCTX_TRANSFER_EXACT 1U
#define VMCTX_TRANSFER_REPLAY 2U
#define VMCTX_TRANSFER_TEARDOWN 4U
#define VMCTX_TRANSFER_FEATURES (VMCTX_TRANSFER_EXACT|VMCTX_TRANSFER_REPLAY|VMCTX_TRANSFER_TEARDOWN)
struct vmctx_transfer {
    __u32 version,size,op,features;
    __u64 mm_identity,address,ticket,buf;
    __u32 status,flags,class,state,gen,sum;
    __u64 reserved;
};
#endif
