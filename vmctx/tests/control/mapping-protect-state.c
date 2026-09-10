// SPDX-License-Identifier: GPL-2.0
/* The monitor's explicit protection operation changes the target mm, skips its lazy
 * holes, reports invalid requests, and leaves the monitor's mappings alone. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>
#include "vmctx_executor_map.h"
static pid_t child;
static void expired(int sig) { (void)sig; if (child > 0) kill(child, SIGKILL); }
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno); goto out; } } while (0)
static int protection(pid_t pid, uintptr_t address)
{
    char path[64],line[512],access[5]; unsigned long lo,hi;
    snprintf(path,sizeof(path),"/proc/%d/maps",pid);
    FILE *f=fopen(path,"r"); int result=-1;
    if (!f) return -1;
    while (fgets(line,sizeof(line),f))
        if (sscanf(line,"%lx-%lx %4s",&lo,&hi,access)==3 && address>=lo && address<hi) {
            result=(access[0]=='r'?PROT_READ:0)|(access[1]=='w'?PROT_WRITE:0)|(access[2]=='x'?PROT_EXEC:0); break;
        }
    fclose(f); return result;
}
int main(int argc,char **argv)
{
    if (argc!=2) return 2;
    long ctl=strtol(argv[1],NULL,10); int status,ok=0;
    pid_t parent=getpid();
    unsigned char *p=mmap(NULL,12288,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(p!=MAP_FAILED); p[0]=0x51; p[8192]=0x63;
    child=fork(); CHECK(child>=0);
    if (!child) {
        if (prctl(PR_SET_PDEATHSIG,SIGKILL)||getppid()!=parent||munmap(p+4096,4096)||ptrace(PTRACE_TRACEME,0,NULL,NULL)) _exit(125);
        raise(SIGSTOP); _exit(126);
    }
    signal(SIGALRM,expired); alarm(20);
    CHECK(waitpid(child,&status,0)==child && WIFSTOPPED(status));
    CHECK(!syscall(ctl,child,VMCTX_CTL_ADOPT,NULL));
    CHECK(!ptrace(PTRACE_DETACH,child,NULL,NULL));
    struct vmctx_protection rep={.address=(uintptr_t)p,.length=12288,.protection=PROT_READ};
    CHECK(!syscall(ctl,child,VMCTX_CTL_PROTECT_MM,&rep));
    CHECK(protection(child,(uintptr_t)p)==PROT_READ && protection(child,(uintptr_t)(p+8192))==PROT_READ);
    CHECK(protection(child,(uintptr_t)(p+4096))==-1 && protection(getpid(),(uintptr_t)p)==(PROT_READ|PROT_WRITE));
    p[0]=0x75;
    rep.protection=PROT_NONE; CHECK(!syscall(ctl,child,VMCTX_CTL_PROTECT_MM,&rep));
    CHECK(protection(child,(uintptr_t)p)==0);
    rep.protection=PROT_READ|PROT_WRITE; CHECK(!syscall(ctl,child,VMCTX_CTL_PROTECT_MM,&rep));
    CHECK(protection(child,(uintptr_t)p)==(PROT_READ|PROT_WRITE));
    rep.address++; CHECK(syscall(ctl,child,VMCTX_CTL_PROTECT_MM,&rep)==-1 && errno==EINVAL);
    rep.address--; rep.reserved=1; CHECK(syscall(ctl,child,VMCTX_CTL_PROTECT_MM,&rep)==-1 && errno==EINVAL);
    ok=1;
out:
    if (child>0) { kill(child,SIGKILL); while (waitpid(child,&status,0)<0 && errno==EINTR) {} }
    alarm(0);
    if (ok) puts("PASS: remote protection applies to target only, preserves lazy holes and rejects invalid ranges");
    return ok?0:1;
}
