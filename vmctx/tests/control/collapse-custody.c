/* SPDX-License-Identifier: GPL-2.0 */
/* A THP collapse must not zero a remotely owned page's absent source PTE. */
#define _GNU_SOURCE
#define main vmhome_program_main
#include "../../user/vmhome.c"
#undef main
#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif
static pid_t collapse_child;
static volatile sig_atomic_t collapse_expired;
static void collapse_deadline(int sig)
{ (void)sig;collapse_expired=1;if(collapse_child>0)kill(collapse_child,SIGKILL); }
static int collapse_present(pid_t pid,uint64_t address)
{
    char name[80];uint64_t pte=0;
    snprintf(name,sizeof(name),"/proc/%d/pagemap",pid);
    int fd=open(name,O_RDONLY|O_CLOEXEC);if(fd<0)return -1;
    ssize_t n=pread(fd,&pte,sizeof(pte),(off_t)(address/4096*8));close(fd);
    return n==sizeof(pte) ? !!(pte&(UINT64_C(1)<<63)):-1;
}
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno);goto done;}}while(0)
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    vmctx_ctl_nr=strtol(argv[2],NULL,10);
    const size_t huge=2*1024*1024;int pass=0,status;pid_t parent=getpid();
    void *allocation=MAP_FAILED;size_t allocated=0;source_id id=0;
    unsigned char bytes[4096];struct source_custody custody={0};
    struct source_page_receipt receipt={0};struct sigaction sa={.sa_handler=collapse_deadline};
    CHECK(!sigaction(SIGALRM,&sa,NULL));CHECK(!source_memory_capabilities() && !source_transfer_caps());
    allocation=mmap(NULL,2*huge,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(allocation!=MAP_FAILED);allocated=2*huge;
    uintptr_t aligned=((uintptr_t)allocation+huge-1)&~(huge-1);
    void *region=(void *)aligned;uintptr_t page=aligned+8192;
    alarm(3);collapse_child=fork();CHECK(collapse_child>=0);
    if(!collapse_child) {
        if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent)_exit(125);
        /* These pages are born in the child: no fork aliases, no remote
         * holes, and no THP request until after the measured transfer. */
        memset(region,0x73,huge);
        if(ptrace(PTRACE_TRACEME,0,NULL,NULL))_exit(125);
        raise(SIGSTOP);_exit(126);
    }
    CHECK(waitpid(collapse_child,&status,0)==collapse_child && WIFSTOPPED(status));
    CHECK(!source_control_raw(collapse_child,VMCTX_CTL_ADOPT,NULL));
    CHECK(!ptrace(PTRACE_DETACH,collapse_child,NULL,NULL));
    struct source_context handle={.fd=-1};CHECK(!source_context_open(collapse_child,&handle));
    id=source_record_add(&handle);CHECK(id>0);
    struct vmr_mm_binding target=source_binding_required(id);
    CHECK(collapse_present(collapse_child,page)==1);
    CHECK(ctx_page(&custody,&target,page,4096,(char *)bytes,sizeof(bytes),0,&receipt)==4096);
    CHECK(pg_state(&target,page)==PG_INTRANSIT && !collapse_present(collapse_child,page));
    CHECK(!ctx_page_ack(&custody,&target,page,receipt.episode));
    CHECK(pg_state(&target,page)==PG_THEIRS);
    struct vmctx_syscall collapse={.nr=SYS_madvise,.args={aligned,huge,MADV_COLLAPSE}};
    CHECK(!ctl(id,VMCTX_CTL_SYSCALL,&collapse));
    int present=collapse_present(collapse_child,page),state=pg_state(&target,page);
    printf("OBSERVED: MADV_COLLAPSE ret=%lld record=%d source_pte_present=%d\n",
           (long long)collapse.ret,state,present);
    CHECK(state==PG_THEIRS && !present);
    CHECK((long)collapse.ret==-EBUSY || (long)collapse.ret==-EAGAIN || (long)collapse.ret==-EINVAL);
    pass=1;
done:
    if(custody.count && receipt.episode)
        if(ctx_page_ack(&custody,&receipt.target,receipt.base,receipt.episode))pass=0;
    if(custody.owned && source_custody_abandon(&custody))pass=0;
    if(collapse_child>0) {
        kill(collapse_child,SIGKILL);pid_t reaped;
        do {reaped=waitpid(collapse_child,&status,0);}while(reaped<0 && errno==EINTR);
        if(reaped!=collapse_child || !WIFSIGNALED(status) || WTERMSIG(status)!=SIGKILL)pass=0;
    }
    if(id)source_record_release(id);
    if(allocation!=MAP_FAILED)munmap(allocation,allocated);
    alarm(0);if(collapse_expired)pass=0;
    printf("%s: THP collapse respects native remote custody\n",pass?"PASS":"FAIL");
    return pass?0:1;
}
