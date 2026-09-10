/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#define main vmremote_program_main
#include "../../user/vmremote.c"
#undef main
#define TEST_MM_REGISTRY executor_mms
#include "execution-fixture.h"

struct receipt_peer {int fd;struct vmr_mm_binding target[2];};
static void *receipt_peer_run(void *opaque)
{
    struct receipt_peer *peer=opaque;
    unsigned char bytes[4096];uint64_t episodes[2];
    for(unsigned i=0;i<2;i++) {
        struct vmr_req request;assert(!pg_rw(peer->fd,&request,sizeof(request),0));
        assert(request.nr==VMR_OP_CTXPAGE && vmr_binding_equal(&request.target,&peer->target[i]));
        struct vmr_rsp reply={.magic=VMR_MAGIC,.retval=4096,.datalen=4096,.ngen=1,
            .pages_taken=1,.binding=peer->target[1]};
        reply.page_episode=episodes[i]=UINT64_C(0x8000000100000011)+i;
        reply.pggen[0]=51+i;vmr_memory_receipt(&request,&reply);
        memset(bytes,0x11+i,sizeof(bytes));
        assert(!pg_rw(peer->fd,&reply,sizeof(reply),1));assert(!pg_rw(peer->fd,bytes,sizeof(bytes),1));
    }
    for(unsigned i=0;i<2;i++) {
        struct vmr_req ack;assert(!pg_rw(peer->fd,&ack,sizeof(ack),0));
        assert(ack.nr==VMR_OP_INSTALLED && ack.args[0]==0x4000 && ack.args[1]==episodes[i]);
        assert(vmr_binding_equal(&ack.target,&peer->target[i]));
        struct vmr_rsp confirmed={.magic=VMR_MAGIC,.binding=peer->target[1]};
        vmr_memory_receipt(&ack,&confirmed);
        assert(!pg_rw(peer->fd,&confirmed,sizeof(confirmed),1));
    }
    close(peer->fd);return NULL;
}
static int replace_binding(void *unused,int fd,uint64_t epoch,uint64_t *published)
{(void)unused;assert(fd>=0);*published=epoch+1;return 0;}
int main(void)
{
    alarm(10);signal(SIGPIPE,SIG_IGN);
    memory_target old=test_target(7);
    struct vmr_mm_binding changed=old->source;changed.mm+=UINT64_C(1)<<32;changed.epoch++;
    struct execution_mm *fresh=source_mm_resolve(&changed);assert(fresh);
    memory_target next=execution_target_publish(old->context,fresh,&changed,replace_binding,NULL);assert(next);
    int fds[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));sock=fds[0];
    struct receipt_peer peer={.fd=fds[1],.target={old->source,next->source}};pthread_t worker;
    assert(!pthread_create(&worker,NULL,receipt_peer_run,&peer));
    uint64_t args[6]={0x4000,4096};unsigned char bytes[4096];
    struct source_page_receipt first={0},second={0};
    assert(source_page_fetch(old,args,bytes,sizeof(bytes),&first)==4096);
    assert(source_page_fetch(next,args,bytes,sizeof(bytes),&second)==4096);
    assert(first.generation[0]==51 && second.generation[0]==52);
    assert(vmr_binding_equal(&first.target,&old->source));
    memset(bytes,0xee,sizeof(bytes)); /* A local content repair is not the wire receipt. */
    ack_installed(&first,0x4000);ack_installed(&second,0x4000);ack_flush();
    assert(n_acks_sent==2 && !ack_pend.set);
    assert(!pthread_join(worker,NULL));close(sock);sock=-1;
    pid_t child=fork();assert(child>=0);
    if(!child) {ack_installed(&first,0x5000);_exit(0);}
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==97);
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));child=fork();assert(child>=0);
    if(!child) {
        close(fds[1]);sock=fds[0];
        source_page_fetch(old,args,bytes,sizeof(bytes),&first);_exit(0);
    }
    close(fds[0]);struct vmr_req request;assert(!pg_rw(fds[1],&request,sizeof(request),0));
    struct vmr_rsp failed={.magic=VMR_MAGIC,.retval=VMR_CTXPAGE_FAILED,.binding=changed};
    vmr_memory_receipt(&request,&failed);assert(!pg_rw(fds[1],&failed,sizeof(failed),1));close(fds[1]);
    assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==97);
    puts("PASS: nested fetches and exec preserve caller-owned page receipts; ACKs echo opaque full64 episodes and old MM; missing receipts and uncertain source outcomes stop before permission grants");
    return 0;
}
