/* SPDX-License-Identifier: GPL-2.0 */
/* Equal bytes cannot identify two transfer episodes. Observe the complete
 * production wire ACK independently of the sender's local receipt fields. */
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

int main(void)
{
    alarm(10);signal(SIGPIPE,SIG_IGN);
    const uint64_t episode=UINT64_C(0x800000010000beef);
    memory_target target=test_target(7);
    struct source_page_receipt receipt={.target=target->source,.base=0x4000,.count=1,.taken=1};
    receipt.sum[0]=0xbeef;
#if VMR_MAGIC == 0x50524d56u
    receipt.episode=episode;
#endif
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    pid_t child=fork();assert(child>=0);
    if(!child) {
        close(sockets[1]);sock=sockets[0];
        ack_installed(&receipt,0x4000);ack_flush();_exit(0);
    }
    close(sockets[0]);struct vmr_req request;
    assert(!pg_rw(sockets[1],&request,sizeof(request),0));
    struct vmr_rsp reply={.magic=VMR_MAGIC,.binding=target->source};
    vmr_memory_receipt(&request,&reply);
    assert(!pg_rw(sockets[1],&reply,sizeof(reply),1));close(sockets[1]);
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    int pass=request.nr==VMR_OP_INSTALLED && request.args[0]==0x4000 &&
        request.args[1]==episode && vmr_binding_equal(&request.target,&target->source);
    printf("%s: acknowledgement identity=%016llx expected episode=%016llx; equal page checksum=%04x\n",
        pass ? "PASS":"FAIL",(unsigned long long)request.args[1],
        (unsigned long long)episode,receipt.sum[0]);
    return pass ? 0:1;
}
