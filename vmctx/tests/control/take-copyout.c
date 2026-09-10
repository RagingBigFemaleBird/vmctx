// SPDX-License-Identifier: GPL-2.0
/* Native TAKE: successful locked-page transfer, and fail-closed termination
 * if a monitor copyout fails after destructive removal. <run nr> <ctl nr>. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>
static volatile sig_atomic_t child,expired;
static long run_nr,ctl_nr;
static void deadline(int sig){(void)sig;expired=1;if(child>0)kill(child,SIGKILL);}
static int control(unsigned op,void *arg) {
 for(unsigned i=0;i<10000&&!expired;i++) {
  int r=syscall(ctl_nr,child,op,arg);if(r>=0||errno!=EAGAIN)return r;usleep(100);
 }
 errno=ETIMEDOUT;return -1;
}
static int create(void) {
 struct vmctx_run_config cfg={.flags=VMCTX_FLAG_WAIT_MONITOR|VMCTX_FLAG_SERVICE,.backing_fd=-1,.shared_fd=-1};
 pid_t parent=getpid();child=fork();if(child<0)return -1;
 if(!child){if(prctl(PR_SET_PDEATHSIG,SIGKILL)||getppid()!=parent)_exit(125);syscall(run_nr,&cfg);_exit(126);}
 for(unsigned i=0;i<2000&&!expired;i++){if(!control(VMCTX_CTL_ATTACH,NULL))return 0;usleep(1000);}return -1;
}
static void reap(void){if(child>0){int s;kill(child,SIGKILL);while(waitpid(child,&s,0)<0&&errno==EINTR){}child=0;}}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line %d: %s errno=%d expired=%d\n",__LINE__,#x,errno,expired);goto done;}}while(0)
int main(int argc,char **argv) {
 if(argc!=3)return 2;
 run_nr=strtol(argv[1],NULL,10);ctl_nr=strtol(argv[2],NULL,10);
 struct sigaction sa={.sa_handler=deadline};if(sigaction(SIGALRM,&sa,NULL))return 2;
 unsigned char *data=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 if(data==MAP_FAILED)return 2;
 struct vmctx_cpu_model model;int pass=0;alarm(20);
 CHECK(!syscall(ctl_nr,0,VMCTX_CTL_CPU_CAPS,&model));
 for(unsigned bad=0;bad<2;bad++) {
  CHECK(!create()&&!control(VMCTX_CTL_CPU_MODEL,&model));
  struct vmctx_syscall c={.nr=SYS_mmap,.args={0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,(uint64_t)-1,0}};
  CHECK(!control(VMCTX_CTL_SYSCALL,&c)&&c.ret>0);uint64_t address=c.ret;
  c=(struct vmctx_syscall){.nr=SYS_mlock,.args={address,4096}};
  CHECK(!control(VMCTX_CTL_SYSCALL,&c)&&!c.ret);
  memset(data,0x69,4096);
  struct vmctx_mem m={.addr=address,.buf=(uintptr_t)data,.len=4096};
  CHECK(control(VMCTX_CTL_POKE,&m)==4096);
  memset(data,0xa5,4096);if(bad)CHECK(!mprotect(data,4096,PROT_READ));
  int r=control(VMCTX_CTL_TAKE,&m);
  if(!bad) {CHECK(r==4096);for(unsigned i=0;i<4096;i++)CHECK(data[i]==0x69);reap();}
  else {
   CHECK(r==-1&&errno==ENOTRECOVERABLE);
   int status;pid_t p;do{p=waitpid(child,&status,0);}while(p<0&&errno==EINTR);
   CHECK(p==child&&!expired&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);child=0;
  }
 }
 pass=1;puts("PASS: locked-page TAKE preserves bytes; destructive bad copyout terminates its owner");
done:
 reap();alarm(0);munmap(data,4096);return pass?0:1;
}
