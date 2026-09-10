/* SPDX-License-Identifier: GPL-2.0 */
/* One connection owns this collection on one native thread. Allocate before
 * BEGIN; retain every issued handle until confirmed FORGET or ABANDON. The
 * collection has no eviction policy. Its caller must stop a failed session
 * after abandonment, including other contexts sharing any poisoned MM. */
#ifndef VMR_SOURCE_CUSTODY_H
#define VMR_SOURCE_CUSTODY_H
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include "vmrproto.h"
#include "source-transfer.h"

struct source_custody_entry {
    struct source_custody_entry *next;
    struct vmr_mm_binding target;
    struct source_transfer transfer;
};
struct source_custody {
    struct source_custody_entry *pending;
    size_t count;
    pthread_t owner;
    int owned,closed;
};

static inline int source_custody_thread(struct source_custody *custody)
{
    if(!custody) {errno=EINVAL;return -1;}
    if(custody->owned && !pthread_equal(custody->owner,pthread_self())) {
        errno=EPERM;return -1;
    }
    if(!custody->owned) {custody->owner=pthread_self();custody->owned=1;}
    return 0;
}

/* A failed BEGIN with a returned handle remains in this collection. No caller
 * may infer that an error discarded the native operation. The out pointer is
 * NULL only when no native handle can have been issued. */
static inline int source_custody_begin(struct source_custody *custody,
        const struct vmr_mm_binding *target,uint64_t address,
        struct source_custody_entry **out)
{
    if(!out) {errno=EINVAL;return -1;}
    *out=NULL;
    if(source_custody_thread(custody))return -1;
    if(custody->closed) {errno=ESHUTDOWN;return -1;}
    if(!target || !vmr_binding_valid(target) || target->context>INT_MAX || (address&4095)) {
        errno=EINVAL;return -1;
    }
    if(custody->count==SIZE_MAX) {errno=EOVERFLOW;return -1;}
    struct source_custody_entry *entry=calloc(1,sizeof(*entry));
    if(!entry)return -1;
    entry->target=*target;
    struct source_memory_target saved={(source_id)target->context,target->mm,target->epoch};
    int result=source_transfer_begin(&saved,address,&entry->transfer),error=errno;
    if(result && !entry->transfer.begun) {free(entry);errno=error;return -1;}
    entry->next=custody->pending;custody->pending=entry;custody->count++;
    *out=entry;
    errno=error;return result;
}

static inline struct source_custody_entry **source_custody_find(
        struct source_custody *custody,const struct vmr_mm_binding *target,
        uint64_t address,uint64_t ticket)
{
    if(source_custody_thread(custody))return NULL;
    if(custody->closed) {errno=ESHUTDOWN;return NULL;}
    if(!target || !vmr_binding_valid(target) || !ticket || (address&4095)) {
        errno=EINVAL;return NULL;
    }
    for(struct source_custody_entry **at=&custody->pending;*at;at=&(*at)->next) {
        struct source_custody_entry *entry=*at;
        if(entry->transfer.ticket!=ticket)continue;
        if(entry->transfer.address!=address || !vmr_binding_equal(&entry->target,target)) {
            errno=ESTALE;return NULL;
        }
        return at;
    }
    errno=ENOENT;return NULL;
}

static inline int source_custody_retire(struct source_custody *custody,
        struct source_custody_entry **at)
{
    struct source_custody_entry *entry=*at;
    if(source_transfer_forget(&entry->transfer))return -1;
    *at=entry->next;custody->count--;free(entry);return 0;
}

/* ACK and FORGET are separate native commits. A failed FORGET retains the
 * settled handle for an exact retry. A stale ACK stays a failure even when
 * native cleanup succeeds; it can never authorize executor continuation. */
static inline int source_custody_ack(struct source_custody *custody,
        const struct vmr_mm_binding *target,uint64_t address,uint64_t ticket)
{
    struct source_custody_entry **at=source_custody_find(custody,target,address,ticket);
    if(!at)return -1;
    int result=source_transfer_ack(&(*at)->transfer),error=errno;
    if(!(*at)->transfer.settled)return result;
    if(source_custody_retire(custody,at))return -1;
    errno=error;return result;
}

/* Cancellation is not an installation. Native custody rejects it after an
 * unresolved grant unless mapping invalidation has made the grant unusable. */
static inline int source_custody_cancel(struct source_custody *custody,
        const struct vmr_mm_binding *target,uint64_t address,uint64_t ticket)
{
    struct source_custody_entry **at=source_custody_find(custody,target,address,ticket);
    if(!at)return -1;
    if(source_transfer_cancel(&(*at)->transfer))return -1;
    return source_custody_retire(custody,at);
}

/* ABANDON affects all tickets on the native thread. There must be only one
 * custody collection per connection thread, and no independent native ticket
 * lifetime on that thread. Close before waiting for source exit or clear-TID.
 * Failure retains all records and prohibits new work. */
static inline int source_custody_abandon(struct source_custody *custody)
{
    if(source_custody_thread(custody))return -1;
    custody->closed=1;
    if(source_transfer_abandon())return -1;
    while(custody->pending) {
        struct source_custody_entry *entry=custody->pending;
        custody->pending=entry->next;free(entry);
    }
    custody->count=0;return 0;
}
#endif
