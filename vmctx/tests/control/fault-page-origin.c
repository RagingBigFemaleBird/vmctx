/* SPDX-License-Identifier: GPL-2.0 */
/* Observe production fault landing and wire ACK. A protected local fork
 * snapshot supplies bytes without creating a source transfer episode. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <unistd.h>
static long origin_syscall(long number,...);
#define syscall origin_syscall
#define main vmremote_program_main
#ifndef VMREMOTE_SOURCE
#define VMREMOTE_SOURCE "../../user/vmremote.c"
#endif
#include VMREMOTE_SOURCE
#undef main
#undef syscall
#define TEST_MM_REGISTRY executor_mms
#include "execution-fixture.h"
static memory_target child;
static int source_bytes,peer_fd,object_only,loan_map,watched_local,no_landing;
static unsigned acknowledgements,native_writes;
static const uint64_t episode=UINT64_C(0x8000000100000042);
static long origin_syscall(long number,...)
{
    if(number==SYS_gettid)return syscall(number);
    assert(number==__NR_vmctx_ctl);
    va_list ap;va_start(ap,number);int selector=va_arg(ap,int);
    unsigned command=va_arg(ap,unsigned);void *arg=va_arg(ap,void *);va_end(ap);
    assert(selector==-ctx_native[0].fd-1);
    if(command==VMCTX_CTL_MAPOBJ_READ && watched_local) {
        struct vmctx_mem *m=arg;
        assert(m->addr==0x4000 && m->len==4096 && !m->buf);
        return 4096;
    }
    if(command==VMCTX_CTL_MAPOBJ && loan_map)return 4096;
    if(command==VMCTX_CTL_POKE) {
        struct vmctx_mem *m=arg;assert(m->addr==0x4000 && m->len==4096);
        if(object_only) {errno=EFAULT;return -1;}
        native_writes++;
        return pwrite(child->view.mm->backing_fd,(void *)(uintptr_t)m->buf,4096,0x4000);
    }
    errno=EFAULT;return -1; /* no native present mapping in this fixture */
}
static void *peer(void *unused)
{
    (void)unused;struct vmr_req request;
    if(watched_local) {
        /* The source has no transfer to offer. A watched folio remains
         * local, so even asking for it is an ownership-routing failure. */
        if(!pg_rw(peer_fd,&request,sizeof(request),0)) {
            fputs("FAIL: watched local page was requested from source\n",stderr);
            close(peer_fd);return NULL;
        }
        close(peer_fd);return NULL;
    }
    assert(!pg_rw(peer_fd,&request,sizeof(request),0));
    assert(request.nr==VMR_OP_CTXPAGE && request.args[0]==0x4000 && request.args[1]==4096);
    assert(vmr_binding_equal(&request.target,&child->source));
    struct vmr_rsp reply={.magic=VMR_MAGIC,.binding=child->source,
        .retval=source_bytes ? 4096:VMR_CTXPAGE_COWBREAK};
    vmr_memory_receipt(&request,&reply);
    unsigned char data[4096];memset(data,0x63,sizeof(data));
    if(source_bytes) {reply.datalen=4096;reply.ngen=1;reply.pages_taken=1;reply.page_episode=episode;}
    assert(!pg_rw(peer_fd,&reply,sizeof(reply),1));
    if(source_bytes)assert(!pg_rw(peer_fd,data,sizeof(data),1));
    while(!pg_rw(peer_fd,&request,sizeof(request),0)) {
        assert(source_bytes && request.nr==VMR_OP_INSTALLED && request.args[0]==0x4000 &&
            request.args[1]==episode && vmr_binding_equal(&request.target,&child->source));
        acknowledgements++;
        reply=(struct vmr_rsp){.magic=VMR_MAGIC,.binding=child->source};
        vmr_memory_receipt(&request,&reply);assert(!pg_rw(peer_fd,&reply,sizeof(reply),1));
    }
    close(peer_fd);return NULL;
}
int main(int argc,char **argv)
{
    assert(argc==2);alarm(10);signal(SIGPIPE,SIG_IGN);coh_lock_init();
    no_landing=strstr(argv[1],"no-landing")!=NULL;
    if(no_landing) {
        pid_t worker=fork();assert(worker>=0);
        if(worker) {
            int status;assert(waitpid(worker,&status,0)==worker);
            int actual=WIFEXITED(status) ? WEXITSTATUS(status):-1;
            printf("%s: %s exits before continuation when no landing can consume the receipt (exit=%d)\n",
                actual==97 ? "PASS":"FAIL",argv[1],actual);
            return actual==97 ? 0:1;
        }
        alarm(10);
    }
    source_bytes=!strncmp(argv[1],"source",6);object_only=strstr(argv[1],"object")!=NULL;
    object_only |= no_landing;
    loan_map=!strcmp(argv[1],"source-loan-map");
    watched_local=!strncmp(argv[1],"watched-",8);
    int region=strstr(argv[1],"region")!=NULL;
    memory_target parent=test_target(7);
    struct execution_mm *mm=execution_mm_resolve(&executor_mms,8,7,test_mm_object,NULL);assert(mm);
    struct vmr_mm_binding binding={.context=8,.mm=8,.parent_mm=7,.epoch=1};
    assert(!execution_context_binding_init(&ctx_memory[0],8,mm,&binding,1));
    ctxs[0]=8;ctx_source_context[0]=8;nctxs=1;child=ctx_target(8);
    ctx_native[0]=(struct linux_execution_context){.fd=open("/dev/null",O_RDONLY|O_CLOEXEC),.identity=1,.exit_fd=-1};
    assert(ctx_native[0].fd>=0);
    if(no_landing) {
        char path[80];snprintf(path,sizeof(path),"/proc/self/fd/%d",mm->backing_fd);
        int readonly=open(path,O_RDONLY|O_CLOEXEC);assert(readonly>=0);
        close(mm->backing_fd);mm->backing_fd=readonly;
    }
    unsigned char original[4096],landed[4096];memset(original,0x53,sizeof(original));
    if(watched_local) {
        assert(pwrite(mm->backing_fd,original,4096,0x4000)==4096);
        page_watch_set(child,0x4000,1);
        cow_prot_set(8,0x4000,COW_PROT_OWED);
    } else if(!source_bytes) {
        assert(pwrite(parent->view.mm->backing_fd,original,4096,0x4000)==4096);
        cow_prot_set(7,0x4000,COW_PROT_OWED);
    }
    if(loan_map) {
        memset(original,0x63,sizeof(original));
        assert(pwrite(mm->backing_fd,original,4096,0x4000)==4096);
        page_lend(child,0x4000,44);
    }
    if(region)region_add(8,0x4000,0x5000,PROT_READ|PROT_WRITE);
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    sock=sockets[0];peer_fd=sockets[1];pthread_t server;
    assert(!pthread_create(&server,NULL,peer,NULL));
    int result=fault_from_home_inner(child,14,0x4000,(loan_map || strstr(argv[1],"write")) ? 6:
        strstr(argv[1],"exec") ? 20:4,1,0);
    ack_flush();close(sock);sock=-1;assert(!pthread_join(server,NULL));
    assert(pread(mm->backing_fd,landed,4096,0x4000)==4096);
    int pass=result==1 && !coh_depth && acknowledgements==(unsigned)source_bytes;
    if(object_only)pass=result==0 && n_decl_object_fresh==1 && !fresh_object_fill &&
        !coh_depth && acknowledgements==1;
    for(unsigned i=0;i<4096;i++)pass &= landed[i]==(source_bytes ? 0x63:0x53);
    if(watched_local)pass &= !native_writes && page_watched(child,0x4000) &&
        cow_prot_state(8,0x4000)==COW_PROT_OWED && page_is_installed(child,0x4000);
    else if(!source_bytes)pass &= cow_was_given(8,0x4000);
    printf("%s: fault origin %s result=%d ACKs=%u native_writes=%u; source receipt and protected local snapshot stay distinct\n",
        pass ? "PASS":"FAIL",argv[1],result,acknowledgements,native_writes);
    close(ctx_native[0].fd);return pass ? 0:1;
}
