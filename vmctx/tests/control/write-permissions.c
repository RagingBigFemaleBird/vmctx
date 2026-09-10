/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#define main vmremote_program_main
#ifndef VMREMOTE_SOURCE
#define VMREMOTE_SOURCE "../../user/vmremote.c"
#endif
#include VMREMOTE_SOURCE
#undef main
#define TEST_MM_REGISTRY executor_mms
#include "execution-fixture.h"
struct permission_peer {int fd,cow;struct vmr_mm_binding target;};
static void *permission_peer_run(void *opaque)
{
    struct permission_peer *peer=opaque;struct vmr_req request;
    assert(!pg_rw(peer->fd,&request,sizeof(request),0));
    assert(request.nr==(peer->cow?VMR_OP_WRITEPREP:VMR_OP_CTXPAGE));
    assert(vmr_binding_equal(&request.target,&peer->target));
    struct vmr_rsp reply={.magic=VMR_MAGIC,.binding=peer->target,
        .retval=peer->cow ? -EACCES : VMR_CTXPAGE_CLAIMING};
    vmr_memory_receipt(&request,&reply);
    assert(!pg_rw(peer->fd,&reply,sizeof(reply),1));close(peer->fd);return NULL;
}
int main(int argc,char **argv)
{
    assert(argc==2);alarm(10);signal(SIGPIPE,SIG_IGN);coh_lock_init();
    int cow=!strcmp(argv[1],"cow-denied");
    memory_target target=test_target(7);uint64_t base=0x4000;
    if(cow) {cow_prot_set(7,base,COW_PROT_OWED);page_watch_set(target,base,1);}
    else page_lend(target,base,44);
    int fds[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,fds));sock=fds[0];
    struct permission_peer peer={fds[1],cow,target->source};pthread_t worker;
    assert(!pthread_create(&worker,NULL,permission_peer_run,&peer));
    int result=fault_from_home_inner(target,14,base,3,0,0);
    assert(!pthread_join(worker,NULL));close(sock);sock=-1;
    int pass=!coh_depth && !fault_map.op && (cow ?
        result==FAULT_NEEDS_OWNER && cow_is_protected(7,base) && page_watched(target,base) :
        result==1 && page_holder(target,base)==44);
    printf("%s: %s result=%d loan=%llu COW=%d watched=%d coherence=%d map=%u\n",
        pass?"PASS":"FAIL",argv[1],result,(unsigned long long)page_holder(target,base),
        cow_is_protected(7,base),page_watched(target,base),coh_depth,fault_map.op);
    return pass?0:1;
}
