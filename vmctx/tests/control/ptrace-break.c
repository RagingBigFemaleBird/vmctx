// SPDX-License-Identifier: GPL-2.0
/* A tracer modifies a child's private executable page, observes INT3, restores
 * the instruction, and resumes it. The parent's file mapping stays unchanged. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
extern int breakpoint_probe(void);
asm(".text\n.p2align 12\n.global breakpoint_probe\nbreakpoint_probe:\n"
    "endbr64\nmov $42,%eax\nret\n.p2align 12\n");
static int wait_child(pid_t child,int *status)
{
    pid_t got;
    do {got=waitpid(child,status,0);}while(got<0 && errno==EINTR);
    return got==child;
}
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"FAIL line %d: %s errno=%d status=%x\n",__LINE__,#x,errno,status);goto done;}}while(0)
int main(void)
{
    pid_t child=fork();if(child<0)return 2;
    if(!child) {
        if(ptrace(PTRACE_TRACEME,0,NULL,NULL) || raise(SIGSTOP))_exit(2);
        _exit(breakpoint_probe()==42 ? 0:3);
    }
    int status=0,pass=0,reaped=0;
    int lifetime=(int)syscall(SYS_pidfd_open,child,0);
    struct user_regs_struct regs;
    unsigned char *address=(unsigned char *)(uintptr_t)breakpoint_probe+4;
    CHECK(lifetime>=0);
    CHECK(wait_child(child,&status) && WIFSTOPPED(status) && WSTOPSIG(status)==SIGSTOP);
    errno=0;long original=ptrace(PTRACE_PEEKTEXT,child,address,NULL);CHECK(!errno);
    CHECK((original&255)==0xb8);
    CHECK(!ptrace(PTRACE_POKETEXT,child,address,(void *)((original&~255UL)|0xcc)));
    errno=0;long patched=ptrace(PTRACE_PEEKTEXT,child,address,NULL);CHECK(!errno && (patched&255)==0xcc);
    CHECK(address[0]==0xb8 && breakpoint_probe()==42);
    CHECK(!ptrace(PTRACE_CONT,child,NULL,NULL) && wait_child(child,&status));
    CHECK(WIFSTOPPED(status) && WSTOPSIG(status)==SIGTRAP);
    CHECK(!ptrace(PTRACE_GETREGS,child,NULL,&regs));
    CHECK(regs.rip==(uintptr_t)address+1);
    siginfo_t info;CHECK(!ptrace(PTRACE_GETSIGINFO,child,NULL,&info));
    CHECK(info.si_signo==SIGTRAP && (info.si_code==SI_KERNEL || info.si_code==TRAP_BRKPT));
    CHECK(!ptrace(PTRACE_POKETEXT,child,address,(void *)original));
    errno=0;long restored=ptrace(PTRACE_PEEKTEXT,child,address,NULL);
    CHECK(!errno && restored==original);
    regs.rip=(uintptr_t)address;
    CHECK(!ptrace(PTRACE_SETREGS,child,NULL,&regs));
    CHECK(!ptrace(PTRACE_CONT,child,NULL,NULL) && wait_child(child,&status));
    if(WIFSTOPPED(status) && !ptrace(PTRACE_GETREGS,child,NULL,&regs)) {
        errno=0;long observed=ptrace(PTRACE_PEEKTEXT,child,address,NULL);
        fprintf(stderr,"unexpected stop: signal=%d rip=%llx source-text=%lx expected=%lx errno=%d\n",
                WSTOPSIG(status),regs.rip,observed,original,errno);
    }
    reaped=WIFEXITED(status)||WIFSIGNALED(status);
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status) && address[0]==0xb8);
    pass=1;
done:
    if(!reaped && lifetime>=0) {
        syscall(SYS_pidfd_send_signal,lifetime,SIGKILL,NULL,0);
        while(wait_child(child,&status) && WIFSTOPPED(status))
            ptrace(PTRACE_CONT,child,NULL,(void *)(uintptr_t)SIGKILL);
    }
    if(lifetime>=0)close(lifetime);
    if(pass)puts("PASS: source ptrace writes child text, receives INT3, restores and resumes without changing parent code");
    return pass?0:1;
}
