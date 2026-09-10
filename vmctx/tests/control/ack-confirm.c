/* SPDX-License-Identifier: GPL-2.0 */
/* A successful socket write is not an acknowledgement. The peer independently
 * rejects, corrupts, or omits the response after consuming the full request. */
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

int main(int argc,char **argv)
{
    assert(argc==2);alarm(10);signal(SIGPIPE,SIG_IGN);
    const char *mode=argv[1];
    int good=!strcmp(mode,"accepted"),eof=!strcmp(mode,"eof"),negative=!strcmp(mode,"rejected"),
        target=!strcmp(mode,"wrong-target"),payload=!strcmp(mode,"payload");
    assert(good || eof || negative || target || payload);
    memory_target saved=test_target(7);int sockets[2];
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    pid_t child=fork();assert(child>=0);
    if(!child) {
        close(sockets[1]);sock=sockets[0];
        ack_pend.target=saved->source;ack_pend.page=0x4000;ack_pend.episode=UINT64_C(0x800000010000beef);ack_pend.set=1;
        ack_flush();
        assert(!ack_pend.set && n_acks_sent==1);ack_flush();_exit(0);
    }
    close(sockets[0]);struct vmr_req request;
    assert(!pg_rw(sockets[1],&request,sizeof(request),0));
    assert(request.nr==VMR_OP_INSTALLED && request.args[0]==0x4000 && request.args[1]==UINT64_C(0x800000010000beef));
    assert(vmr_binding_equal(&request.target,&saved->source));
    if(!eof) {
        struct vmr_rsp reply={.magic=VMR_MAGIC,.retval=negative ? -ESTALE:0,.binding=saved->source};
        vmr_memory_receipt(&request,&reply);
        if(target)reply.memory_target.mm+=UINT64_C(1)<<32;
        if(payload)reply.datalen=1;
        /* An old one-way sender may already have exited. Preserve that
         * outcome instead of failing at the peer's EPIPE. */
        (void)pg_rw(sockets[1],&reply,sizeof(reply),1);
    }
    close(sockets[1]);int status;
    assert(waitpid(child,&status,0)==child);
    int actual=WIFEXITED(status) ? WEXITSTATUS(status):-1;
    int pass=actual==(good ? 0:97);
    printf("%s: source acknowledgement %s, executor exit=%d expected=%d\n",
        pass ? "PASS":"FAIL",mode,actual,good ? 0:97);
    return pass ? 0:1;
}
