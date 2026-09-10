/* SPDX-License-Identifier: GPL-2.0 */
/* A refused outbound capture must leave an incumbent fault claim free to
 * land, even while the caller retains its refused transfer ticket. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>
#include <linux/vmctx_access.h>
#include <linux/vmctx_memory.h>
#include <linux/vmctx_transfer.h>
static volatile sig_atomic_t child, expired;
static long ctl_nr;
static void deadline(int sig)
{ (void)sig; expired=1; if(child>0)kill(child,SIGKILL); }
static long control(unsigned command,void *arg)
{ return syscall(ctl_nr,child,command,arg); }
static long step(struct vmctx_transfer *t,unsigned op)
{ t->op=op;t->buf=0;return syscall(ctl_nr,0,VMCTX_CTL_TRANSFER,t); }
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno);goto done;} } while(0)
int main(int argc,char **argv)
{
    if(argc!=3)return 2;
    ctl_nr=strtol(argv[2],NULL,10);
    int pass=0,status;pid_t parent=getpid();
    void *mapping=MAP_FAILED;
    unsigned char bytes[4096],observed[4096];
    struct vmctx_transfer t={0};
    struct sigaction sa={.sa_handler=deadline};
    CHECK(!sigaction(SIGALRM,&sa,NULL));alarm(15);
    mapping=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(mapping!=MAP_FAILED);memset(mapping,0x31,4096);memset(bytes,0xa7,4096);
    child=fork();CHECK(child>=0);
    if(!child) {
        if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent || ptrace(PTRACE_TRACEME,0,NULL,NULL))_exit(125);
        raise(SIGSTOP);_exit(126);
    }
    CHECK(waitpid(child,&status,0)==child && WIFSTOPPED(status));
    CHECK(!control(VMCTX_CTL_ADOPT,NULL));CHECK(!ptrace(PTRACE_DETACH,child,NULL,NULL));
    struct vmctx_access_log info={.version=VMCTX_ACCESS_ABI,.size=sizeof(info)};
    CHECK(!control(VMCTX_CTL_ACCESS_LOG,&info) && info.mm_id);
    struct vmctx_pgset claim={.addr=(uintptr_t)mapping,.state=VMCTX_PG_CLAIM,.gen=7};
    CHECK(!control(VMCTX_CTL_PGSET,&claim));
    t=(struct vmctx_transfer){.version=VMCTX_TRANSFER_ABI,.size=sizeof(t),
        .op=VMCTX_TRANSFER_BEGIN,.mm_identity=info.mm_id,.address=(uintptr_t)mapping};
    struct vmctx_memory call={.version=VMCTX_MEMORY_ABI,.size=sizeof(call),
        .op=VMCTX_MEMORY_CALL,.command=VMCTX_CTL_TRANSFER,.mm_identity=info.mm_id,.argument=(uintptr_t)&t};
    CHECK(!control(VMCTX_CTL_MEMORY,&call) && t.ticket);
    long captured=step(&t,VMCTX_TRANSFER_CAPTURE);int capture_errno=errno;
    CHECK((captured==-1 && capture_errno==EBUSY) || (!captured && t.status==VMCTX_SERVE_CLAIMING));
    struct vmctx_land land={.addr=(uintptr_t)mapping,.buf=(uintptr_t)bytes,.gen=7};
    long landed=control(VMCTX_CTL_LAND,&land);int land_errno=errno;
    printf("OBSERVED: capture=%ld errno=%d status=%u; LAND=%ld errno=%d with refused ticket still retained\n",
        captured,captured<0?capture_errno:0,t.status,landed,landed<0?land_errno:0);
    CHECK(landed==4096);
    struct vmctx_serve state={.addr=(uintptr_t)mapping};
    CHECK(!control(VMCTX_CTL_PGSTATE,&state) && state.state==VMCTX_PG_HOME && state.gen==7);
    struct vmctx_mem read={.addr=(uintptr_t)mapping,.buf=(uintptr_t)observed,.len=4096};
    CHECK(control(VMCTX_CTL_PEEK,&read)==4096 && !memcmp(bytes,observed,4096));
    for(unsigned i=0;i<4096;i++)CHECK(((unsigned char *)mapping)[i]==0x31);
    CHECK(!step(&t,VMCTX_TRANSFER_CANCEL));CHECK(!step(&t,VMCTX_TRANSFER_FORGET));t.ticket=0;
    pass=1;
done:
    if(t.ticket) {
        if(step(&t,VMCTX_TRANSFER_CANCEL) || step(&t,VMCTX_TRANSFER_FORGET))pass=0;
    }
    if(child>0) {
        kill(child,SIGKILL);pid_t reaped;
        do {reaped=waitpid(child,&status,0);}while(reaped<0 && errno==EINTR);
        if(reaped!=child || !WIFSIGNALED(status) || WTERMSIG(status)!=SIGKILL)pass=0;
    }
    alarm(0);if(expired)pass=0;
    if(mapping!=MAP_FAILED)munmap(mapping,4096);
    printf("%s: refused capture leaves the source fault claim free to land\n",pass?"PASS":"FAIL");
    return pass?0:1;
}
