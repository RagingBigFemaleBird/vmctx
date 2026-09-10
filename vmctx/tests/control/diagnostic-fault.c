/* SPDX-License-Identifier: GPL-2.0 */
/* Real native source write holds a shared stderr file-position lock while
 * waiting for a page. The fault owner emits diagnostics before landing it.
 * diagnostic-fault <run-nr> <ctl-nr> <sink-file>; baseline build intentionally
 * retains synchronous diagnostics and must fail before the kernel deadline. */
#define _GNU_SOURCE
#define main vmhome_program_main
#include "../../user/vmhome.c"
#undef main

static pid_t fault_child;
static volatile sig_atomic_t fault_expired;
static source_id fault_source;
static struct vmctx_syscall fault_write;
static long fault_result;
static void fault_deadline(int sig)
{(void)sig;fault_expired=1;if(fault_child>0)kill(fault_child,SIGKILL);}
static void *fault_caller(void *unused)
{(void)unused;fault_result=ctl(fault_source,VMCTX_CTL_SYSCALL,&fault_write);return NULL;}
#define CHECK(x) do {if(!(x)) {dprintf(1,"FAIL line %d: %s errno=%d expired=%d\n",__LINE__,#x,errno,fault_expired);goto done;}}while(0)
int main(int argc,char **argv)
{
    if(argc!=4)return 2;
    vmctx_ctl_nr=strtol(argv[2],NULL,10);
    int pass=0,status,started=0,fd=-1;pthread_t caller;
    pid_t parent=getpid();void *page=MAP_FAILED;
    struct source_custody custody={0};struct source_page_receipt receipt={0};
    unsigned char bytes[4096];struct vmr_mm_binding target;
    struct sigaction sa={.sa_handler=fault_deadline};CHECK(!sigaction(SIGALRM,&sa,NULL));
    signal(SIGPIPE,SIG_IGN);setvbuf(stderr,NULL,_IONBF,0);
    fd=open(argv[3],O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0600);CHECK(fd>=0 && dup2(fd,2)==2);
#ifndef VMCTX_DIAGNOSTICS_BASELINE
    CHECK(!monitor_diagnostics_start());
#endif
    CHECK(!source_memory_capabilities() && !source_transfer_caps());
    page=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(page!=MAP_FAILED);memset(page,'g',4096);((char *)page)[4095]='\n';
    fault_child=fork();CHECK(fault_child>=0);
    if(!fault_child) {
        if(monitor_diagnostics_child() || prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent ||
           ptrace(PTRACE_TRACEME,0,NULL,NULL))_exit(125);
        raise(SIGSTOP);_exit(126);
    }
    alarm(3);
    CHECK(waitpid(fault_child,&status,0)==fault_child && WIFSTOPPED(status));
    CHECK(!source_control_raw(fault_child,VMCTX_CTL_ADOPT,NULL));
    CHECK(!ptrace(PTRACE_DETACH,fault_child,NULL,NULL));
    struct source_context handle={.fd=-1};CHECK(!source_context_open(fault_child,&handle));
    fault_source=source_record_add(&handle);CHECK(fault_source>0);
    target=source_binding_required(fault_source);
    CHECK(!ctl(fault_source,VMCTX_CTL_ATTACH,NULL));
    CHECK(ctx_page(&custody,&target,(uintptr_t)page,4096,(char *)bytes,sizeof(bytes),0,&receipt)==4096);
    CHECK(!ctx_page_ack(&custody,&target,(uintptr_t)page,receipt.episode));
    CHECK(pg_state(&target,(uintptr_t)page)==PG_THEIRS);
    fault_write=(struct vmctx_syscall){.nr=SYS_write,.args={2,(uintptr_t)page,4096}};
    CHECK(!pthread_create(&caller,NULL,fault_caller,NULL));started=1;
    struct vmctx_event event;CHECK(!ctl(fault_source,VMCTX_CTL_WAIT,&event));
    CHECK(event.type==VMCTX_EV_FAULT && (event.fault_addr&~UINT64_C(4095))==(uintptr_t)page);
    dprintf(1,"OBSERVED: source stderr write fault on transferred page; emitting monitor diagnostic before landing\n");
    fprintf(stderr,"[vmhome] diagnostic emitted while source output waits for its page\n");
    CHECK(!fault_expired);
    /* WAIT already owns the native fault claim. A second RECALL_BEGIN must
     * refuse that incumbent; land under the claim exactly as ctx_monitor. */
    CHECK(ctx_land(&target,(uintptr_t)page,bytes,0,0)==4096);
    struct vmctx_reply reply={.action=VMCTX_ACT_DONE};
    CHECK(!ctl(fault_source,VMCTX_CTL_RESUME,&reply));
    CHECK(!pthread_join(caller,NULL));started=0;
    CHECK(!fault_result && fault_write.ret==4096 && !fault_expired);
    pass=1;
done:
    if(custody.owned && source_custody_abandon(&custody))pass=0;
    if(fault_child>0) {
        kill(fault_child,SIGKILL);
        if(started)pthread_join(caller,NULL);
        pid_t reaped;do {reaped=waitpid(fault_child,&status,0);}while(reaped<0 && errno==EINTR);
        if(reaped!=fault_child || !WIFSIGNALED(status) || WTERMSIG(status)!=SIGKILL)pass=0;
    }
    if(fault_source)source_record_release(fault_source);
    alarm(0);
    /* Only now is it safe to drain synchronously: the source is reaped and
     * cannot hold this sink. Production fault service never waits on logs. */
    struct monitor_diagnostics *d=&monitor_diagnostics;
    if(d->sink>=0) {
        stderr=d->original;fclose(d->stream);dup2(d->sink,2);close(d->sender);
        pthread_join(d->worker,NULL);close(d->receiver);close(d->sink);
        if(atomic_load(&d->dropped_bytes) || atomic_load(&d->sink_errors))pass=0;
    }
    if(pass) {
        char observed[8192]={0};
        ssize_t n=pread(fd,observed,sizeof(observed)-1,0);
        if(n<=0 || !strstr(observed,"[vmhome] diagnostic emitted while source output waits for its page\n") ||
           !strstr(observed,(const char *)"gggggggggggggggg"))pass=0;
    }
    if(fd>=0)close(fd);
    if(page!=MAP_FAILED)munmap(page,4096);
    dprintf(1,"%s: fault-service diagnostic completes while source stderr holds its output lock; expired=%d\n",
            pass ? "PASS":"FAIL",fault_expired);
    return pass ? 0:1;
}
