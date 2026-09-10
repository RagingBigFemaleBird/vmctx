/* SPDX-License-Identifier: GPL-2.0 */
/* A native EAGAIN is harmless only when the same retained sibling exits. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <unistd.h>
static long permission_syscall(long number,...);
#define syscall permission_syscall
#define main vmremote_program_main
#ifndef VMREMOTE_SOURCE
#define VMREMOTE_SOURCE "../../user/vmremote.c"
#endif
#include VMREMOTE_SOURCE
#undef main
#undef syscall
#define TEST_MM_REGISTRY executor_mms
#include "execution-fixture.h"
static pid_t owned_child;
static int end_sibling,owner_error,peer_error;
static unsigned applied[2];
static long permission_syscall(long number,...)
{
    if(number==SYS_gettid)return syscall(number);
    assert(number==__NR_vmctx_ctl);
    va_list ap;va_start(ap,number);int selector=va_arg(ap,int);
    unsigned command=va_arg(ap,unsigned);void *argument=va_arg(ap,void *);va_end(ap);
    assert(command==VMCTX_CTL_PROTECT_MM);
    const struct vmctx_protection *p=argument;
    assert(p->address==0x4000 && p->length==4096 && p->protection==PROT_READ);
    int slot=selector==-ctx_native[0].fd-1 ? 0:1;
    assert(selector==-ctx_native[slot].fd-1);applied[slot]++;
    if(slot==0 && !owner_error)return 0;
    if(slot==1 && end_sibling)assert(!kill(owned_child,SIGKILL));
    errno=slot==0 ? EAGAIN:peer_error;return -1;
}
int main(int argc,char **argv)
{
    assert(argc==2);alarm(10);coh_lock_init();
    end_sibling=!strcmp(argv[1],"ended");owner_error=!strcmp(argv[1],"owner");
    peer_error=!strcmp(argv[1],"error") ? EIO:EAGAIN;
    assert(end_sibling || owner_error || !strcmp(argv[1],"live") || !strcmp(argv[1],"error"));
    owned_child=fork();assert(owned_child>=0);
    if(!owned_child) {alarm(10);for(;;)pause();}
    struct execution_mm *mm=execution_mm_resolve(&executor_mms,7,0,test_mm_object,NULL);assert(mm);
    for(int i=0;i<2;i++) {
        struct vmr_mm_binding binding={.context=7+i,.mm=7,.epoch=1};
        assert(!execution_context_binding_init(&ctx_memory[i],7+i,mm,&binding,1));
        ctxs[i]=7+i;ctx_source_context[i]=7+i;
        ctx_native[i]=(struct linux_execution_context){.fd=open("/dev/null",O_RDONLY|O_CLOEXEC),
            .identity=7+i,.exit_fd=syscall(SYS_pidfd_open,i ? owned_child:getpid(),0)};
        assert(ctx_native[i].fd>=0 && ctx_native[i].exit_fd>=0);
    }
    nctxs=2;
    struct vmctx_reply reply={.map_op=VMCTX_MAP_PROT,.map_addr=0x4000,.map_len=4096,.map_prot=PROT_READ};
    int result=as_apply_map_siblings(ctx_target(7),&reply);
    int pass=end_sibling ? !result && reply.map_op==VMCTX_MAP_NONE && ctx_execution_ended(8,0):
        result<0 && reply.map_op==VMCTX_MAP_PROT && !ctx_execution_ended(8,0);
    pass &= applied[0]==1 && applied[1]==!owner_error;
    assert(!kill(owned_child,SIGKILL) || errno==ESRCH);
    int status;assert(waitpid(owned_child,&status,0)==owned_child);
    for(int i=0;i<2;i++)linux_execution_close(&ctx_native[i]);
    printf("%s: permission %s result=%d; only an observed sibling exit retires its mapping obligation\n",
        pass?"PASS":"FAIL",argv[1],result);
    return pass?0:1;
}
