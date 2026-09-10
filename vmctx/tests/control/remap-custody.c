/* SPDX-License-Identifier: GPL-2.0 */
/* Moving a mapping must move its remote residency, not erase its provenance. */
#define _GNU_SOURCE
#define main vmhome_program_main
#include "../../user/vmhome.c"
#undef main
static pid_t remap_child;
static volatile sig_atomic_t remap_expired;
static void remap_deadline(int sig)
{ (void)sig;remap_expired=1;if(remap_child>0)kill(remap_child,SIGKILL); }
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno);goto done;}}while(0)
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    vmctx_ctl_nr=strtol(argv[2],NULL,10);
    int pass=0,status;pid_t parent=getpid();source_id id=0;
    void *old=MAP_FAILED,*destination=MAP_FAILED;unsigned char bytes[4096];
    struct source_custody custody={0};struct source_page_receipt receipt={0};
    struct sigaction sa={.sa_handler=remap_deadline};
    CHECK(!sigaction(SIGALRM,&sa,NULL));CHECK(!source_memory_capabilities() && !source_transfer_caps());
    old=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    destination=mmap(NULL,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(old!=MAP_FAILED && destination!=MAP_FAILED);memset(old,0x73,4096);
    alarm(3);remap_child=fork();CHECK(remap_child>=0);
    if(!remap_child) {
        if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent ||
           ptrace(PTRACE_TRACEME,0,NULL,NULL))_exit(125);
        raise(SIGSTOP);_exit(126);
    }
    CHECK(waitpid(remap_child,&status,0)==remap_child && WIFSTOPPED(status));
    CHECK(!source_control_raw(remap_child,VMCTX_CTL_ADOPT,NULL));
    CHECK(!ptrace(PTRACE_DETACH,remap_child,NULL,NULL));
    struct source_context handle={.fd=-1};CHECK(!source_context_open(remap_child,&handle));
    id=source_record_add(&handle);CHECK(id>0);
    struct vmr_mm_binding target=source_binding_required(id);
    CHECK(ctx_page(&custody,&target,(uintptr_t)old,4096,(char *)bytes,sizeof(bytes),0,&receipt)==4096);
    CHECK(pg_state(&target,(uintptr_t)old)==PG_INTRANSIT);
    CHECK(!ctx_page_ack(&custody,&target,(uintptr_t)old,receipt.episode));
    CHECK(pg_state(&target,(uintptr_t)old)==PG_THEIRS);
    struct vmctx_syscall move={.nr=SYS_mremap,.args={(uintptr_t)old,4096,4096,
        MREMAP_MAYMOVE|MREMAP_FIXED,(uintptr_t)destination}};
    CHECK(!ctl(id,VMCTX_CTL_SYSCALL,&move) && (uint64_t)move.ret==(uintptr_t)destination);
    int prior=pg_state(&target,(uintptr_t)old),next=pg_state(&target,(uintptr_t)destination);
    printf("OBSERVED: mremap success prior_record=%d destination_record=%d\n",prior,next);
    CHECK(prior==PG_NONE && next==PG_THEIRS);
    pass=1;
done:
    if(custody.count && receipt.episode)
        if(ctx_page_ack(&custody,&receipt.target,receipt.base,receipt.episode))pass=0;
    if(custody.owned && source_custody_abandon(&custody))pass=0;
    if(remap_child>0) {
        kill(remap_child,SIGKILL);pid_t reaped;
        do {reaped=waitpid(remap_child,&status,0);}while(reaped<0 && errno==EINTR);
        if(reaped!=remap_child || !WIFSIGNALED(status) || WTERMSIG(status)!=SIGKILL)pass=0;
    }
    if(id)source_record_release(id);
    if(old!=MAP_FAILED)munmap(old,4096);
    if(destination!=MAP_FAILED)munmap(destination,4096);
    alarm(0);if(remap_expired)pass=0;
    printf("%s: mremap preserves native remote custody\n",pass?"PASS":"FAIL");
    return pass?0:1;
}
