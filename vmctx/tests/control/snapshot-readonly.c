/* SPDX-License-Identifier: GPL-2.0 */
/* A fork inherits private bytes even when their current mapping is read-only.
 * Exercise the production snapshot walk and strict copy gate with real sparse
 * objects; only the local native protection operation is supplied here. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
static long snapshot_syscall(long number,...);
#define syscall snapshot_syscall
#define main vmremote_program_main
#ifndef VMREMOTE_SOURCE
#define VMREMOTE_SOURCE "../../user/vmremote.c"
#endif
#include VMREMOTE_SOURCE
#undef main
#undef syscall
#define TEST_MM_REGISTRY executor_mms
#include "execution-fixture.h"
static unsigned protections;
static long snapshot_syscall(long number,...)
{
    assert(number==__NR_vmctx_ctl);
    va_list ap;va_start(ap,number);int selector=va_arg(ap,int);
    unsigned command=va_arg(ap,unsigned);struct vmctx_mem *memory=va_arg(ap,void *);va_end(ap);
    assert(selector==-ctx_native[0].fd-1 && command==VMCTX_CTL_PROTECT_BACKING);
    assert(memory->addr==0x4000 && memory->len==4096 && !memory->buf);
    protections++;return 4096;
}
int main(int argc,char **argv)
{
    assert(argc==2);alarm(10);coh_lock_init();
    int executable=!strcmp(argv[1],"executable"),readonly=!strcmp(argv[1],"readonly"),
        shared=!strcmp(argv[1],"shared");
    assert(executable || readonly || shared);
    memory_target parent=test_target(7);
    struct execution_mm *child=execution_mm_resolve(&executor_mms,8,7,test_mm_object,NULL);
    assert(child && child->parent==parent->view.mm);
    unsigned char original[4096],after[4096],changed[4096];
    memset(original,0x53,sizeof(original));memset(changed,0xb7,sizeof(changed));
    assert(pwrite(parent->view.mm->backing_fd,original,4096,0x4000)==4096);
    region_add(7,0x4000,0x5000,PROT_READ|(executable ? PROT_EXEC:0));
    if(shared)shared_range_note(parent,0x4000,4096,0,VMR_PROT_READ,91);
    ctx_native[0]=(struct linux_execution_context){.fd=open("/dev/null",O_RDONLY|O_CLOEXEC),.identity=1,.exit_fd=-1};
    assert(ctx_native[0].fd>=0);ctxs[0]=ctx_id(parent);nctxs=1;
    assert(!cow_protect_address_space(parent));
    nctxs=0; /* The retained objects remain usable with no execution member. */
    int protected=cow_is_protected(7,0x4000);
    int given=cow_give_page(8,7,0x4000);
    /* The parent may subsequently receive write permission or retire its
     * mapping. The child's strict snapshot must already own the old bytes. */
    assert(pwrite(parent->view.mm->backing_fd,changed,4096,0x4000)==4096);
    assert(pread(child->backing_fd,after,4096,0x4000)==4096);
    int pass=shared ? !protected && !protections && !given && !cow_was_given(8,0x4000) :
        protected && protections==1 && given && !memcmp(original,after,4096);
    printf("%s: %s fork page protected=%d native_protections=%u strict_copy=%d snapshot_matches=%d\n",
        pass ? "PASS":"FAIL",argv[1],protected,protections,given,!memcmp(original,after,4096));
    close(ctx_native[0].fd);return pass ? 0:1;
}
