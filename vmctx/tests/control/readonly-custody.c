/* SPDX-License-Identifier: GPL-2.0 */
/* Source transfer must revoke a private read-only page before publishing it.
 * Native COW must preserve VMA permissions, fork aliases and file contents. */
#define _GNU_SOURCE
#define main vmhome_program_main
#include "../../user/vmhome.c"
#undef main
static pid_t readonly_child;
static volatile sig_atomic_t readonly_expired;
static void readonly_deadline(int sig)
{(void)sig;readonly_expired=1;if(readonly_child>0)kill(readonly_child,SIGKILL);}
static int readonly_present(pid_t pid,uint64_t address)
{
    char name[80];uint64_t pte=0;
    snprintf(name,sizeof(name),"/proc/%d/pagemap",pid);
    int fd=open(name,O_RDONLY|O_CLOEXEC);if(fd<0)return -1;
    ssize_t n=pread(fd,&pte,sizeof(pte),(off_t)(address/4096*8));close(fd);
    return n==sizeof(pte) ? !!(pte&(UINT64_C(1)<<63)):-1;
}
static int readonly_protection(pid_t pid,uint64_t address)
{
    char name[80],line[512],perms[5];unsigned long lo,hi;
    snprintf(name,sizeof(name),"/proc/%d/maps",pid);
    FILE *f=fopen(name,"r");if(!f)return -1;
    int result=-1;
    while(fgets(line,sizeof(line),f))
        if(sscanf(line,"%lx-%lx %4s",&lo,&hi,perms)==3 && address>=lo && address<hi) {
            result=(perms[0]=='r'?PROT_READ:0)|(perms[1]=='w'?PROT_WRITE:0)|(perms[2]=='x'?PROT_EXEC:0);break;
        }
    fclose(f);return result;
}
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno);goto done;}}while(0)
int main(int argc,char **argv)
{
    if(argc!=4)return 2;
    vmctx_ctl_nr=strtol(argv[2],NULL,10);
    const char *mode=argv[3];int zero=!strcmp(mode,"zero"),anon=!strcmp(mode,"anon"),
        executable=!strcmp(mode,"executable"),pass=0,status,fd=-1;
    void *page=MAP_FAILED;unsigned char seed[4096],bytes[4096],file[4096];
    source_id id=0;struct source_custody custody={0};struct source_page_receipt receipt={0};
    struct sigaction sa={.sa_handler=readonly_deadline};pid_t parent=getpid();
    CHECK(!sigaction(SIGALRM,&sa,NULL));CHECK(zero || anon || executable || !strcmp(mode,"file"));
    CHECK(!source_memory_capabilities() && !source_transfer_caps());
    for(unsigned i=0;i<4096;i++)seed[i]=zero ? 0:(unsigned char)(i*37+19);
    if(!zero && !anon) {
        fd=memfd_create("readonly-custody",MFD_CLOEXEC);CHECK(fd>=0 && write(fd,seed,4096)==4096);
    }
    page=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|(fd<0 ? MAP_ANONYMOUS:0),fd,0);
    CHECK(page!=MAP_FAILED);
    if(anon)memcpy(page,seed,4096);
    if(zero) {volatile unsigned char read_zero=*(volatile unsigned char *)page;CHECK(!read_zero);}
    if(fd>=0)CHECK(!memcmp(page,seed,4096)); /* read faults in the file, no private write */
    int prot=PROT_READ|(executable?PROT_EXEC:0);CHECK(!mprotect(page,4096,prot));
    readonly_child=fork();CHECK(readonly_child>=0);
    if(!readonly_child) {
        /* Fork may omit read-only file PTEs without an anon_vma. Establish
         * the child's own native read before adoption; never write/COW it. */
        if(*(volatile unsigned char *)page!=seed[0])_exit(124);
        if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent || ptrace(PTRACE_TRACEME,0,NULL,NULL))_exit(125);
        raise(SIGSTOP);_exit(126);
    }
    alarm(10);CHECK(waitpid(readonly_child,&status,0)==readonly_child && WIFSTOPPED(status));
    CHECK(!source_control_raw(readonly_child,VMCTX_CTL_ADOPT,NULL));CHECK(!ptrace(PTRACE_DETACH,readonly_child,NULL,NULL));
    struct source_context handle={.fd=-1};CHECK(!source_context_open(readonly_child,&handle));
    id=source_record_add(&handle);CHECK(id>0);struct vmr_mm_binding target=source_binding_required(id);
    CHECK(readonly_present(readonly_child,(uintptr_t)page)==1);
    CHECK(readonly_protection(readonly_child,(uintptr_t)page)==prot);
    if(zero) {
        /* Being the monitor alone cannot authorize zero-page COW. POKE
         * already takes a page guard, but only TAKE scopes this operation. */
        char foreign=0x5a;
        struct vmctx_mem poke={.addr=(uintptr_t)page,.len=1,.buf=(uintptr_t)&foreign};
        errno=0;
        CHECK(ctl(id,VMCTX_CTL_POKE,&poke)==-1 && errno==EFAULT);
        CHECK(readonly_present(readonly_child,(uintptr_t)page)==1);
        CHECK(pg_state(&target,(uintptr_t)page)==PG_NONE);
        CHECK(!memcmp(seed,page,4096));
    }
    CHECK(ctx_page(&custody,&target,(uintptr_t)page,4096,(char *)bytes,sizeof(bytes),0,&receipt)==4096);
    /* Check native state before ACK; matching bytes cannot prove ownership. */
    printf("OBSERVED: readonly %s record=%d source_pte_present=%d episode=%llu\n",mode,
           pg_state(&target,(uintptr_t)page),readonly_present(readonly_child,(uintptr_t)page),
           (unsigned long long)receipt.episode);
    CHECK(pg_state(&target,(uintptr_t)page)==PG_INTRANSIT);
    CHECK(readonly_present(readonly_child,(uintptr_t)page)==0);
    CHECK(readonly_protection(readonly_child,(uintptr_t)page)==prot);
    CHECK(!memcmp(seed,bytes,4096) && !memcmp(seed,page,4096));
    CHECK(!ctx_page_ack(&custody,&target,(uintptr_t)page,receipt.episode));
    CHECK(pg_state(&target,(uintptr_t)page)==PG_THEIRS);
    struct vmctx_syscall upgrade={.nr=SYS_mprotect,.args={(uintptr_t)page,4096,PROT_READ|PROT_WRITE}};
    CHECK(!ctl(id,VMCTX_CTL_SYSCALL,&upgrade) && !upgrade.ret);
    CHECK(pg_state(&target,(uintptr_t)page)==PG_THEIRS && !readonly_present(readonly_child,(uintptr_t)page));
    CHECK(!memcmp(seed,page,4096));
    if(fd>=0)CHECK(pread(fd,file,4096,0)==4096 && !memcmp(seed,file,4096));
    pass=1;
done:
    /* A failed old-kernel baseline still owns its original receipt. Settle
     * it explicitly, then tear down; never abandon a delivered grant. */
    if(custody.count && receipt.episode)
        if(ctx_page_ack(&custody,&receipt.target,receipt.base,receipt.episode))pass=0;
    if(custody.owned && source_custody_abandon(&custody))pass=0;
    if(readonly_child>0) {
        kill(readonly_child,SIGKILL);pid_t reaped;
        do {reaped=waitpid(readonly_child,&status,0);}while(reaped<0 && errno==EINTR);
        if(reaped!=readonly_child || !WIFSIGNALED(status) || WTERMSIG(status)!=SIGKILL)pass=0;
    }
    if(id)source_record_release(id);
    alarm(0);
    if(fd>=0)close(fd);
    if(page!=MAP_FAILED)munmap(page,4096);
    if(readonly_expired)pass=0;
    printf("%s: private read-only %s custody revokes PTE before ACK; permissions, aliases and file unchanged; upgrade preserves remote ownership\n",pass?"PASS":"FAIL",mode);
    return pass?0:1;
}
