/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#define main vmremote_program_main
#include "../../user/vmremote.c"
#undef main
#define TEST_MM_REGISTRY executor_mms
#include "execution-fixture.h"

struct memory_peer {int fd,mode;struct vmr_mm_binding target;};
static void *memory_peer_run(void *opaque)
{
    struct memory_peer *peer=opaque;
    struct vmr_req request;
    assert(!pg_rw(peer->fd,&request,sizeof(request),0));
    assert(request.nr==VMR_OP_CTXPAGE && vmr_binding_equal(&request.target,&peer->target));
    struct vmr_rsp reply={.magic=VMR_MAGIC,.retval=4096,.datalen=4096,.ngen=1,.pages_taken=1,.page_episode=UINT64_C(0x8000000100000001)};
    reply.binding=peer->target;reply.binding.mm+=UINT64_C(1)<<32;reply.binding.epoch++;
    reply.pggen[0]=73;
    vmr_memory_receipt(&request,&reply);
    switch(peer->mode) {
    case 0:reply.memory_target.context++;break;
    case 1:reply.memory_target.mm+=UINT64_C(1)<<32;break;
    case 2:reply.memory_target.parent_mm++;break;
    case 3:reply.memory_target.epoch++;break;
    case 4:reply.memory_op=VMR_OP_PAGE;break;
    case 11:reply.ngen=17;break;
    case 12:reply.datalen=4097;break;
    case 14:reply.retval=-ESTALE;reply.datalen=0;reply.ngen=0;reply.pages_taken=0;reply.page_episode=0;break;
    case 16:reply.retval=8192;reply.datalen=8192;reply.ngen=2;reply.pages_taken=3;break;
    case 17:reply.retval=-ESTALE;reply.datalen=0;reply.ngen=0;break;
    case 18:reply.pages_taken=UINT64_C(1)<<32;break;
    case 19:reply.page_episode=0;break;
    case 20:reply.pages_taken=0;break;
    default:if(peer->mode>=5 && peer->mode<=10)reply.memory_args[peer->mode-5]++;break;
    }
    unsigned char payload[4097];memset(payload,0xa5,sizeof(payload));
    assert(!pg_rw(peer->fd,&reply,sizeof(reply),1));
    if(peer->mode==13)assert(!pg_rw(peer->fd,payload,2048,1));
    else if(peer->mode==15)assert(!pg_rw(peer->fd,payload,4096,1));
    close(peer->fd);return NULL;
}
int main(void)
{
    signal(SIGPIPE,SIG_IGN);alarm(10);
    memory_target target=test_target(UINT64_C(0x100000007));
    for(int mode=0;mode<21;mode++) {
        int fds[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));sock=fds[0];
        struct memory_peer peer={fds[1],mode,target->source};pthread_t worker;
        assert(!pthread_create(&worker,NULL,memory_peer_run,&peer));
        uint64_t args[6]={0x4000,4096};unsigned char bytes[4096];memset(bytes,0xcc,sizeof(bytes));
        struct vmr_rsp receipt,before;memset(&receipt,0x67,sizeof(receipt));before=receipt;
        long result=home_memory_call(target,VMR_OP_CTXPAGE,args,NULL,0,bytes,sizeof(bytes),&receipt);
        if(mode<14 || mode>=16) {
            assert(result==-1 && home_call_failed && !home_data_len);
            assert(!memcmp(&receipt,&before,sizeof(receipt)));
            for(unsigned i=0;i<sizeof(bytes);i++)assert(bytes[i]==0xcc);
        } else if(mode==14) {
            assert(result==-ESTALE && !home_call_failed && !receipt.datalen);
            for(unsigned i=0;i<sizeof(bytes);i++)assert(bytes[i]==0xcc);
        } else {
            assert(result==4096 && !home_call_failed && receipt.pggen[0]==73);
            assert(vmr_binding_equal(&receipt.memory_target,&target->source));
            assert(receipt.binding.mm!=target->source.mm);
            for(unsigned i=0;i<sizeof(bytes);i++)assert(bytes[i]==0xa5);
        }
        close(sock);sock=-1;assert(!pthread_join(worker,NULL));
    }
    puts("PASS: memory replies match saved MM/context/epoch/ancestry and all operation arguments; malformed/truncated frames preserve caller bytes and receipts");
    return 0;
}
