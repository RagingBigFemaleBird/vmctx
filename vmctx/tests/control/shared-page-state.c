// SPDX-License-Identifier: GPL-2.0
/* Native execution-adapter control: shared reply installs existing bytes,
 * preserves permissions, and never fills a missing object slot with zeros. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>
#include "vmctx_executor_map.h"
extern char shared_probe[], shared_probe_done[];
asm(".text\n.global shared_probe,shared_probe_done\n"
    "shared_probe: mov (%rdi),%rax\n"
    "test %rsi,%rsi; jz shared_probe_done\n"
    "add $1,%rax; mov %rax,(%rdi)\n"
    "shared_probe_done: ud2\n");
static volatile sig_atomic_t child, expired;
static long ctl_nr;
static void deadline(int sig) { (void)sig; expired=1; if(child>0) kill(child,SIGKILL); }
static int ctl(unsigned op,void *arg)
{
    for(unsigned i=0;i<10000 && !expired;i++) {
        int r=syscall(ctl_nr,child,op,arg);
        if(!r || errno!=EAGAIN) return r;
        usleep(100);
    }
    errno=ETIMEDOUT; return -1;
}
static int run(long run_nr,int hole,int write)
{
    const uint64_t magic=0x1234567887654321ULL;
    int fd=memfd_create("shared-page-control",MFD_CLOEXEC), status, pass=0, reports=0;
    void *address=MAP_FAILED,*stack=MAP_FAILED;
    struct vmctx_cpu_model model;
    struct vmctx_cpu_state state;
    if(fd<0 || ftruncate(fd,8192)) goto done;
    if(!hole && pwrite(fd,&magic,sizeof(magic),4096)!=sizeof(magic)) goto done;
    address=mmap(NULL,4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    stack=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(address==MAP_FAILED || stack==MAP_FAILED || syscall(ctl_nr,0,VMCTX_CTL_CPU_CAPS,&model)) goto done;
    struct vmctx_run_config cfg={.flags=VMCTX_FLAG_USERCODE|VMCTX_FLAG_WAIT_MONITOR|
        VMCTX_FLAG_RESTORE|VMCTX_FLAG_REDIRECT_FAULT|VMCTX_FLAG_REDIRECT_SYSCALL,
        .backing_fd=-1,.shared_fd=fd,.max_exits=256};
    pid_t parent=getpid(); child=fork();
    if(child<0) goto done;
    if(!child) {
        if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent) _exit(125);
        syscall(run_nr,&cfg); _exit(126);
    }
    expired=0; alarm(15);
    int attached=0;
    for(unsigned i=0;i<2000 && !expired;i++) {
        if(!ctl(VMCTX_CTL_ATTACH,NULL)) {attached=1;break;}
        usleep(1000);
    }
    if(!attached || ctl(VMCTX_CTL_GETCPU,&state)) goto done;
    state.regs.rip=(uintptr_t)shared_probe; state.regs.rdi=(uintptr_t)address;
    state.regs.rsi=write; state.regs.rsp=(uintptr_t)stack+65536-256;
    state.regs.rflags=0x202; state.regs.orig_rax=~0ULL;
    if(ctl(VMCTX_CTL_CPU_MODEL,&model) || ctl(VMCTX_CTL_SETCPU,&state)) goto done;
    struct vmctx_reply reply={.action=VMCTX_ACT_SELF};
    if(ctl(VMCTX_CTL_RESUME,&reply)) goto done;
    for(unsigned i=0;i<256 && !expired;i++) {
        struct vmctx_event ev;
        if(ctl(VMCTX_CTL_WAIT,&ev) || ev.type!=VMCTX_EV_FAULT) goto done;
        reply=(struct vmctx_reply){.action=VMCTX_ACT_SELF};
        if(ev.nr==14 && ev.fault_addr==(uintptr_t)address) {
            reports++;
            if(reports>(hole?2:1)) goto done;
            if(hole && reports==2) {
                errno=0;
                if(lseek(fd,4096,SEEK_DATA)!=-1 || errno!=ENXIO) goto done;
                if(pwrite(fd,&magic,sizeof(magic),4096)!=sizeof(magic)) goto done;
            }
            reply.action=VMCTX_ACT_DONE; reply.map_op=VMCTX_MAP_SET_SHARED_PAGE;
            reply.map_addr=(uintptr_t)address; reply.map_len=4096; reply.map_off=4096;
            reply.map_prot=PROT_READ|(write?PROT_WRITE:0);
        } else if(ev.nr==6 && ev.rip==(uintptr_t)shared_probe_done) {
            uint64_t actual;
            if(ctl(VMCTX_CTL_GETCPU,&state) || state.regs.rax!=magic+write ||
               pread(fd,&actual,sizeof(actual),4096)!=sizeof(actual) || actual!=magic+write) goto done;
            struct vmctx_mem probe={.addr=(uintptr_t)address,.len=2};
            int denied=syscall(ctl_nr,child,VMCTX_CTL_TRYFAULT,&probe);
            if((write && denied) || (!write && (denied!=-1 || errno!=EACCES))) goto done;
            pass=reports==(hole?2:1); break;
        } else if(ev.nr!=14) goto done;
        if(ctl(VMCTX_CTL_RESUME,&reply)) goto done;
    }
done:
    if(!pass) fprintf(stderr,"FAIL: shared page hole=%d write=%d reports=%d errno=%d timeout=%d\n",hole,write,reports,errno,expired);
    if(child>0) { kill(child,SIGKILL); while(waitpid(child,&status,0)<0 && errno==EINTR) {} }
    child=0; alarm(0);
    if(address!=MAP_FAILED) munmap(address,4096);
    if(stack!=MAP_FAILED) munmap(stack,65536);
    if(fd>=0) close(fd);
    if(pass) printf("PASS: shared page hole=%d write=%d reports=%d\n",hole,write,reports);
    return pass?0:1;
}
int main(int argc,char **argv)
{
    struct sigaction sa={.sa_handler=deadline};
    struct vmctx_executor_map_caps caps={0};
    if(argc!=3 || sigaction(SIGALRM,&sa,NULL)) return 2;
    ctl_nr=strtol(argv[2],NULL,10);
    if(syscall(ctl_nr,0,VMCTX_CTL_EXECUTOR_MAP_CAPS,&caps) || caps.version!=1 ||
       caps.size!=sizeof(caps) || !(caps.features&VMCTX_EXECUTOR_MAP_SHARED_PAGE)) return 1;
    return run(strtol(argv[1],NULL,10),0,0) || run(strtol(argv[1],NULL,10),0,1) ||
           run(strtol(argv[1],NULL,10),1,0);
}
