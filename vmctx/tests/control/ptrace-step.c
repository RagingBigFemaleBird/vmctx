// SPDX-License-Identifier: GPL-2.0
/* The source owns ptrace. Single-step must travel as architectural state and
 * a debug exception; the executor must not interpret native ptrace requests. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
 volatile unsigned *value=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
 if(value==MAP_FAILED)return 2;
 pid_t child=fork();if(child<0)return 2;
 if(!child) {
  if(ptrace(PTRACE_TRACEME,0,NULL,NULL)||raise(SIGSTOP))_exit(2);
  for(unsigned i=0;i<32;i++)*value+=i+1;
  _exit(0);
 }
 int status=0,pass=0;struct user_regs_struct regs;
 if(waitpid(child,&status,0)!=child||!WIFSTOPPED(status)||WSTOPSIG(status)!=SIGSTOP)goto done;
 if(ptrace(PTRACE_GETREGS,child,NULL,&regs))goto done;
 unsigned long previous=regs.rip;unsigned moved=0;
 for(unsigned i=0;i<12;i++) {
  siginfo_t info;
  if(ptrace(PTRACE_SINGLESTEP,child,NULL,NULL)||waitpid(child,&status,0)!=child||
     !WIFSTOPPED(status)||WSTOPSIG(status)!=SIGTRAP||ptrace(PTRACE_GETREGS,child,NULL,&regs)||
     ptrace(PTRACE_GETSIGINFO,child,NULL,&info)||info.si_code!=TRAP_TRACE)goto done;
  errno=0;
  long dr6=ptrace(PTRACE_PEEKUSER,child,(void *)offsetof(struct user,u_debugreg[6]),NULL);
  if(errno||!(dr6&(1UL<<14)))goto done;
  moved+=regs.rip!=previous;previous=regs.rip;
 }
 if(moved<8||ptrace(PTRACE_CONT,child,NULL,NULL)||waitpid(child,&status,0)!=child||
    !WIFEXITED(status)||WEXITSTATUS(status)||*value!=528)goto done;
 pass=1;
done:
 if(!pass) {fprintf(stderr,"FAIL: source ptrace single-step status=%x errno=%d value=%u\n",status,errno,*value);kill(child,SIGKILL);while(waitpid(child,&status,0)<0&&errno==EINTR){}}
 munmap((void *)value,4096);
 if(pass)puts("PASS: source ptrace single-step preserves execution and returns debug stops");
 return pass?0:1;
}
