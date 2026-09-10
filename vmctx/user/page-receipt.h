/* SPDX-License-Identifier: GPL-2.0 */
/* The caller retains the source's exact page reply through local landings and
 * nested callbacks. Content repair cannot change the acknowledged wire fact. */
#ifndef VMR_PAGE_RECEIPT_H
#define VMR_PAGE_RECEIPT_H
#include "vmrproto.h"
struct source_page_receipt {
    struct vmr_mm_binding target;
    uint64_t base,episode;
    uint32_t count,taken;
    uint32_t generation[16],sum[16];
    /* Internal lifetime only; not part of the wire. The queued ACK owns the
     * obligation after local landing; its flush must confirm settlement. */
    int ack_queued;
};

static inline int source_page_response_valid(const struct vmr_req *request,
        const struct vmr_rsp *reply)
{
    if(request->nr!=VMR_OP_CTXPAGE)return 1;
    if(reply->retval<=0)return !reply->datalen && !reply->ngen && !reply->pages_taken && !reply->page_episode;
    if(request->args[1]!=4096 || (request->args[0]&4095) ||
       (uint64_t)reply->retval!=reply->datalen || (reply->datalen&4095) ||
       reply->datalen!=4096)return 0;
    unsigned count=(unsigned)(reply->datalen/4096);
    return reply->ngen==count && reply->pages_taken==1 && reply->page_episode;
}
#endif
