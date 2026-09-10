// SPDX-License-Identifier: GPL-2.0
/* Native execution adapter: quiesce-state <run nr> <ctl nr>.
 * A guest counter is sampled only after BEGIN has acknowledged exclusion.
 * Cover nested leases, stale identity/token, bad copyout and owner task exit. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>
#include "vmctx_quiesce.h"
extern char quiesce_spin[];
asm(".text\n.global quiesce_spin\nquiesce_spin: lock incq (%rdi); jmp quiesce_spin\n");
static volatile sig_atomic_t child, expired;
static long ctl_nr;
static uint64_t *counter;
static uint64_t orphan_token;
static int owner_result;
static void deadline(int sig) { (void)sig; expired=1; if(child>0) kill(child,SIGKILL); }
static struct vmctx_quiesce query(unsigned op)
{
 return (struct vmctx_quiesce){.version=VMCTX_QUIESCE_ABI,.size=sizeof(struct vmctx_quiesce),.op=op};
}
static int ctl(unsigned op,void *arg)
{
 for(unsigned i=0;i<10000 && !expired;i++) {
  int r=syscall(ctl_nr,child,op,arg);
  if(!r || errno!=EAGAIN) return r;
  usleep(100);
 }
 errno=ETIMEDOUT;return -1;
}
static int progressing(void)
{
 uint64_t before=__atomic_load_n(counter,__ATOMIC_SEQ_CST);
 for(unsigned i=0;i<2000 && !expired;i++) {
  if(__atomic_load_n(counter,__ATOMIC_SEQ_CST)!=before) return 1;
  usleep(1000);
 }
 return 0;
}
static int stopped(void)
{
 uint64_t before=__atomic_load_n(counter,__ATOMIC_SEQ_CST);
 usleep(20000);
 return __atomic_load_n(counter,__ATOMIC_SEQ_CST)==before;
}
static void *owner(void *unused)
{
 (void)unused;
 struct vmctx_quiesce q=query(VMCTX_QUIESCE_BEGIN);
 owner_result=!ctl(VMCTX_CTL_QUIESCE,&q) && stopped();
 orphan_token=q.token;
 return NULL; /* native task exit must release the lease */
}
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"FAIL line %d: %s errno=%d expired=%d\n",__LINE__,#x,errno,expired);goto done;} }while(0)
int main(int argc,char **argv)
{
 int pass=0,status,attached=0,backing=-1;pthread_t thread;
 struct sigaction sa={.sa_handler=deadline};
 struct vmctx_cpu_model model;struct vmctx_cpu_state state;
 struct vmctx_quiesce a=query(VMCTX_QUIESCE_INFO),b,q;
 void *stack=MAP_FAILED,*bad=MAP_FAILED;
 if(argc!=3 || sigaction(SIGALRM,&sa,NULL)) return 2;
 long run_nr=strtol(argv[1],NULL,10);ctl_nr=strtol(argv[2],NULL,10);
 CHECK(!syscall(ctl_nr,0,VMCTX_CTL_QUIESCE,&a));
 counter=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
 stack=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 bad=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 CHECK(counter!=MAP_FAILED && stack!=MAP_FAILED && bad!=MAP_FAILED);
 *counter=0;
 CHECK(!syscall(ctl_nr,0,VMCTX_CTL_CPU_CAPS,&model));
 backing=memfd_create("quiesce-unmapped",MFD_CLOEXEC);
 uint64_t expected=0x123456789abcdef0ULL,actual=0;
 CHECK(backing>=0 && !ftruncate(backing,12288));
 CHECK(pwrite(backing,&expected,sizeof(expected),4096)==sizeof(expected));
 struct vmctx_run_config cfg={.flags=VMCTX_FLAG_USERCODE|VMCTX_FLAG_WAIT_MONITOR|
  VMCTX_FLAG_RESTORE|VMCTX_FLAG_REDIRECT_FAULT|VMCTX_FLAG_REDIRECT_SYSCALL,
  .backing_fd=backing,.shared_fd=-1};
 pid_t parent=getpid();child=fork();CHECK(child>=0);
 if(!child) {
  if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent) _exit(125);
  syscall(run_nr,&cfg);_exit(126);
 }
 alarm(20);
 for(unsigned i=0;i<2000 && !expired;i++) {
  if(!ctl(VMCTX_CTL_ATTACH,NULL)) {attached=1;break;}
  usleep(1000);
 }
 CHECK(attached && !ctl(VMCTX_CTL_GETCPU,&state));
 state.regs.rip=(uintptr_t)quiesce_spin;state.regs.rdi=(uintptr_t)counter;
 state.regs.rsp=(uintptr_t)stack+65536-256;state.regs.rflags=0x202;state.regs.orig_rax=~0ULL;
 CHECK(!ctl(VMCTX_CTL_CPU_MODEL,&model) && !ctl(VMCTX_CTL_SETCPU,&state));
 struct vmctx_reply reply={.action=VMCTX_ACT_SELF};
 CHECK(!ctl(VMCTX_CTL_RESUME,&reply));
 /* Fork may leave an inherited mapping without a populated child PTE.
  * Service these native demand faults before measuring hardware exclusion. */
 for(unsigned i=0;i<2000 && !*counter && !expired;i++) {
  struct vmctx_event ev;
  int r=syscall(ctl_nr,child,VMCTX_CTL_WAIT,&ev);
  if(!r) {
   CHECK(ev.type==VMCTX_EV_FAULT && ev.nr==14);
   CHECK(!ctl(VMCTX_CTL_RESUME,&reply));
  } else CHECK(errno==EAGAIN);
  usleep(1000);
 }
 CHECK(progressing());
 struct vmctx_mem page={.addr=4096,.len=4096};
 CHECK(ctl(VMCTX_CTL_PROTECT_BACKING,&page)==-1 && errno==EPERM);
 for(unsigned i=0;i<16;i++) {
  a=query(VMCTX_QUIESCE_BEGIN);CHECK(!ctl(VMCTX_CTL_QUIESCE,&a) && a.token && a.mm_id && stopped());
  CHECK(!ctl(VMCTX_CTL_PROTECT_BACKING,&page));
  CHECK(pread(backing,&actual,sizeof(actual),4096)==sizeof(actual) && actual==expected);
  struct vmctx_mem hole={.addr=8192,.len=4096};
  CHECK(!ctl(VMCTX_CTL_PROTECT_BACKING,&hole));
  CHECK(lseek(backing,8192,SEEK_DATA)==-1 && errno==ENXIO);
  b=query(VMCTX_QUIESCE_BEGIN);b.mm_id=a.mm_id;CHECK(!ctl(VMCTX_CTL_QUIESCE,&b) && b.token!=a.token);
  a.op=VMCTX_QUIESCE_END;CHECK(!syscall(ctl_nr,0,VMCTX_CTL_QUIESCE,&a) && stopped());
  CHECK(syscall(ctl_nr,0,VMCTX_CTL_QUIESCE,&a)==-1 && errno==ESTALE);
  q=query(VMCTX_QUIESCE_BEGIN);q.mm_id=b.mm_id+1;
  CHECK(ctl(VMCTX_CTL_QUIESCE,&q)==-1 && errno==ESTALE);
  b.op=VMCTX_QUIESCE_END;CHECK(!syscall(ctl_nr,0,VMCTX_CTL_QUIESCE,&b) && progressing());
 }
 *(struct vmctx_quiesce *)bad=query(VMCTX_QUIESCE_BEGIN);
 CHECK(!mprotect(bad,4096,PROT_READ));
 CHECK(ctl(VMCTX_CTL_QUIESCE,bad)==-1 && errno==EFAULT && progressing());
 CHECK(!pthread_create(&thread,NULL,owner,NULL));
 CHECK(!pthread_join(thread,NULL) && owner_result && progressing());
 q=query(VMCTX_QUIESCE_END);q.token=orphan_token;
 CHECK(syscall(ctl_nr,0,VMCTX_CTL_QUIESCE,&q)==-1 && errno==ESTALE);
 pass=1;
done:
 if(child>0) {kill(child,SIGKILL);while(waitpid(child,&status,0)<0 && errno==EINTR) {}}
 alarm(0);
 if(counter && counter!=MAP_FAILED) munmap(counter,4096);
 if(stack!=MAP_FAILED) munmap(stack,65536);
 if(bad!=MAP_FAILED) munmap(bad,4096);
 if(backing>=0) close(backing);
 if(pass) puts("PASS: unmapped backing protection without allocation, guest execution exclusion, nested leases, identity/token fencing, bad-copy cleanup and monitor owner exit");
 return pass?0:1;
}
