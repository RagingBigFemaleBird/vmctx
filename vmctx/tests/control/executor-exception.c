/* SPDX-License-Identifier: GPL-2.0 */
/* Real exception wire and retained pidfds. A terminal source reply must stop
 * exactly its executor, keep a sibling alive and never install a CPU frame. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/prctl.h>
static long exception_syscall(long number,...);
#define syscall exception_syscall
#define main vmremote_program_main
#ifndef VMREMOTE_SOURCE
#define VMREMOTE_SOURCE "../../user/vmremote.c"
#endif
#include VMREMOTE_SOURCE
#undef main
#undef syscall
#define TEST_MM_REGISTRY executor_mms
#include "execution-fixture.h"

static const char *mode;
static struct observations {unsigned setcpu,signals[2];uint64_t rip;} *seen;
static int exit_fds[2],peer_socket;
static struct vmr_mm_binding expected;
static long exception_syscall(long number,...)
{
    va_list ap;va_start(ap,number);
    if(number==SYS_pidfd_send_signal) {
        int fd=va_arg(ap,int),sig=va_arg(ap,int);void *info=va_arg(ap,void *);
        int flags=va_arg(ap,int);va_end(ap);
        unsigned i;for(i=0;i<2;i++)if(exit_fds[i]==fd)break;assert(i<2);
        seen->signals[i]++;
        if(!strcmp(mode,"stop-failed") && i==0 && seen->signals[i]==1) {
            errno=EPERM;return -1;
        }
        return syscall(number,fd,sig,info,flags);
    }
    assert(number==__NR_vmctx_ctl);
    int selector=va_arg(ap,int);unsigned command=va_arg(ap,unsigned);
    void *arg=va_arg(ap,void *);va_end(ap);assert(selector<0);
    if(command==VMCTX_CTL_GETCPU) {
        *(struct vmr_cpu_state *)arg=(struct vmr_cpu_state){.xstate_size=576,
            .regs={.rip=0x401000,.rsp=0x8000}};return 0;
    }
    if(command==VMCTX_CTL_SETCPU) {
        seen->setcpu++;seen->rip=((struct vmr_cpu_state *)arg)->regs.rip;return 0;
    }
    if(command==VMCTX_CTL_RUNTIME) {
        struct vmctx_runtime *q=arg;assert(q->op==VMCTX_RUNTIME_READ);
        q->epoch=9;q->total_ns=12345;return 0;
    }
    /* Diagnostic probes have no bytes. They must not influence delivery. */
    errno=EFAULT;return -1;
}
static void *peer(void *unused)
{
    (void)unused;
    struct vmr_req request;struct vmr_cpu_state cpu;
    assert(!pg_rw(peer_socket,&request,sizeof(request),0));
    assert(request.nr==VMR_NR_EXCEPTION && request.args[0]==6);
    assert(request.execution_epoch==9 && request.execution_ns==12345);
    assert(request.datalen<=sizeof(cpu));
    assert(!pg_rw(peer_socket,&cpu,request.datalen,0));
    assert(!vmr_cpu_wire_decode(&cpu,request.datalen));
    struct vmr_rsp reply={.magic=VMR_MAGIC,.binding=expected,.ended=1,
        .ended_status=SIGSEGV|128};
    if(!strcmp(mode,"wrong-mm"))reply.binding.mm+=UINT64_C(1)<<32;
    if(!strcmp(mode,"wrong-task"))reply.binding.context++;
    if(!strcmp(mode,"bad-ended"))reply.ended=2;
    if(!strcmp(mode,"bad-status"))reply.retval=-ETIMEDOUT;
    if(!strcmp(mode,"payload") || !strcmp(mode,"handler")) {
        cpu.regs.rip=0x5678;reply.datalen=vmr_cpu_wire_size(&cpu);
    }
    if(!strcmp(mode,"handler") || !strcmp(mode,"legal")) {
        reply.ended=0;reply.ended_status=0;
        if(!strcmp(mode,"legal"))reply.retval=VMR_EXC_LEGAL;
    }
    assert(!pg_rw(peer_socket,&reply,sizeof(reply),1));
    if(reply.datalen)assert(!pg_rw(peer_socket,&cpu,reply.datalen,1));
    close(peer_socket);return NULL;
}
static int worker(void)
{
    pid_t children[2];
    for(unsigned i=0;i<2;i++) {
        children[i]=fork();assert(children[i]>=0);
        if(!children[i]) {for(;;)pause();}
        exit_fds[i]=(int)syscall(SYS_pidfd_open,children[i],0);assert(exit_fds[i]>=0);
        ctxs[i]=7+i;
        ctx_native[i]=(struct linux_execution_context){.fd=open("/dev/null",O_RDONLY),
            .exit_fd=exit_fds[i],.identity=70+i,.pid=children[i]};
        assert(ctx_native[i].fd>=0);
    }
    struct execution_mm *mm=test_target(7)->view.mm;
    for(unsigned i=0;i<2;i++) {
        struct vmr_mm_binding binding={.context=7+i,.mm=7,.epoch=1};
        assert(!execution_context_binding_init(&ctx_memory[i],7+i,mm,&binding,1));
        ctx_source_context[i]=7+i;
    }
    nctxs=2;memory_target target=ctx_target(7);expected=target->source;
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    sock=sockets[0];peer_socket=sockets[1];pthread_t server;
    assert(!pthread_create(&server,NULL,peer,NULL));
    int result=owner_takes_fault(target,6,0,0,0);
    assert(!pthread_join(server,NULL));close(sock);
    unsigned status=0;int pass;
    if(!strcmp(mode,"ended")) {
        pass=result==1 && src_ended_get(7,&status) && status==(SIGSEGV|128) &&
            !seen->setcpu && seen->signals[0]==1 && !seen->signals[1] &&
            linux_execution_ended(&ctx_native[0],0)==1 &&
            linux_execution_ended(&ctx_native[1],0)==0;
    } else {
        int handler=!strcmp(mode,"handler");
        pass=result==(handler ? 1:2) && !src_ended_get(7,&status) &&
            !seen->signals[0] && !seen->signals[1] &&
            seen->setcpu==(unsigned)handler && (!handler || seen->rip==0x5678);
    }
    for(unsigned i=0;i<2;i++) {
        assert(!syscall(SYS_pidfd_send_signal,exit_fds[i],SIGKILL,NULL,0));
        int st;assert(waitpid(children[i],&st,0)==children[i]);
        linux_execution_close(&ctx_native[i]);
    }
    return pass ? 0:1;
}
int main(int argc,char **argv)
{
    assert(argc==2);alarm(15);signal(SIGPIPE,SIG_IGN);mode=argv[1];
    assert(!prctl(PR_SET_CHILD_SUBREAPER,1));
    seen=mmap(NULL,sizeof(*seen),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    assert(seen!=MAP_FAILED);
    pid_t monitor=fork();assert(monitor>=0);
    if(!monitor)_exit(worker());
    int status;assert(waitpid(monitor,&status,0)==monitor);
    int bad=strcmp(mode,"ended") && strcmp(mode,"handler") && strcmp(mode,"legal");
    int pass=WIFEXITED(status) && WEXITSTATUS(status)==(bad ? 97:0);
    if(bad)pass &= !seen->setcpu && seen->signals[0]>=1 && seen->signals[1]==1;
    unsigned adopted=0;
    while(waitpid(-1,&status,0)>0)adopted++;
    assert(errno==ECHILD);
    pass &= adopted==(bad ? 2U:0U);
    printf("%s: executor exception %s checks saved task/MM, terminal framing and observed task exit before success (SETCPU=%u signals=%u/%u)\n",
        pass ? "PASS":"FAIL",mode,seen->setcpu,seen->signals[0],seen->signals[1]);
    munmap(seen,sizeof(*seen));return pass ? 0:1;
}
