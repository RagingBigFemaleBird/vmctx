// SPDX-License-Identifier: GPL-2.0
/* Native runtime API: runtime-state <run nr> <ctl nr>. */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/vmctx.h>
#include "vmctx_runtime.h"
extern char runtime_spin[];
asm(".text\n.global runtime_spin\nruntime_spin: cmp %rax,%r12; jne 1f; lock incq (%rdi); jmp runtime_spin\n1: movq $1,8(%rdi); ud2\n");
static volatile sig_atomic_t child,expired;
static long run_nr,ctl_nr;
static uint64_t *report;
static void deadline(int sig) { (void)sig;expired=1;if(child>0)kill(child,SIGKILL); }
static struct vmctx_runtime query(unsigned op) {
 return (struct vmctx_runtime){.version=VMCTX_RUNTIME_ABI,.size=sizeof(struct vmctx_runtime),.op=op};
}
static int control(unsigned op,void *arg) {
 for(unsigned i=0;i<10000&&!expired;i++) {
  int r=syscall(ctl_nr,child,op,arg);if(!r||errno!=EAGAIN)return r;usleep(100);
 }
 errno=ETIMEDOUT;return -1;
}
static int create(int source) {
 struct vmctx_run_config cfg={.flags=VMCTX_FLAG_WAIT_MONITOR|
  (source?VMCTX_FLAG_SERVICE:VMCTX_FLAG_USERCODE|VMCTX_FLAG_RESTORE|VMCTX_FLAG_REDIRECT_FAULT|VMCTX_FLAG_REDIRECT_SYSCALL),
  .backing_fd=-1,.shared_fd=-1};
 pid_t parent=getpid();child=fork();if(child<0)return -1;
 if(!child) {if(prctl(PR_SET_PDEATHSIG,SIGKILL)||getppid()!=parent)_exit(125);syscall(run_nr,&cfg);_exit(126);}
 for(unsigned i=0;i<2000&&!expired;i++) {if(!control(VMCTX_CTL_ATTACH,NULL))return 0;usleep(1000);}return -1;
}
static int reap(void) {
 if(child<=0)return -1;
 int status;kill(child,SIGKILL);pid_t p;
 do {p=waitpid(child,&status,0);}while(p<0&&errno==EINTR);
 child=0;return p>0&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL?0:-1;
}
static uint64_t clock_ns(clockid_t id) {
 struct timespec t;if(clock_gettime(id,&t))return UINT64_MAX;
 return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;
}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d: %s errno=%d expired=%d\n",__LINE__,#x,errno,expired);goto done;}}while(0)
int main(int argc,char **argv) {
 int pass=0;struct sigaction sa={.sa_handler=deadline};
 struct vmctx_runtime q,r;struct vmctx_cpu_model model;struct vmctx_cpu_state state;
 struct vmctx_reply reply={.action=VMCTX_ACT_SELF};
 if((argc!=3&&argc!=4)||sigaction(SIGALRM,&sa,NULL))return 2;
 run_nr=strtol(argv[1],NULL,10);ctl_nr=strtol(argv[2],NULL,10);alarm(30);
 report=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
 void *bad=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 CHECK(report!=MAP_FAILED&&bad!=MAP_FAILED);
 q=query(VMCTX_RUNTIME_INFO);CHECK(!syscall(ctl_nr,0,VMCTX_CTL_RUNTIME,&q)&&q.quantum_ns==VMCTX_RUNTIME_QUANTUM_NS);
 q=query(VMCTX_RUNTIME_INFO);q.reserved=1;CHECK(syscall(ctl_nr,0,VMCTX_CTL_RUNTIME,&q)==-1&&errno==EINVAL);
 CHECK(!syscall(ctl_nr,0,VMCTX_CTL_CPU_CAPS,&model));
 CHECK(!create(1)&&!control(VMCTX_CTL_CPU_MODEL,&model));
 CHECK(!control(VMCTX_CTL_GETCPU,&state));state.regs.rax=0xabcdef789123ULL;
 CHECK(!control(VMCTX_CTL_SETCPU,&state));
 q=query(VMCTX_RUNTIME_READ);CHECK(control(VMCTX_CTL_RUNTIME,&q)==-1&&errno==EOPNOTSUPP);
 q=query(VMCTX_RUNTIME_ARM);q.quantum_ns=1000000;CHECK(control(VMCTX_CTL_RUNTIME,&q)==-1&&errno==EOPNOTSUPP);
 q=query(VMCTX_RUNTIME_CREDIT);CHECK(control(VMCTX_CTL_RUNTIME,&q)==-1&&errno==EINVAL);
 q.epoch=77;CHECK(!control(VMCTX_CTL_RUNTIME,&q));
 clockid_t id;CHECK(!clock_getcpuclockid(child,&id));uint64_t before=clock_ns(id);CHECK(before!=UINT64_MAX);
 q.total_ns=500000000;CHECK(!control(VMCTX_CTL_RUNTIME,&q));uint64_t after=clock_ns(id);
 CHECK(after>=before+500000000&&after<before+505000000);
 CHECK(!control(VMCTX_CTL_RUNTIME,&q));uint64_t replay=clock_ns(id);CHECK(replay>=after&&replay<after+1000000);
 r=q;r.epoch++;CHECK(control(VMCTX_CTL_RUNTIME,&r)==-1&&errno==ESTALE);
 r=q;r.total_ns--;CHECK(control(VMCTX_CTL_RUNTIME,&r)==-1&&errno==ESTALE);
 r=q;r.total_ns=UINT64_MAX;CHECK(control(VMCTX_CTL_RUNTIME,&r)==-1&&errno==EINVAL);
 r=q;r.total_ns+=100000000;memcpy(bad,&r,sizeof(r));CHECK(!mprotect(bad,4096,PROT_READ));
 CHECK(control(VMCTX_CTL_RUNTIME,bad)==-1&&errno==EFAULT);
 CHECK(!control(VMCTX_CTL_RUNTIME,&r));after=clock_ns(id);CHECK(after>=before+600000000&&after<before+605000000);
 struct vmctx_syscall call={0};CHECK(!control(VMCTX_CTL_BOUNDARY,&call)&&call.regs.rax==state.regs.rax);
 call=(struct vmctx_syscall){.nr=1};CHECK(control(VMCTX_CTL_BOUNDARY,&call)==-1&&errno==EINVAL);
 call=(struct vmctx_syscall){.nr=SYS_getrusage,.args={RUSAGE_SELF,(uintptr_t)(report+32)}};
 CHECK(!control(VMCTX_CTL_SYSCALL,&call)&&call.ret==0);
 struct rusage *usage=(struct rusage *)(report+32);
 uint64_t used=(uint64_t)usage->ru_utime.tv_sec*1000000000ULL+usage->ru_utime.tv_usec*1000ULL;
 CHECK(used>=590000000&&used<620000000);
 CHECK(!reap());puts("PASS: source CPU credit, replay/epoch fencing, bad-copy retry, rusage and boundary AX preservation");
 memset(report,0,65536);CHECK(!create(0)&&!control(VMCTX_CTL_GETCPU,&state));
 q=query(VMCTX_RUNTIME_CREDIT);q.epoch=77;CHECK(control(VMCTX_CTL_RUNTIME,&q)==-1&&errno==EOPNOTSUPP);
 q=query(VMCTX_RUNTIME_ARM);q.quantum_ns=1;CHECK(control(VMCTX_CTL_RUNTIME,&q)==-1&&errno==EINVAL);
 q=query(VMCTX_RUNTIME_ARM);q.quantum_ns=1000000;CHECK(!control(VMCTX_CTL_RUNTIME,&q)&&q.epoch&&q.total_ns==0);
 uint64_t epoch=q.epoch,last=0,marker=argc==4?strtoull(argv[3],NULL,0):0x123456789abcdefULL;
 state.regs.rip=(uintptr_t)runtime_spin;state.regs.rdi=(uintptr_t)report;
 state.regs.rsp=(uintptr_t)report+65536-256;state.regs.rflags=0x202;state.regs.orig_rax=run_nr;
 state.regs.rax=state.regs.r12=marker;
 CHECK(!control(VMCTX_CTL_CPU_MODEL,&model)&&!control(VMCTX_CTL_SETCPU,&state));
 CHECK(!control(VMCTX_CTL_RESUME,&reply));
 for(unsigned checkpoints=0;checkpoints<12&&!expired;) {
  struct vmctx_event ev;CHECK(!control(VMCTX_CTL_WAIT,&ev));
  if(ev.type==VMCTX_EV_FAULT) {
   if(ev.nr!=14||report[1]) {
    struct vmctx_cpu_state actual;memset(&actual,0,sizeof(actual));int captured=control(VMCTX_CTL_GETCPU,&actual);
    fprintf(stderr,"unexpected runtime fault: checkpoint=%u vec=%llu rip=%llx flag=%llu captured=%d AX=%llx R12=%llx expected=%llx\n",checkpoints,(unsigned long long)ev.nr,(unsigned long long)ev.rip,(unsigned long long)report[1],captured,(unsigned long long)actual.regs.rax,(unsigned long long)actual.regs.r12,(unsigned long long)marker);
   }
   CHECK(ev.nr==14&&!report[1]);reply.action=VMCTX_ACT_SELF;CHECK(!control(VMCTX_CTL_RESUME,&reply));continue;}
  CHECK(ev.type==VMCTX_EV_RUNTIME&&!report[1]);
  q=query(VMCTX_RUNTIME_READ);CHECK(!control(VMCTX_CTL_RUNTIME,&q)&&q.epoch==epoch&&q.total_ns>=last+1000000);
  last=q.total_ns;r=query(VMCTX_RUNTIME_READ);usleep(20000);CHECK(!control(VMCTX_CTL_RUNTIME,&r)&&r.total_ns==last);
  CHECK(!control(VMCTX_CTL_GETCPU,&state)&&state.regs.rax==marker&&state.regs.r12==marker&&state.regs.orig_rax==~0ULL);
  marker+=0x1357;state.regs.rax=state.regs.r12=marker;CHECK(!control(VMCTX_CTL_SETCPU,&state));
  q=query(VMCTX_RUNTIME_ARM);q.quantum_ns=1000000;CHECK(control(VMCTX_CTL_RUNTIME,&q)==-1&&errno==EBUSY);
  reply=(struct vmctx_reply){.action=VMCTX_ACT_DONE,.retval=0xbadbadbadULL};CHECK(!control(VMCTX_CTL_RESUME,&reply));checkpoints++;
 }
 CHECK(!expired&&report[0]>100&&!report[1]&&!reap());pass=1;
 puts("PASS: execution checkpoints, CPU-time measurement excluding parked time, immutable budget and AX preservation after SETCPU");
done:
 if(child>0)reap();
 alarm(0);
 if(report!=MAP_FAILED)munmap(report,65536);
 if(bad!=MAP_FAILED)munmap(bad,4096);
 return pass?0:1;
}
