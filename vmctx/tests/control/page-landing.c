/* SPDX-License-Identifier: GPL-2.0 */
/* A native POKE may commit a prefix before encountering an unmapped page.
 * The production landing path must not publish the entire page as installed. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <unistd.h>
static long landing_syscall(long number,...);
#define syscall landing_syscall
#define main vmremote_program_main
#ifndef VMREMOTE_SOURCE
#define VMREMOTE_SOURCE "../../user/vmremote.c"
#endif
#include VMREMOTE_SOURCE
#undef main
#undef syscall
#define TEST_MM_REGISTRY executor_mms
#include "execution-fixture.h"
static size_t native_limit;
static unsigned char native_bytes[4096];
static long landing_syscall(long number,...)
{
    assert(number==__NR_vmctx_ctl);
    va_list ap;va_start(ap,number);int selector=va_arg(ap,int);
    unsigned command=va_arg(ap,unsigned);void *arg=va_arg(ap,void *);va_end(ap);
    assert(selector==-ctx_native[0].fd-1 && command==VMCTX_CTL_POKE);
    struct vmctx_mem *memory=arg;assert(memory->len==4096 && memory->addr==0x4000);
    memcpy(native_bytes,(void *)(uintptr_t)memory->buf,native_limit);
    return native_limit;
}

int main(void)
{
    alarm(10);coh_lock_init();int pass=1;
    unsigned char bytes[4096];memset(bytes,0x51,sizeof(bytes));
    const size_t limits[]={1,4095,4096};
    for(unsigned i=0;i<sizeof(limits)/sizeof(*limits);i++) {
        memory_target target=test_target(7+i);
        char path[80];snprintf(path,sizeof(path),"/proc/self/fd/%d",target->view.mm->backing_fd);
        int readonly=open(path,O_RDONLY|O_CLOEXEC);assert(readonly>=0);
        close(target->view.mm->backing_fd);target->view.mm->backing_fd=readonly;
        ctx_native[0]=(struct linux_execution_context){.fd=open("/dev/null",O_RDONLY|O_CLOEXEC),.identity=1,.exit_fd=-1};
        assert(ctx_native[0].fd>=0);ctxs[0]=ctx_id(target);nctxs=1;
        retain_put(target,0x4000,bytes);native_limit=limits[i];memset(native_bytes,0xcc,sizeof(native_bytes));
        int result=fatal_retained_rescue(target,0x4000);
        int complete=native_limit==4096,installed=page_is_installed(target,0x4000);
        for(unsigned j=0;j<4096;j++)assert(native_bytes[j]==(j<native_limit ? 0x51:0xcc));
        int good=result==complete && !!installed==complete;
        printf("%s: native write committed %zu/4096; landing result=%d installed=%d\n",
            good?"PASS":"FAIL",native_limit,result,installed);pass&=good;
        close(ctx_native[0].fd);nctxs=0;
    }
    return pass?0:1;
}
