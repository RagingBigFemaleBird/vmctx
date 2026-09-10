/* SPDX-License-Identifier: GPL-2.0 */
/* Native custody regressions: elapsed time is not an installation receipt;
 * identical bytes from two transfers do not make their ACKs interchangeable.
 * The current legacy ABI cannot pass both. Preserve measured native failures. */
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
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>
#include <linux/vmctx_recall.h>
static volatile sig_atomic_t child,expired;
static long ctl_nr;
static void deadline(int sig) {(void)sig;expired=1;if(child>0)kill(child,SIGKILL);}
static long control(unsigned op,void *arg) {return syscall(ctl_nr,child,op,arg);}
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"FAIL line %d: %s errno=%d expired=%d\n",__LINE__,#x,errno,(int)expired);goto done;}} while(0)
int main(int argc,char **argv)
{
    if(argc!=4)return 2;
    ctl_nr=strtol(argv[2],NULL,10);const char *mode=argv[3];
    int shared=!strcmp(mode,"stale-ack"),pass=0,status=0,fd=-1;
    pid_t parent=getpid(),reaped=-1;void *mapping=MAP_FAILED;
    unsigned char bytes[4096],received[4096];memset(bytes,0x63,sizeof(bytes));
    struct sigaction sa={.sa_handler=deadline};if(sigaction(SIGALRM,&sa,NULL))return 2;
    if(shared) {fd=memfd_create("serve-episode",MFD_CLOEXEC);CHECK(fd>=0 && !ftruncate(fd,4096));}
    mapping=mmap(NULL,4096,PROT_READ|PROT_WRITE,shared?MAP_SHARED:MAP_PRIVATE|MAP_ANONYMOUS,fd,0);
    CHECK(mapping!=MAP_FAILED);memcpy(mapping,bytes,sizeof(bytes));
    child=fork();CHECK(child>=0);
    if(!child) {
        if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent || ptrace(PTRACE_TRACEME,0,NULL,NULL))_exit(125);
        raise(SIGSTOP);_exit(126);
    }
    alarm(20);CHECK(waitpid(child,&status,0)==child && WIFSTOPPED(status));
    CHECK(!control(VMCTX_CTL_ADOPT,NULL));CHECK(!ptrace(PTRACE_DETACH,child,NULL,NULL));
    struct vmctx_serve first={.addr=(uintptr_t)mapping,.buf=(uintptr_t)received};
    CHECK(!control(VMCTX_CTL_SERVE,&first));
    CHECK(first.status==(shared?VMCTX_SERVE_COPIED:VMCTX_SERVE_TAKEN));
    CHECK(!memcmp(bytes,received,sizeof(bytes)));
    struct vmctx_serve state={.addr=(uintptr_t)mapping};
    CHECK(!control(VMCTX_CTL_PGSTATE,&state) && state.state==VMCTX_PG_TRANSIT);
    if(shared) {
        struct vmctx_pgack old={.addr=(uintptr_t)mapping,.sum=first.sum};
        CHECK(control(VMCTX_CTL_PGACK,&old)==1);
        struct vmctx_recall recall={.addr=(uintptr_t)mapping};
        CHECK(!control(VMCTX_CTL_RECALL,&recall));
        recall.op=VMCTX_RECALL_COMMIT;recall.buf=(uintptr_t)bytes;recall.gen=73;
        CHECK(syscall(ctl_nr,0,VMCTX_CTL_RECALL,&recall)==4096);
        struct vmctx_serve second={.addr=(uintptr_t)mapping,.buf=(uintptr_t)received};
        CHECK(!control(VMCTX_CTL_SERVE,&second) && second.status==VMCTX_SERVE_COPIED);
        CHECK(second.flags&VMCTX_SERVE_GRANT);CHECK(first.sum==second.sum);
        long acked=control(VMCTX_CTL_PGACK,&old);int saved=errno;
        CHECK(!control(VMCTX_CTL_PGSTATE,&state));
        printf("stale-ack old_sum=%u new_sum=%u old_gen=%u new_gen=%u ack_result=%ld errno=%d state=%u\n",
            first.sum,second.sum,first.gen,second.gen,acked,saved,state.state);
        CHECK(acked==0 && state.state==VMCTX_PG_TRANSIT);
    } else {
        usleep(250000);
        struct vmctx_serve retry={.addr=(uintptr_t)mapping,.buf=(uintptr_t)received};
        CHECK(!control(VMCTX_CTL_SERVE,&retry));CHECK(!control(VMCTX_CTL_PGSTATE,&state));
        printf("unacked-age first_status=%u retry_status=%u state=%u\n",first.status,retry.status,state.state);
        CHECK(retry.status==VMCTX_SERVE_CLAIMING && state.state==VMCTX_PG_TRANSIT);
    }
    pass=1;
done:
    if(child>0) {kill(child,SIGKILL);do {reaped=waitpid(child,&status,0);}while(reaped<0 && errno==EINTR);}
    alarm(0);if(mapping!=MAP_FAILED)munmap(mapping,4096);if(fd>=0)close(fd);
    printf("%s: native serve episode %s\n",pass?"PASS":"FAIL",mode);
    return pass && !expired && reaped==child ? 0:1;
}
