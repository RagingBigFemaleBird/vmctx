/* SPDX-License-Identifier: GPL-2.0 */
/* Actual production source page/ACK path against an adopted native MM. */
#define _GNU_SOURCE
#define main vmhome_program_main
#include "../../user/vmhome.c"
#undef main

static pid_t probe_child;
static void probe_deadline(int sig)
{(void)sig;if(probe_child>0)kill(probe_child,SIGKILL);}
static long probe_counter(const char *name)
{
    char path[256];snprintf(path,sizeof(path),"/sys/module/kernel/parameters/%s",name);
    FILE *file=fopen(path,"r");long value=-1;if(!file)return -1;
    if(fscanf(file,"%ld",&value)!=1)value=-1;
    fclose(file);return value;
}
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno);goto done;}}while(0)
int main(int argc,char **argv)
{
    if(argc!=4)return 2;
    vmctx_ctl_nr=strtol(argv[2],NULL,10);
    const char *mode=argv[3];int ro=!strcmp(mode,"readonly"),zero=!strcmp(mode,"zero"),
        stale=!strcmp(mode,"stale"),pass=0,status=0;
    source_id id=0;void *mapping=MAP_FAILED;pid_t monitor=getpid(),reaped=-1;
    struct source_custody custody={0};struct source_page_receipt receipt={0};
    struct vmr_mm_binding target={0};unsigned char bytes[4096];
    struct sigaction action={.sa_handler=probe_deadline};CHECK(!sigaction(SIGALRM,&action,NULL));
    CHECK(ro || zero || stale || !strcmp(mode,"taken"));
    CHECK(!source_memory_capabilities() && !source_transfer_caps());
    CHECK(probe_counter("vmctx_transfer_live")==0);
    long captures=probe_counter("vmctx_transfer_captures");CHECK(captures>=0);
    mapping=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(mapping!=MAP_FAILED);
    if(!zero)memset(mapping,0x63,4096);
    if(ro)CHECK(!mprotect(mapping,4096,PROT_READ));
    probe_child=fork();CHECK(probe_child>=0);
    if(!probe_child) {
        if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=monitor || ptrace(PTRACE_TRACEME,0,NULL,NULL))_exit(125);
        raise(SIGSTOP);_exit(126);
    }
    alarm(20);CHECK(waitpid(probe_child,&status,0)==probe_child && WIFSTOPPED(status));
    CHECK(!source_control_raw(probe_child,VMCTX_CTL_ADOPT,NULL));
    CHECK(!ptrace(PTRACE_DETACH,probe_child,NULL,NULL));
    struct source_context handle={.fd=-1};CHECK(!source_context_open(probe_child,&handle));
    id=source_record_add(&handle);CHECK(id>0 && handle.fd==-1);
    target=source_binding_required(id);uint64_t address=(uintptr_t)mapping;
    memset(bytes,0xcc,sizeof(bytes));
    CHECK(ctx_page(&custody,&target,address,4096,(char *)bytes,sizeof(bytes),0,&receipt)==4096);
    CHECK(receipt.episode && receipt.count==1 && receipt.taken==1 && custody.count==1);
    CHECK(vmr_binding_equal(&target,&receipt.target));
    for(unsigned i=0;i<sizeof(bytes);i++)CHECK(bytes[i]==(zero ? 0:0x63));
    CHECK(probe_counter("vmctx_transfer_live")==1 && probe_counter("vmctx_transfer_captures")==captures+1);
    CHECK(ro || pg_state(&target,address)==PG_INTRANSIT);
    struct vmr_mm_binding forged=target;forged.mm+=UINT64_C(1)<<32;
    CHECK(ctx_page_ack(&custody,&forged,address,receipt.episode)==-ESTALE && custody.count==1);
    if(stale) {
        struct vmctx_syscall remap={.nr=SYS_mmap,.args={address,4096,PROT_READ|PROT_WRITE,
            MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,UINT64_MAX,0}};
        CHECK(!source_control_raw(probe_child,VMCTX_CTL_SYSCALL,&remap) && remap.ret==(long)address);
        CHECK(ctx_page_ack(&custody,&target,address,receipt.episode)==-ESTALE);
    } else {
        CHECK(!ctx_page_ack(&custody,&target,address,receipt.episode));
        CHECK(ro || pg_state(&target,address)==PG_THEIRS);
    }
    CHECK(!custody.count && probe_counter("vmctx_transfer_live")==0);
    CHECK(ctx_page_ack(&custody,&target,address,receipt.episode)==-ENOENT);
    source_connection_close(&custody);
    pass=1;
done:
    if(custody.owned && source_custody_abandon(&custody))pass=0;
    if(probe_child>0) {
        kill(probe_child,SIGKILL);
        do {reaped=waitpid(probe_child,&status,0);}while(reaped<0 && errno==EINTR);
        if(reaped!=probe_child || !WIFSIGNALED(status) || WTERMSIG(status)!=SIGKILL)pass=0;
    }
    if(id)source_record_release(id);
    alarm(0);
    if(mapping!=MAP_FAILED)munmap(mapping,4096);
    printf("%s: production native source page %s; exact receipt and complete custody retirement\n",pass ? "PASS":"FAIL",mode);
    return pass ? 0:1;
}
