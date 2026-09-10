/* SPDX-License-Identifier: GPL-2.0 */
/* Exercise real settlement, object slots and wire receipts. Native POKE has
 * no mapping in this fixture: a sibling may own the only mapped view. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <unistd.h>
static long settlement_syscall(long number, ...);
static ssize_t settlement_pwrite(int fd,const void *buf,size_t count,off_t offset);
#define syscall settlement_syscall
#define pwrite settlement_pwrite
#define main vmremote_program_main
#ifndef VMREMOTE_SOURCE
#define VMREMOTE_SOURCE "../../user/vmremote.c"
#endif
#include VMREMOTE_SOURCE
#undef main
#undef syscall
#undef pwrite
#define TEST_MM_REGISTRY executor_mms
#include "execution-fixture.h"

static memory_target target;
static int peer_fd, bytes, hole;
static int short_write, readonly, drift;
static unsigned *observed_ack;
static off_t slot;
static unsigned acknowledgements;
static int acknowledged_storage;
static const uint64_t base=0x4000, alias=0x9000;
static const uint64_t episode=UINT64_C(0x8000000100000042);
static ssize_t settlement_pwrite(int fd,const void *buf,size_t count,off_t offset)
{
    if(short_write && fd==shared_obj && count==4096)count=64;
    return pwrite(fd,buf,count,offset);
}
static long settlement_syscall(long number, ...)
{
    if(number==SYS_gettid)return syscall(number);
    assert(number==__NR_vmctx_ctl);
    va_list ap;va_start(ap,number);int selector=va_arg(ap,int);
    unsigned command=va_arg(ap,unsigned);(void)va_arg(ap,void *);va_end(ap);
    assert(selector==-ctx_native[0].fd-1);
    if(command==VMCTX_CTL_TAKEOBJ)return 0;
    errno=EFAULT;return -1;
}
static int slot_contains(unsigned char expected)
{
    unsigned char data[4096];
    if(pread(shared_obj,data,sizeof(data),slot)!=sizeof(data))return 0;
    for(size_t i=0;i<sizeof(data);i++)if(data[i]!=expected)return 0;
    return 1;
}
static void *peer(void *unused)
{
    (void)unused;struct vmr_req request;
    assert(!pg_rw(peer_fd,&request,sizeof(request),0));
    assert(request.nr==VMR_OP_CTXPAGE && request.args[0]==base && request.args[1]==4096);
    assert(vmr_binding_equal(&request.target,&target->source));
    struct vmr_rsp reply={.magic=VMR_MAGIC,.binding=target->source,
        .retval=bytes ? 4096:VMR_CTXPAGE_ABSENT};
    vmr_memory_receipt(&request,&reply);
    if(drift) {
        /* A callback replaces this mapping while the pull is in flight. */
        uint64_t other=UINT64_C(0x100000006);
        assert(shared_page_slot(shared_obj,other,0,1)>=0);
        shared_range_note(target,base,4096,0,PROT_READ,other);
    }
    unsigned char data[4096];memset(data,0x63,sizeof(data));
    if(bytes) {reply.datalen=4096;reply.ngen=1;reply.pages_taken=1;reply.page_episode=episode;}
    assert(!pg_rw(peer_fd,&reply,sizeof(reply),1));
    if(bytes)assert(!pg_rw(peer_fd,data,sizeof(data),1));
    while(!pg_rw(peer_fd,&request,sizeof(request),0)) {
        assert(bytes && request.nr==VMR_OP_INSTALLED && request.args[0]==base &&
            request.args[1]==episode && vmr_binding_equal(&request.target,&target->source));
        acknowledgements++;
        __atomic_add_fetch(observed_ack,1,__ATOMIC_RELAXED);
        acknowledged_storage=slot_contains(0x63);
        reply=(struct vmr_rsp){.magic=VMR_MAGIC,.binding=target->source};
        vmr_memory_receipt(&request,&reply);assert(!pg_rw(peer_fd,&reply,sizeof(reply),1));
    }
    close(peer_fd);return NULL;
}
int main(int argc,char **argv)
{
    assert(argc==2);alarm(10);signal(SIGPIPE,SIG_IGN);coh_lock_init();
    short_write=!strcmp(argv[1],"short");readonly=!strcmp(argv[1],"readonly");
    drift=!strcmp(argv[1],"replaced");
    int read_slot=!strcmp(argv[1],"read-slot"),read_hole=!strcmp(argv[1],"read-hole");
    int rejected=short_write || readonly || drift;
    bytes=!strcmp(argv[1],"bytes") || rejected;hole=!strcmp(argv[1],"hole") || read_hole;
    assert(bytes || hole || read_slot || !strcmp(argv[1],"absent"));
    struct execution_mm *mm=execution_mm_resolve(&executor_mms,7,0,test_mm_object,NULL);assert(mm);
    struct vmr_mm_binding binding={.context=7,.mm=7,.epoch=1};
    assert(!execution_context_binding_init(&ctx_memory[0],7,mm,&binding,1));
    ctxs[0]=7;ctx_source_context[0]=7;nctxs=1;target=ctx_target(7);
    ctx_native[0]=(struct linux_execution_context){.fd=open("/dev/null",O_RDONLY|O_CLOEXEC),.identity=1,.exit_fd=-1};
    assert(ctx_native[0].fd>=0);
    shared_obj=memfd_create("settlement-shared",MFD_CLOEXEC);assert(shared_obj>=0);
    const uint64_t object=UINT64_C(0x100000005),offset=0x37000;
    shared_range_note(target,base,4096,offset,PROT_READ,object);
    shared_range_note(target,alias,4096,offset,PROT_READ,object);
    slot=shared_page_slot(shared_obj,object,offset,1);assert(slot>=0 && slot!=(off_t)base);
    assert(shared_obj_off(target,base)==slot && shared_obj_off(target,alias)==slot);
    unsigned char original[4096],actual[4096];memset(original,0x31,sizeof(original));
    if(!hole)assert(pwrite(shared_obj,original,sizeof(original),slot)==sizeof(original));
    if(read_slot || read_hole) {
        /* No context maps this page. A private decoy must never answer a
         * service read of the retained shared object, including its holes. */
        memset(actual,0x92,sizeof(actual));
        assert(obj_write(target,base,actual,sizeof(actual))==sizeof(actual));
        int found=page_read_as(target,base,actual);
        int pass=read_hole ? !found : found && !memcmp(original,actual,sizeof(actual));
        printf("%s: page service %s resolves shared storage without a mapped context or private fallback\n",
            pass?"PASS":"FAIL",argv[1]);
        return pass?0:1;
    }
    /* A private page at the same VA must not count as a shared landing. */
    if(bytes || hole)assert(obj_write(target,base,original,sizeof(original))==sizeof(original));
    page_lend_at(target,base,44,1,__func__,__LINE__);
    observed_ack=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    assert(observed_ack!=MAP_FAILED);
    if(readonly) {
        char path[80];snprintf(path,sizeof(path),"/proc/self/fd/%d",shared_obj);
        int ro=open(path,O_RDONLY|O_CLOEXEC);assert(ro>=0);
        assert(dup2(ro,shared_obj)==shared_obj);close(ro);
    }
    if(rejected) {
        pid_t worker=fork();assert(worker>=0);
        if(worker) {
            int status;assert(waitpid(worker,&status,0)==worker);
            int pass=WIFEXITED(status) && WEXITSTATUS(status)==97 && !*observed_ack;
            pass &= pread(mm->backing_fd,actual,sizeof(actual),base)==sizeof(actual) &&
                !memcmp(original,actual,sizeof(actual));
            printf("%s: shared settlement %s stops before ACK and private fallback (status=%d ACKs=%u)\n",
                pass?"PASS":"FAIL",argv[1],status,*observed_ack);
            return pass?0:1;
        }
        alarm(10);
    }
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    sock=sockets[0];peer_fd=sockets[1];pthread_t server;
    assert(!pthread_create(&server,NULL,peer,NULL));
    int result=page_recall_all(target);
    ack_flush();close(sock);sock=-1;assert(!pthread_join(server,NULL));
    int pass=!coh_depth && acknowledgements==(unsigned)bytes;
    if(hole)pass &= result<0 && page_holder(target,base)==44 && !shared_obj_has(slot);
    else pass &= !result && !page_holder(target,base) && slot_contains(bytes ? 0x63:0x31);
    if(bytes)pass &= acknowledged_storage;
    if(bytes || hole)pass &= pread(mm->backing_fd,actual,sizeof(actual),base)==sizeof(actual) &&
        !memcmp(original,actual,sizeof(actual));
    struct shared_range mapping;
    assert(shared_layout_lookup(7,alias,&mapping));pass &= mapping.prot==PROT_READ;
    printf("%s: shared settlement %s result=%d ACKs=%u; bytes follow the shared slot, private decoys grant nothing\n",
        pass?"PASS":"FAIL",argv[1],result,acknowledgements);
    close(ctx_native[0].fd);close(shared_obj);return pass?0:1;
}
