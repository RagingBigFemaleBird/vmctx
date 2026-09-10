// SPDX-License-Identifier: GPL-2.0
/* A retained owner's gate QUERY must survive native exit/monitor retirement. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>

static long ctl_nr;
static int selector;
static atomic_int stop_readers, readers_ready, failure;
static atomic_ulong queries;
static long ctl(unsigned op, void *arg) { return syscall(ctl_nr, selector, op, arg); }
static void *query(void *unused)
{
    (void)unused;
    atomic_fetch_add(&readers_ready, 1);
    while (!atomic_load(&stop_readers)) {
        struct vmctx_syscall_gate q = {.version=VMCTX_SYSCALL_GATE_ABI,
            .size=sizeof(q),.op=VMCTX_GATE_QUERY,.ticket=1};
        int r=ctl(VMCTX_CTL_SYSCALL_GATE,&q),e=errno;
        if (r && e!=EBUSY && e!=EAGAIN) {
            atomic_store(&failure,e);
            break;
        }
        if (!r) atomic_fetch_add(&queries,1);
    }
    return NULL;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL iteration=%u line=%d: %s errno=%d query_error=%d\n",iteration,__LINE__,#x,errno,atomic_load(&failure)); goto cleanup; } } while(0)
int main(int argc,char **argv)
{
    if (argc!=4) return 2;
    long run_nr=strtol(argv[1],NULL,10);ctl_nr=strtol(argv[2],NULL,10);
    unsigned rounds=strtoul(argv[3],NULL,10),completed=0;
    if (!rounds || rounds>10000) return 2;
    for (unsigned iteration=0;iteration<rounds;iteration++) {
        pid_t child=-1;int pidfd=-1,fd=-1,status=0,started=0,pass=0;
        pthread_t readers[4];
        atomic_store(&stop_readers,0);atomic_store(&readers_ready,0);atomic_store(&failure,0);
        struct vmctx_run_config cfg={.flags=VMCTX_FLAG_SERVICE|VMCTX_FLAG_WAIT_MONITOR,
            .backing_fd=-1,.shared_fd=-1};
        pid_t parent=getpid();
        child=fork();CHECK(child>=0);
        if (!child) {
            if (prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent) _exit(125);
            syscall(run_nr,&cfg);_exit(126);
        }
        pidfd=syscall(SYS_pidfd_open,child,0);CHECK(pidfd>=0);
        for (unsigned n=0;syscall(ctl_nr,child,VMCTX_CTL_ATTACH,NULL);n++) {
            CHECK(n<20000);usleep(100);
        }
        struct vmctx_context info={.version=VMCTX_CONTEXT_ABI,.size=sizeof(info),.op=VMCTX_CONTEXT_OPEN};
        CHECK(!syscall(ctl_nr,child,VMCTX_CTL_CONTEXT,&info));fd=info.fd;selector=-(fd+1);
        struct vmctx_cpu_state cpu;
        for (unsigned n=0;ctl(VMCTX_CTL_GETCPU,&cpu);n++) {CHECK(errno==EAGAIN && n<20000);usleep(100);}
        cpu.regs.orig_rax=SYS_exit;cpu.regs.rdi=37;
        CHECK(!ctl(VMCTX_CTL_SETCPU,&cpu));
        struct vmctx_syscall_gate gate={.version=VMCTX_SYSCALL_GATE_ABI,.size=sizeof(gate),.ticket=1,.op=VMCTX_GATE_BEGIN};
        CHECK(!ctl(VMCTX_CTL_SYSCALL_GATE,&gate));gate.op=VMCTX_GATE_QUERY;
        for (unsigned n=0;;n++) {
            CHECK(n<20000);
            int r=ctl(VMCTX_CTL_SYSCALL_GATE,&gate);
            CHECK(!r || errno==EAGAIN || errno==EBUSY);
            if (!r && gate.state==VMCTX_GATE_ADMITTED) break;
            usleep(100);
        }
        CHECK(gate.call.nr==SYS_exit && gate.call.args[0]==37);
        for (unsigned i=0;i<4;i++) {CHECK(!pthread_create(&readers[i],NULL,query,NULL));started++;}
        while (atomic_load(&readers_ready)!=4) sched_yield();
        gate.op=VMCTX_GATE_COMMIT;
        for (unsigned n=0;ctl(VMCTX_CTL_SYSCALL_GATE,&gate);n++) {CHECK(errno==EBUSY && n<20000);usleep(100);}
        CHECK(waitpid(child,&status,0)==child);child=-1;
        CHECK(WIFEXITED(status) && WEXITSTATUS(status)==37);
        atomic_store(&stop_readers,1);
        for (int i=0;i<started;i++) CHECK(!pthread_join(readers[i],NULL));
        started=0;
        CHECK(!atomic_load(&failure));
        info=(struct vmctx_context){.version=VMCTX_CONTEXT_ABI,.size=sizeof(info),.op=VMCTX_CONTEXT_INFO};
        CHECK(!ctl(VMCTX_CTL_CONTEXT,&info) && (info.flags&VMCTX_CONTEXT_ENDED));
        pass=1;
cleanup:
        if (child>0 && pidfd>=0) {syscall(SYS_pidfd_send_signal,pidfd,SIGKILL,NULL,0);while(waitpid(child,&status,0)<0 && errno==EINTR) {}}
        atomic_store(&stop_readers,1);
        for (int i=0;i<started;i++) pthread_join(readers[i],NULL);
        if (fd>=0) close(fd);
        if (pidfd>=0) close(pidfd);
        if (!pass) return 1;
        completed++;
    }
    printf("PASS: %u native exits raced retained gate QUERY; %lu successful queries, no lost authorization\n",completed,atomic_load(&queries));
    return 0;
}
