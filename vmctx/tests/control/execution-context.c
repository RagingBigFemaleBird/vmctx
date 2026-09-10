/* SPDX-License-Identifier: GPL-2.0 */
/* Native execution descriptors remain bound to one context through teardown
 * and native PID reuse. The namespace mode changes only its own PID counter. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>
#include "../../kernel/vmctx_context.h"
#include "../../kernel/vmctx_object.h"

static long run_nr, ctl_nr;
static volatile sig_atomic_t child;
static void deadline(int signal_number)
{ (void)signal_number; if(child>0) kill(child,SIGKILL); _exit(124); }
static long ctl(pid_t selector,unsigned cmd,void *arg)
{ return syscall(ctl_nr,selector,cmd,arg); }
static pid_t selector(int fd) { return -(fd+1); }
static struct vmctx_context request(unsigned op)
{ return (struct vmctx_context){.version=VMCTX_CONTEXT_ABI,.size=sizeof(struct vmctx_context),.op=op}; }
static int context(pid_t target,unsigned op,struct vmctx_context *info)
{ *info=request(op);return ctl(target,VMCTX_CTL_CONTEXT,info); }
static int signal_context(int fd,int sig)
{ struct vmctx_context info=request(VMCTX_CONTEXT_SIGNAL);info.exit_status=sig;return ctl(selector(fd),VMCTX_CTL_CONTEXT,&info); }
static int info_wait(int fd,struct vmctx_context *info)
{
    for(unsigned i=0;i<2000;i++) {
        int r=context(selector(fd),VMCTX_CONTEXT_INFO,info);
        if(!r || (errno!=EBUSY && errno!=EAGAIN)) return r;
        usleep(1000);
    }
    errno=ETIMEDOUT;return -1;
}
static long handles(void)
{
    long count=-1;
    FILE *f=fopen("/sys/module/kernel/parameters/vmctx_context_handles","r");
    if(f) { if(fscanf(f,"%ld",&count)!=1) count=-1;fclose(f); }
    return count;
}
static int count_wait(long expected)
{
    for(unsigned i=0;i<2000;i++) { if(handles()==expected)return 1;usleep(1000); }
    return 0;
}
static int reap(pid_t pid)
{
    int status;pid_t p;
    do { p=waitpid(pid,&status,0); } while(p<0 && errno==EINTR);
    return p==pid && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL;
}
static int start(void)
{
    pid_t parent=getpid(),pid=fork();
    if(pid<0) return -1;
    if(!pid) {
        if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent)_exit(125);
        struct vmctx_run_config cfg={.flags=VMCTX_FLAG_USERCODE|VMCTX_FLAG_WAIT_MONITOR|
            VMCTX_FLAG_RESTORE|VMCTX_FLAG_REDIRECT_SYSCALL|VMCTX_FLAG_REDIRECT_FAULT,
            .backing_fd=-1,.shared_fd=-1};
        syscall(run_nr,&cfg);_exit(126);
    }
    child=pid;
    for(unsigned i=0;i<2000;i++) {
        if(!ctl(pid,VMCTX_CTL_ATTACH,NULL)) return pid;
        if(errno!=EINVAL && errno!=EAGAIN && errno!=ESRCH) return -1;
        usleep(1000);
    }
    errno=ETIMEDOUT;return -1;
}
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL execution-context line %d: %s errno=%d\n",__LINE__,#x,errno);goto done; } } while(0)
static int run(int reuse)
{
    int pass=0,old=-1,duplicate=-1,fresh=-1;
    pid_t original=0;
    void *bad=MAP_FAILED;
    struct vmctx_context info;
    uint64_t identity=0,mm=0;
    long before=handles();
    CHECK(before>=0);
    CHECK(!context(0,VMCTX_CONTEXT_CAPS,&info));
    CHECK((info.flags&(VMCTX_CONTEXT_CAP_REFERENCE|VMCTX_CONTEXT_CAP_SIGNAL|
        VMCTX_CONTEXT_CAP_MMINFO|VMCTX_CONTEXT_CAP_EXECUTOR))==
        (VMCTX_CONTEXT_CAP_REFERENCE|VMCTX_CONTEXT_CAP_SIGNAL|
        VMCTX_CONTEXT_CAP_MMINFO|VMCTX_CONTEXT_CAP_EXECUTOR));
    CHECK(start()>0);original=child;
    CHECK(!context(child,VMCTX_CONTEXT_OPEN,&info));old=info.fd;identity=info.identity;
    CHECK(identity && info.native_pid==(unsigned)child && info.native_tgid==(unsigned)child);
    CHECK(fcntl(old,F_GETFD)&FD_CLOEXEC);
    CHECK(!info_wait(old,&info));mm=info.mm_identity;
    CHECK(mm && !info.flags && !info.exit_status && info.identity==identity);
    CHECK(!context(child,VMCTX_CONTEXT_OPEN,&info));duplicate=info.fd;
    CHECK(info.identity==identity && duplicate!=old && count_wait(before+2));
    bad=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(bad!=MAP_FAILED);*(struct vmctx_context *)bad=request(VMCTX_CONTEXT_OPEN);
    CHECK(!mprotect(bad,4096,PROT_READ));
    CHECK(ctl(child,VMCTX_CTL_CONTEXT,bad)==-1 && errno==EFAULT);
    CHECK(count_wait(before+2));
    /* Inheriting the FD does not inherit monitor ownership. */
    pid_t foreign=fork();CHECK(foreign>=0);
    if(!foreign) { struct vmctx_context q;_exit(context(selector(old),VMCTX_CONTEXT_INFO,&q)==-1 && errno==EPERM ? 0 : 1); }
    int status;CHECK(waitpid(foreign,&status,0)==foreign && WIFEXITED(status) && !WEXITSTATUS(status));
    struct vmctx_cpu_state cpu;
    for(unsigned i=0;;i++) {
        if(!ctl(selector(old),VMCTX_CTL_GETCPU,&cpu))break;
        CHECK(i<2000 && (errno==EAGAIN || errno==EBUSY));usleep(1000);
    }
    CHECK(!signal_context(old,0));
    info=request(VMCTX_CONTEXT_CHILD);info.ticket=1;
    CHECK(ctl(selector(old),VMCTX_CTL_CONTEXT,&info)==-1 && errno==EOPNOTSUPP);
    CHECK(!signal_context(old,SIGKILL));CHECK(reap(child));child=0;
    CHECK(!info_wait(old,&info));
    CHECK(info.identity==identity && info.mm_identity==mm && info.flags==VMCTX_CONTEXT_ENDED && !info.exit_status);
    CHECK(!info_wait(duplicate,&info) && info.identity==identity);
    CHECK(ctl(selector(old),VMCTX_CTL_GETCPU,&cpu)==-1 && errno==ESRCH);
    CHECK(signal_context(old,0)==-1 && errno==ESRCH);
    info=request(VMCTX_CONTEXT_MMINFO);info.mm_identity=mm;
    CHECK(!ctl(0,VMCTX_CTL_CONTEXT,&info) && info.flags==VMCTX_CONTEXT_MM_ENDED);
    if(reuse) {
        FILE *counter=fopen("/proc/sys/kernel/ns_last_pid","w");CHECK(counter);
        int written=fprintf(counter,"%d\n",original-1),closed=fclose(counter);
        CHECK(written>0 && !closed);
        CHECK(start()==original);
        CHECK(!context(child,VMCTX_CONTEXT_OPEN,&info));fresh=info.fd;
        CHECK(info.identity && info.identity!=identity);
        CHECK(!info_wait(old,&info) && info.identity==identity && info.mm_identity==mm && info.flags==VMCTX_CONTEXT_ENDED);
        CHECK(signal_context(old,SIGKILL)==-1 && errno==ESRCH);
        CHECK(!signal_context(fresh,0));
        CHECK(!info_wait(fresh,&info) && info.mm_identity && info.mm_identity!=mm && !(info.flags&VMCTX_CONTEXT_ENDED));
        CHECK(!signal_context(fresh,SIGKILL));CHECK(reap(child));child=0;
    }
    pass=1;
done:
    if(child>0) { kill(child,SIGKILL);(void)reap(child);child=0; }
    if(old>=0)close(old);
    if(duplicate>=0)close(duplicate);
    if(fresh>=0)close(fresh);
    if(bad!=MAP_FAILED)munmap(bad,4096);
    if(!count_wait(before))pass=0;
    if(pass)puts(reuse ? "PASS: retained execution identity survives native PID reuse without targeting its replacement" :
        "PASS: execution descriptors preserve identity, ownership, terminal MM and reference cleanup");
    return pass ? 0 : 1;
}
int main(int argc,char **argv)
{
    if(argc!=4)return 2;
    run_nr=strtol(argv[1],NULL,10);ctl_nr=strtol(argv[2],NULL,10);
    signal(SIGALRM,deadline);alarm(20);
    if(!strcmp(argv[3],"lifetime"))return run(0);
    if(strcmp(argv[3],"pid-reuse"))return 2;
    if(unshare(CLONE_NEWNS|CLONE_NEWPID) || mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL))return 2;
    pid_t init=fork();if(init<0)return 2;
    if(!init) {
        signal(SIGALRM,deadline);alarm(15);
        if(getpid()!=1 || mount("proc","/proc","proc",0,NULL))_exit(123);
        exit(run(1));
    }
    int status;
    if(waitpid(init,&status,0)!=init || !WIFEXITED(status))return 1;
    return WEXITSTATUS(status);
}
