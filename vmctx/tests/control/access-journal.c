/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdatomic.h>
#define main vmremote_program_main
#ifndef VMREMOTE_SOURCE
#define VMREMOTE_SOURCE "../../user/vmremote.c"
#endif
#include VMREMOTE_SOURCE
#undef main
#define TEST_MM_REGISTRY executor_mms
#include "execution-fixture.h"

static uint64_t fixture_mm=UINT64_C(0x100000007);
static pthread_mutex_t source_lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t source_acked;
static unsigned source_reads,source_acks,callback_blocked,wrong_target;
static int malformed,callback_newer,failed_ack;
struct peer {int fd;memory_target target;};
static void *journal_peer(void *opaque)
{
    struct peer *p=opaque;struct vmr_req request;
    while(!pg_rw(p->fd,&request,sizeof(request),0)) {
        assert(request.magic==VMR_MAGIC && !request.datalen);
        assert(request.nr==VMR_OP_ACCESS_READ || request.nr==VMR_OP_ACCESS_ACK);
        struct vmr_access_batch batch={.identity=fixture_mm};
        struct vmr_rsp response={.magic=VMR_MAGIC,.retval=sizeof(batch),.datalen=sizeof(batch),
            /* A reply publication can describe the context's newer image;
             * journal bytes still belong to the request's saved MM. */
            .binding={.context=fixture_mm,.mm=fixture_mm+1,.epoch=2}};
        vmr_memory_receipt(&request,&response);
        assert(vmr_binding_equal(&request.target,&p->target->source));
        pthread_mutex_lock(&source_lock);
        if(request.args[0]!=fixture_mm)wrong_target++;
        if(request.nr==VMR_OP_ACCESS_READ) {
            source_reads++;
            assert(request.args[1]==source_acked);
            batch.acknowledged=source_acked;batch.cursor=source_acked;batch.head=1;
            if(!source_acked) {
                batch.count=1;batch.cursor=1;
                batch.event[0]=(struct vmr_access_event){.sequence=1,.start=0x4000,.end=0x6000,
                    .kind=VMR_ACCESS_PROTECT,.protection=VMR_PROT_READ};
                if(malformed) {
                    batch.count=2;batch.event[1]=batch.event[0];
                    batch.event[1].sequence=0; /* Bad suffix must not apply valid prefix. */
                }
            }
        } else {
            source_acks++;
            assert(request.args[1]==1 && !source_acked);
            if(failed_ack) {response.retval=-EIO;response.datalen=0;}
            else source_acked=1;
            batch.acknowledged=batch.cursor=batch.head=source_acked;
        }
        pthread_mutex_unlock(&source_lock);
        /* Simulate a source callback that must publish local construction
         * before it can answer. Probe instead of hanging a broken baseline. */
        int error=pthread_mutex_trylock(&layout_order_lock);
        if(error==EBUSY) {
            pthread_mutex_lock(&source_lock);callback_blocked++;pthread_mutex_unlock(&source_lock);
        } else {
            assert(!error);
            if(callback_newer && request.nr==VMR_OP_ACCESS_READ)
                cons_note_locked(p->target,0x4000,0x6000,2,VMCTX_MAP_SET,PROT_WRITE,0);
            pthread_mutex_unlock(&layout_order_lock);
        }
        assert(!pg_rw(p->fd,&response,sizeof(response),1));
        if(response.datalen)assert(!pg_rw(p->fd,&batch,sizeof(batch),1));
    }
    close(p->fd);return NULL;
}
struct client {int fd;memory_target target;int result;};
static void *journal_client(void *opaque)
{
    struct client *p=opaque;sock=p->fd;
#ifdef JOURNAL_BASELINE
    pthread_mutex_lock(&layout_order_lock);
    p->result=access_sync_locked(p->target);
    pthread_mutex_unlock(&layout_order_lock);
#else
    p->result=access_sync(p->target);
    if(p->result) {
        /* Failure poisons the cursor. It cannot silently retry an ACK whose
         * source-side commit is unknown or consume more malformed data. */
        assert(access_sync(p->target)==-1);
    }
#endif
    close(sock);sock=-1;return NULL;
}
int main(int argc,char **argv)
{
    assert(argc==2);alarm(10);coh_lock_init();
    int concurrent=!strcmp(argv[1],"concurrent");
    malformed=!strcmp(argv[1],"malformed");
    failed_ack=!strcmp(argv[1],"failed-ack");
    callback_newer=!strcmp(argv[1],"callback-newer");
    memory_target target=test_target(fixture_mm);
    unsigned count=concurrent?2:1;
    pthread_t clients[2],peers[2];struct client client[2];struct peer peer[2];
    for(unsigned i=0;i<count;i++) {
        int fds[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));
        client[i]=(struct client){fds[0],target,-2};peer[i]=(struct peer){fds[1],target};
        assert(!pthread_create(&peers[i],NULL,journal_peer,&peer[i]));
        assert(!pthread_create(&clients[i],NULL,journal_client,&client[i]));
    }
    for(unsigned i=0;i<count;i++) {
        assert(!pthread_join(clients[i],NULL));assert(!pthread_join(peers[i],NULL));
        assert(client[i].result==((malformed||failed_ack)?-1:0));
    }
    struct access_range range;
    int have=access_lookup(fixture_mm,0x4000,0x6000,&range);
    int pass=!wrong_target && !callback_blocked;
    if(malformed)pass &= !have && !source_acks && !source_acked;
    else if(failed_ack)pass &= have && source_acks==1 && !source_acked && !access_cursor_for(fixture_mm)->acknowledged;
    else if(callback_newer) {
        uint64_t start,end,off,sequence;uint32_t op,prot;
        pass &= !have && source_acks==1 && source_acked==1 &&
            cons_covering(fixture_mm,0x4000,&start,&end,&op,&prot,&off,&sequence) && sequence==2 && prot==PROT_WRITE;
    } else pass &= have && range.sequence==1 && range.protection==VMR_PROT_READ && source_acks==1 && source_acked==1;
    pass &= source_reads==count;
    printf("%s: journal mode=%s reads=%u acks=%u wrong_target=%u blocked_callbacks=%u applied=%d source_acked=%llu\n",
        pass?"PASS":"FAIL",argv[1],source_reads,source_acks,wrong_target,callback_blocked,have,(unsigned long long)source_acked);
    return pass?0:1;
}
