// SPDX-License-Identifier: GPL-2.0
/* Native restore must not execute or return through an installed guest frame
 * unless its monitor explicitly releases it. restore-gate <run nr> <ctl nr>. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>
extern char unreleased_guest[];
asm(".text\n.global unreleased_guest\nunreleased_guest: lock incq (%rdi); jmp unreleased_guest\n");
static volatile sig_atomic_t child;
static void deadline(int sig) { (void)sig;if(child>0)kill(child,SIGKILL); }
static long ctl_nr;
static int control(unsigned op,void *arg) {
 for(unsigned i=0;i<10000;i++) {
  int r=syscall(ctl_nr,child,op,arg);if(!r||errno!=EAGAIN)return r;usleep(100);
 }
 errno=ETIMEDOUT;return -1;
}
int main(int argc,char **argv) {
 if(argc!=3)return 2;
 long run_nr=strtol(argv[1],NULL,10);ctl_nr=strtol(argv[2],NULL,10);
 struct sigaction sa={.sa_handler=deadline};if(sigaction(SIGALRM,&sa,NULL))return 2;
 uint64_t *counter=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
 if(counter==MAP_FAILED)return 2;
 struct vmctx_cpu_model model;struct vmctx_cpu_state state;
 if(syscall(ctl_nr,0,VMCTX_CTL_CPU_CAPS,&model))return 2;
 struct vmctx_run_config cfg={.flags=VMCTX_FLAG_USERCODE|VMCTX_FLAG_WAIT_MONITOR|
  VMCTX_FLAG_RESTORE|VMCTX_FLAG_REDIRECT_FAULT|VMCTX_FLAG_REDIRECT_SYSCALL,
  .backing_fd=-1,.shared_fd=-1};
 pid_t parent=getpid();child=fork();if(child<0)return 2;
 if(!child) {
  if(prctl(PR_SET_PDEATHSIG,SIGKILL)||getppid()!=parent)_exit(125);
  syscall(run_nr,&cfg);_exit(126); /* Returning here is also a broken gate. */
 }
 alarm(12);int attached=0,pass=0,status=0;pid_t result=0;
 for(unsigned i=0;i<2000;i++) {if(!control(VMCTX_CTL_ATTACH,NULL)){attached=1;break;}usleep(1000);}
 if(!attached||control(VMCTX_CTL_GETCPU,&state))goto done;
 state.regs.rip=(uintptr_t)unreleased_guest;state.regs.rdi=(uintptr_t)counter;
 state.regs.rsp=(uintptr_t)counter+65536-256;state.regs.rflags=0x202;state.regs.orig_rax=~0ULL;
 if(control(VMCTX_CTL_CPU_MODEL,&model)||control(VMCTX_CTL_SETCPU,&state))goto done;
 /* The native attachment timeout is five seconds. Never issue RESUME. */
 for(unsigned i=0;i<6500;i++) {
  result=waitpid(child,&status,WNOHANG);if(result)break;
  usleep(1000);
 }
 if(result==child) {child=0;pass=WIFEXITED(status)&&WEXITSTATUS(status)==ETIMEDOUT&&!*counter;}
 fprintf(pass?stdout:stderr,"%s: unreleased restore result=%ld status=%#x guest_counter=%llu\n",
  pass?"PASS":"FAIL",(long)result,status,(unsigned long long)*counter);
done:
 if(child>0){kill(child,SIGKILL);while(waitpid(child,&status,0)<0&&errno==EINTR){}child=0;}
 alarm(0);munmap(counter,65536);return pass?0:1;
}
