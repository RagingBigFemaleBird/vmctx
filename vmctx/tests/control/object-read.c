/* SPDX-License-Identifier: GPL-2.0 */
/* Native object population for a read must preserve permissions and aliases,
 * and must leave an absent object slot absent. Run with native run/ctl IDs. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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
extern char object_read_probe[];
asm(".text\n.global object_read_probe\nobject_read_probe: mov (%rdi),%rax; movq $1,(%rdi); ud2\n");
static pid_t child;
static long ctl_nr;
static volatile sig_atomic_t expired;
static void deadline(int sig) { (void)sig;expired=1;if(child>0)kill(child,SIGKILL); }
static long ctl(unsigned command,void *arg)
{
    /* ATTACH may complete before the executor publishes its initial CPU
     * checkpoint. EAGAIN requires a retry, just as in the production adapter. */
    for (unsigned i = 0; i < 10000 && !expired; i++) {
        long result = syscall(ctl_nr,child,command,arg);
        if (result >= 0 || errno != EAGAIN) return result;
        usleep(100);
    }
    errno = ETIMEDOUT;
    return -1;
}
static int present(uint64_t addr)
{
    char name[80];uint64_t pte=0;
    snprintf(name,sizeof(name),"/proc/%d/pagemap",child);
    int fd=open(name,O_RDONLY|O_CLOEXEC);if(fd<0)return -1;
    ssize_t n=pread(fd,&pte,sizeof(pte),(off_t)(addr/4096*8));close(fd);
    return n==sizeof(pte) ? !!(pte&(UINT64_C(1)<<63)):-1;
}
static int protection(uint64_t addr)
{
    char name[80],line[512],perms[5];unsigned long lo,hi;
    snprintf(name,sizeof(name),"/proc/%d/maps",child);
    FILE *f=fopen(name,"r");if(!f)return -1;
    int result=-1;
    while(fgets(line,sizeof(line),f))
        if(sscanf(line,"%lx-%lx %4s",&lo,&hi,perms)==3 && addr>=lo && addr<hi) {
            result=(perms[0]=='r'?PROT_READ:0)|(perms[1]=='w'?PROT_WRITE:0)|(perms[2]=='x'?PROT_EXEC:0);break;
        }
    fclose(f);return result;
}
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno);goto done;}}while(0)
int main(int argc,char **argv)
{
    if(argc!=4)return 2;
    long run_nr=strtol(argv[1],NULL,10);ctl_nr=strtol(argv[2],NULL,10);
    int hole=!strcmp(argv[3],"hole"),write=!strcmp(argv[3],"rw"),execute=!strcmp(argv[3],"exec");
    int protect=!strcmp(argv[3],"protect");
    int pass=0,status,fd=-1,attached=0;uint64_t value=UINT64_C(0x1234567887654321),observed=0;
    void *address=MAP_FAILED,*stack=MAP_FAILED;pid_t parent=getpid();
    struct sigaction sa={.sa_handler=deadline};
    struct vmctx_cpu_state cpu;struct vmctx_cpu_model model;
    CHECK(hole || write || execute || protect || !strcmp(argv[3],"ro"));
    CHECK(!sigaction(SIGALRM,&sa,NULL));
    fd=memfd_create("object-read",MFD_CLOEXEC);CHECK(fd>=0 && !ftruncate(fd,8192));
    unsigned char code[]={0x48,0xb8,0,0,0,0,0,0,0,0,0x0f,0x0b};
    memcpy(code+2,&value,8);
    if(!hole)CHECK(pwrite(fd,execute?(void *)code:(void *)&value,execute?sizeof(code):sizeof(value),4096)==(ssize_t)(execute?sizeof(code):sizeof(value)));
    int prot=execute?PROT_EXEC:PROT_READ|(write?PROT_WRITE:0);
    address=mmap(NULL,4096,prot,MAP_SHARED,fd,4096);
    stack=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(address!=MAP_FAILED && stack!=MAP_FAILED);
    CHECK(!syscall(ctl_nr,0,VMCTX_CTL_CPU_CAPS,&model));
    struct vmctx_run_config cfg={.flags=VMCTX_FLAG_USERCODE|VMCTX_FLAG_WAIT_MONITOR|
        VMCTX_FLAG_RESTORE|VMCTX_FLAG_REDIRECT_FAULT|VMCTX_FLAG_REDIRECT_SYSCALL,
        .backing_fd=fd,.shared_fd=-1};
    alarm(10);child=fork();CHECK(child>=0);
    if(!child) {
        if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent)_exit(125);
        syscall(run_nr,&cfg);_exit(126);
    }
    for(unsigned i=0;i<2000 && !expired;i++) {
        if(!ctl(VMCTX_CTL_ATTACH,NULL)) {attached=1;break;}
        /* The task exists before vmctx_run_current publishes its context. */
        CHECK(errno==EINVAL || errno==EAGAIN || errno==ESRCH);usleep(1000);
    }
    CHECK(attached && !ctl(VMCTX_CTL_GETCPU,&cpu));
    CHECK(present((uintptr_t)address)==0 && protection((uintptr_t)address)==prot);
    struct vmctx_mem m={.addr=(uintptr_t)address,.len=4096};
    if(!write)CHECK(ctl(VMCTX_CTL_MAPOBJ,&m)==-1 && errno==EFAULT);
    long mapped=ctl(VMCTX_CTL_MAPOBJ_READ,&m);int saved=errno;
    printf("OBSERVED: %s read-map=%ld errno=%d present=%d prot=%d\n",argv[3],mapped,mapped<0?saved:0,present((uintptr_t)address),protection((uintptr_t)address));
    CHECK(mapped==(hole?0:4096));
    CHECK(protection((uintptr_t)address)==prot && present((uintptr_t)address)==!hole);
    if(protect) {
        struct vmctx_protection p={.address=(uintptr_t)address,.length=4096,
            .protection=PROT_READ|PROT_WRITE};
        CHECK(!ctl(VMCTX_CTL_PROTECT_MM,&p));
        CHECK(protection((uintptr_t)address)==(PROT_READ|PROT_WRITE));
    }
    if(hole) {CHECK(lseek(fd,4096,SEEK_DATA)==-1 && errno==ENXIO);pass=1;goto done;}
    /* The PTE must map the existing folio, not a private snapshot. */
    value^=UINT64_C(0x2121212121212121);
    CHECK(pwrite(fd,&value,8,execute?4098:4096)==8);
    cpu.regs.rip=(uintptr_t)(execute?address:(void *)object_read_probe);
    cpu.regs.rdi=(uintptr_t)address;cpu.regs.rsp=(uintptr_t)stack+65536-256;
    cpu.regs.rflags=0x202;cpu.regs.orig_rax=~UINT64_C(0);
    CHECK(!ctl(VMCTX_CTL_CPU_MODEL,&model) && !ctl(VMCTX_CTL_SETCPU,&cpu));
    struct vmctx_reply reply={.action=VMCTX_ACT_SELF};CHECK(!ctl(VMCTX_CTL_RESUME,&reply));
    for(unsigned i=0;i<2000 && !expired;i++) {
        struct vmctx_event ev;
        if(ctl(VMCTX_CTL_WAIT,&ev)) {CHECK(errno==EAGAIN);usleep(1000);continue;}
        CHECK(ev.type==VMCTX_EV_FAULT);
        if((execute && ev.nr==6) || (!execute && ev.fault_addr==(uintptr_t)address)) {
            CHECK(!ctl(VMCTX_CTL_GETCPU,&cpu) && cpu.regs.rax==value);
            if(!execute)CHECK(ev.nr==14 && (ev.fault_err&3)==3);
            CHECK(pread(fd,&observed,8,execute?4098:4096)==8 && observed==value);
            pass=1;break;
        }
        CHECK(ev.nr==14 && !ctl(VMCTX_CTL_RESUME,&reply));
    }
done:
    if(child>0) {
        kill(child,SIGKILL);pid_t reaped;
        do {reaped=waitpid(child,&status,0);}while(reaped<0 && errno==EINTR);
        if(reaped!=child || !WIFSIGNALED(status) || WTERMSIG(status)!=SIGKILL)pass=0;
    }
    alarm(0);if(expired)pass=0;
    if(address!=MAP_FAILED)munmap(address,4096);
    if(stack!=MAP_FAILED)munmap(stack,65536);
    if(fd>=0)close(fd);
    printf("%s: read mapping %s preserves VMA permissions, actual read/write restrictions, existing folio and holes\n",pass?"PASS":"FAIL",argv[3]);
    return pass?0:1;
}
