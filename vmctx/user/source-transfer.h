/* SPDX-License-Identifier: GPL-2.0 */
/* Source-native custody adapter. A caller owns this object on one native
 * thread from BEGIN through confirmed FORGET. Errors preserve the handle;
 * only confirmed settlement permits releasing it. No native handle or
 * control number describes executor memory or belongs in its OS adapter. */
#ifndef VMR_SOURCE_TRANSFER_H
#define VMR_SOURCE_TRANSFER_H
#include "source-memory-target.h"
#include "vmctx_transfer.h"

struct source_transfer {
    uint64_t mm, address, ticket;
    struct vmctx_transfer result;
    int begun, captured, read, settled, cancelled, settlement_error;
};

static inline int source_transfer_caps(void)
{
    struct vmctx_transfer q={.version=VMCTX_TRANSFER_ABI,.size=sizeof(q)};
    if(source_control_raw(0,VMCTX_CTL_TRANSFER,&q))return -1;
    if(q.version!=VMCTX_TRANSFER_ABI || q.size!=sizeof(q) || q.op ||
       q.features!=VMCTX_TRANSFER_FEATURES ||
       q.mm_identity || q.address || q.ticket || q.buf || q.status || q.flags ||
       q.class || q.state || q.gen || q.sum || q.reserved) {errno=EPROTO;return -1;}
    return 0;
}

static inline int source_transfer_begin(const struct source_memory_target *target,
        uint64_t address,struct source_transfer *transfer)
{
    if(!target || !target->mm || !transfer || transfer->begun || transfer->ticket || (address&4095)) {
        errno=EINVAL;return -1;
    }
    struct vmctx_transfer q={.version=VMCTX_TRANSFER_ABI,.size=sizeof(q),
        .op=VMCTX_TRANSFER_BEGIN,.mm_identity=target->mm,.address=address};
    if(source_memory_target_call(target,VMCTX_CTL_TRANSFER,&q))return -1;
    /* Preserve even a malformed returned ticket so the caller cannot start
     * another operation while potentially owning native custody. */
    *transfer=(struct source_transfer){.mm=target->mm,.address=address,.ticket=q.ticket,.begun=1};
    if(q.version!=VMCTX_TRANSFER_ABI || q.size!=sizeof(q) ||
       q.op!=VMCTX_TRANSFER_BEGIN || q.mm_identity!=target->mm ||
       q.address!=address || !q.ticket || q.buf || q.features || q.status ||
       q.flags || q.class || q.state || q.gen || q.sum || q.reserved) {
        errno=EPROTO;return -1;
    }
    return 0;
}

/* CAPTURE, READ, ACK and CANCEL are replayable for this exact handle. A
 * native EFAULT may follow a commit, so retry only this operation and ticket.
 * A different target is never selected after BEGIN. The caller decides when
 * to retry; this adapter neither spins nor converts uncertainty into success. */
static inline int source_transfer_step(struct source_transfer *transfer,unsigned op,
        void *bytes,struct vmctx_transfer *result)
{
    if(!transfer || !transfer->ticket || !transfer->mm) {errno=EINVAL;return -1;}
    struct vmctx_transfer q={.version=VMCTX_TRANSFER_ABI,.size=sizeof(q),.op=op,
        .mm_identity=transfer->mm,.address=transfer->address,
        .ticket=transfer->ticket,.buf=(uintptr_t)bytes};
    if(source_control_raw(0,VMCTX_CTL_TRANSFER,&q))return -1;
    if(op!=VMCTX_TRANSFER_FORGET &&
       (q.version!=VMCTX_TRANSFER_ABI || q.size!=sizeof(q) || q.op!=op ||
        q.mm_identity!=transfer->mm || q.address!=transfer->address ||
        q.ticket!=transfer->ticket || q.buf!=(uintptr_t)bytes || q.reserved ||
        q.features!=VMCTX_TRANSFER_FEATURES)) {
        errno=EPROTO;return -1;
    }
    if(result)*result=q;
    return 0;
}

static inline int source_transfer_capture(struct source_transfer *transfer)
{
    struct vmctx_transfer q;
    if(source_transfer_step(transfer,VMCTX_TRANSFER_CAPTURE,NULL,&q))return -1;
    if(q.status<VMCTX_SERVE_TAKEN || q.status>VMCTX_SERVE_NOMAP ||
       (q.flags&~VMCTX_SERVE_GRANT) ||
       (q.class&~(VMCTX_PGC_VALID|VMCTX_PGC_FILE|VMCTX_PGC_SHARED|
                   VMCTX_PGC_WRITE|VMCTX_PGC_PRESENT|VMCTX_PGC_RO)) || q.state>VMCTX_PG_TRANSIT ||
       q.sum>UINT16_MAX || ((q.flags&VMCTX_SERVE_GRANT) &&
       q.status!=VMCTX_SERVE_COPIED && q.status!=VMCTX_SERVE_ABSENT)) {errno=EPROTO;return -1;}
    if(transfer->captured &&
       (q.status!=transfer->result.status || q.flags!=transfer->result.flags ||
        q.class!=transfer->result.class || q.state!=transfer->result.state ||
        q.gen!=transfer->result.gen || q.sum!=transfer->result.sum)) {errno=EPROTO;return -1;}
    transfer->result=q;
    transfer->captured=1;
    return 0;
}

static inline int source_transfer_read(struct source_transfer *transfer,void *bytes)
{
    if(!transfer || !transfer->captured || !bytes) {errno=EINVAL;return -1;}
    unsigned char staging[4096];
    struct vmctx_transfer q;
    if(source_transfer_step(transfer,VMCTX_TRANSFER_READ,staging,&q))return -1;
    /* Identity and complete native copyout precede caller publication. The
     * immutable capture result cannot change while READ replays its bytes. */
    if(q.status!=transfer->result.status || q.flags!=transfer->result.flags ||
       q.class!=transfer->result.class || q.state!=transfer->result.state ||
       q.gen!=transfer->result.gen || q.sum!=transfer->result.sum) {errno=EPROTO;return -1;}
    if(q.status==VMCTX_SERVE_TAKEN || q.status==VMCTX_SERVE_COPIED)
        memcpy(bytes,staging,sizeof(staging));
    transfer->read=1;
    return 0;
}

static inline int source_transfer_ack(struct source_transfer *transfer)
{
    if(transfer && transfer->cancelled) {errno=ECANCELED;return -1;}
    if(!transfer || !transfer->read) {errno=EINVAL;return -1;}
    if(transfer->settled) {
        errno=transfer->settlement_error;
        return errno ? -1:0;
    }
    int result=source_transfer_step(transfer,VMCTX_TRANSFER_ACK,NULL,NULL);
    if(!result || errno==ESTALE) {
        transfer->settled=1;transfer->settlement_error=result ? errno:0;
    }
    return result;
}

static inline int source_transfer_cancel(struct source_transfer *transfer)
{
    if(transfer && transfer->settled)return 0;
    if(source_transfer_step(transfer,VMCTX_TRANSFER_CANCEL,NULL,NULL))return -1;
    transfer->settled=1;transfer->cancelled=1;
    return 0;
}

static inline int source_transfer_forget(struct source_transfer *transfer)
{
    if(!transfer || !transfer->settled) {errno=EINVAL;return -1;}
    if(source_transfer_step(transfer,VMCTX_TRANSFER_FORGET,NULL,NULL))return -1;
    *transfer=(struct source_transfer){0};
    return 0;
}

/* Terminal connection failure only. This invalidates every ticket created
 * by the calling native thread, poisons unresolved grants, and releases native
 * guards before waiting for source exit/clear-TID. It proves no delivery. */
static inline int source_transfer_abandon(void)
{
    struct vmctx_transfer q={.version=VMCTX_TRANSFER_ABI,.size=sizeof(q),
        .op=VMCTX_TRANSFER_ABANDON};
    return source_control_raw(0,VMCTX_CTL_TRANSFER,&q) ? -1:0;
}
#endif
