// SPDX-License-Identifier: GPL-2.0
/* The production executor control route, including its snapshot wrappers and
 * stable monitor-owner thread. Each mode runs in a fresh owned process. */
#define _GNU_SOURCE
#include <sys/types.h>
static void routing_proc_observe(unsigned phase,pid_t pid);
#define LINUX_EXECUTION_PROC_OBSERVE(phase,pid) routing_proc_observe(phase,pid)
#define main vmremote_program_main
#include "../../user/vmremote.c"
#undef main
#include <sys/mount.h>
#include <sys/prctl.h>

static pid_t running_child;
static execution_id running_context;
static struct execution_mm *running_mm;
static struct vmr_mm_binding running_source;
static uint64_t next_source;
static int routing_memory;
#define ROUTING_BASE UINT64_C(0x3000000000)

static void routing_deadline(int sig)
{ (void)sig;if(running_child>0)kill(running_child,SIGKILL);_exit(124); }

static pid_t routing_child(void)
{
    running_source=(struct vmr_mm_binding){.context=++next_source,
        .mm=UINT64_C(0x100000000)|next_source,.epoch=1};
    running_mm=source_mm_resolve(&running_source);
    if(!running_mm)return -1;
    void *mapping=MAP_FAILED;
    if(routing_memory) {
        mapping=mmap((void *)ROUTING_BASE,4096,PROT_READ|PROT_WRITE,
            MAP_SHARED|MAP_FIXED_NOREPLACE,running_mm->backing_fd,ROUTING_BASE);
        if(mapping==MAP_FAILED)return -1;
        memset(mapping,0x5a,4096);
    }
    pid_t owner=getpid(),pid=fork();
    if(pid<0)return -1;
    if(!pid) {
        if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=owner)_exit(125);
        struct vmctx_run_config config={.flags=VMCTX_FLAG_USERCODE|
            VMCTX_FLAG_WAIT_MONITOR|VMCTX_FLAG_RESTORE|VMCTX_FLAG_REDIRECT_SYSCALL|
            VMCTX_FLAG_REDIRECT_FAULT,.backing_fd=running_mm->backing_fd,.shared_fd=-1};
        syscall(__NR_vmctx_run,&config);_exit(126);
    }
    if(mapping!=MAP_FAILED && munmap(mapping,4096)) {kill(pid,SIGKILL);waitpid(pid,NULL,0);return -1;}
    running_child=pid;running_context=0;
    for(unsigned i=0;i<2000;i++) {
        if(!monitor_attach(pid))return pid;
        if(errno!=EINVAL && errno!=EAGAIN && errno!=ESRCH)return -1;
        usleep(1000);
    }
    errno=ETIMEDOUT;return -1;
}

static int routing_reap(pid_t pid)
{
    int status;pid_t got;
    if(running_context)got=ctx_wait(running_context,&status)==running_context ? pid : -1;
    else do {got=waitpid(pid,&status,0);}while(got<0 && errno==EINTR);
    if(got==pid) {running_child=0;running_context=0;}
    return got==pid && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL;
}

#define ROUTE_CHECK(x) do {if(!(x)) {fprintf(stderr,"FAIL execution-routing line %d: %s errno=%d\n",__LINE__,#x,errno);goto done;}}while(0)
static int proc_race_armed,proc_race_fired,proc_race_error;
static int routing_replace(pid_t pid)
{
    if(ctx_signal(running_context,SIGKILL) || !routing_reap(pid))return -1;
    FILE *counter=fopen("/proc/sys/kernel/ns_last_pid","w");
    if(!counter)return -1;
    int wrote=fprintf(counter,"%d\n",pid-1),closed=fclose(counter);
    if(wrote<=0 || closed)return -1;
    return routing_child()==pid ? 0 : -1;
}
static void routing_proc_observe(unsigned phase,pid_t pid)
{
    if(phase!=1 || !proc_race_armed)return;
    proc_race_armed=0;proc_race_fired=1;
    proc_race_error=routing_replace(pid);
}
static int routing_test(int mode)
{
    int pass=0;
    FILE *maps=NULL;
    struct linux_execution_context old={.fd=-1,.exit_fd=-1};
    struct linux_execution_context replacement={.fd=-1,.exit_fd=-1};
    coh_lock_init();
    ROUTE_CHECK(!linux_execution_capabilities());
    routing_memory=mode==4;
    pid_t pid=routing_child();ROUTE_CHECK(pid>0);
    execution_id id=INT_MAX;int created;
    struct execution_context_binding unknown;
    ROUTE_CHECK(!execution_context_binding_init(&unknown,id,running_mm,&running_source,1));
    memory_target target=execution_target_capture(&unknown);
    /* A live numeric child is insufficient for ordinary controls. */
    struct vmctx_uregs registers;
    ROUTE_CHECK(ctl(target,VMCTX_CTL_GETREGS,&registers)==-1 && errno==ESRCH);
    ROUTE_CHECK(!ctx_maps_open(target) && errno==ESRCH);
    struct vmr_mm_binding wrong=running_source;wrong.mm+=UINT64_C(1)<<32;
    struct execution_mm *other=source_mm_resolve(&wrong);ROUTE_CHECK(other);
    ROUTE_CHECK(ctx_register(pid,other,&wrong,&created)==-1 && errno==EPROTO && !created && !nctxs);
    id=ctx_register(pid,running_mm,&running_source,&created);
    ROUTE_CHECK(id==1 && created && nctxs==1);
    running_context=id;
    target=ctx_target(id);
    execution_context_binding_destroy(&unknown);
    ROUTE_CHECK(ctx_native_copy(id,&old) && old.fd>=0 && old.exit_fd>=0 && old.identity);
    ROUTE_CHECK(old.fd>2 && old.exit_fd>2);
    ROUTE_CHECK(fcntl(old.fd,F_GETFD)&FD_CLOEXEC);
    ROUTE_CHECK(fcntl(old.exit_fd,F_GETFD)&FD_CLOEXEC);
    ROUTE_CHECK(ctx_register(pid,running_mm,&running_source,&created)==id && !created && nctxs==1);
    wrong=running_source;wrong.epoch++;
    ROUTE_CHECK(ctx_register(pid,running_mm,&wrong,&created)==-1 && errno==EPROTO && !created && nctxs==1);
    const struct execution_target *initial=execution_target_capture(&ctx_memory[0]);
    ROUTE_CHECK(initial->view.mm==running_mm && initial->view.epoch==1 &&
        vmr_binding_equal(&initial->source,&running_source));
    for(unsigned i=0;;i++) {
        if(!ctl(target,VMCTX_CTL_GETREGS,&registers))break;
        ROUTE_CHECK(i<2000 && (errno==EAGAIN || errno==EBUSY));usleep(1000);
    }
    ROUTE_CHECK(task_alive(target) && !ctx_execution_ended(id,0));
    maps=ctx_maps_open(target);
    if(mode==3) {
        ROUTE_CHECK(!maps && errno==EXDEV);
        ROUTE_CHECK(page_present(target,(uintptr_t)&registers)==PAGE_UNKNOWN);
    }
    else {
        ROUTE_CHECK(maps && fileno(maps)>2);
        ROUTE_CHECK(fcntl(fileno(maps),F_GETFD)&FD_CLOEXEC);
        ROUTE_CHECK(fcntl(fileno(maps),F_GET_SEALS)==
            (F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL));
    }
    if(mode==4) {
        uint64_t values[2]={UINT64_C(0x123456789abcdef0),UINT64_C(0xfeedc0decafebeef)};
        struct iovec local[2]={{&values[0],8},{&values[1],8}};
        struct iovec remote[2]={{(void *)ROUTING_BASE,8},{(void *)(ROUTING_BASE+16),8}};
        ROUTE_CHECK(execution_writev(target,local,2,remote)==16);
        uint64_t got[3]={0};
        ROUTE_CHECK(pread(backing_find(target),got,sizeof(got),ROUTING_BASE)==sizeof(got));
        ROUTE_CHECK(got[0]==values[0] && got[1]==UINT64_C(0x5a5a5a5a5a5a5a5a) && got[2]==values[1]);
        struct vmr_mm_binding changed=target->source;
        changed.mm+=UINT64_C(1)<<32;changed.epoch++;
        struct execution_mm *fresh=source_mm_resolve(&changed);ROUTE_CHECK(fresh);
        struct linux_execution_object adapter={.control=initial_object_control,.opaque=&old};
        memory_target previous=target;
        target=execution_target_publish(target->context,fresh,&changed,linux_execution_object_replace,&adapter);
        ROUTE_CHECK(target && target->view.epoch==2 && ctx_target(id)==target);
        ROUTE_CHECK(execution_writev(previous,local,2,remote)==-1 && errno==ESTALE);
        uint64_t actual_epoch=0;
        ROUTE_CHECK(!linux_execution_object_verify(&adapter,backing_find(target),&actual_epoch) && actual_epoch==2);
        ROUTE_CHECK(pread(backing_find(target),got,sizeof(got),ROUTING_BASE)==sizeof(got));
        ROUTE_CHECK(!got[0] && !got[1] && !got[2]);
        ROUTE_CHECK(obj_write(previous,ROUTING_BASE,&values[1],8)==8);
        ROUTE_CHECK(pread(backing_find(previous),got,8,ROUTING_BASE)==8 && got[0]==values[1]);
        ROUTE_CHECK(pread(backing_find(target),got,8,ROUTING_BASE)==8 && !got[0]);
        memory_target members[MAX_CTX];
        ROUTE_CHECK(!as_members(previous,members,MAX_CTX));
        ROUTE_CHECK(as_members(target,members,MAX_CTX)==1 && members[0]==target);
        puts("PASS: native vector writes use the retained MM; exec rejects stale writes before native entry and keeps delayed object landings in old storage");
    }
    if(mode==2) {
        proc_race_armed=1;
        uint64_t word=UINT64_MAX;
        int result=ctx_pagemap_read(target,(uintptr_t)&registers,&word),saved=errno;
        ROUTE_CHECK(result==-1 && saved==ESRCH && word==UINT64_MAX && proc_race_fired && !proc_race_error);
    } else {
        ROUTE_CHECK(!ctx_signal(id,0) && !ctx_signal(id,SIGKILL));
        ROUTE_CHECK(routing_reap(pid));
    }
    ROUTE_CHECK(ctx_execution_ended(id,0) && !task_alive(target));
    ROUTE_CHECK(page_present(target,(uintptr_t)&registers)==PAGE_ABSENT);
    if(maps) {
        /* Consume only after the native MM is gone (and in proc-reuse, after
         * its numeric name belongs to a new MM). The bytes must be detached. */
        ROUTE_CHECK(fgetc(maps)!=EOF && !ferror(maps));
        ROUTE_CHECK(!fclose(maps));maps=NULL;
    }
    struct vmctx_context q={.version=VMCTX_CONTEXT_ABI,.size=sizeof(q),.op=VMCTX_CONTEXT_INFO};
    ROUTE_CHECK(!ctl(target,VMCTX_CTL_CONTEXT,&q) && q.identity==old.identity && q.flags==VMCTX_CONTEXT_ENDED);
    ROUTE_CHECK(ctl(target,VMCTX_CTL_GETREGS,&registers)==-1 && errno==ESRCH);
    if(mode==1 || mode==2) {
      if(mode==1) {
        FILE *counter=fopen("/proc/sys/kernel/ns_last_pid","w");ROUTE_CHECK(counter);
        int wrote=fprintf(counter,"%d\n",pid-1),closed=fclose(counter);
        ROUTE_CHECK(wrote>0 && !closed);
        ROUTE_CHECK(routing_child()==pid);
      }
        ROUTE_CHECK(!linux_execution_open_child(pid,&replacement));
        ROUTE_CHECK(replacement.identity!=old.identity);
        ROUTE_CHECK(nctxs==1);
        ROUTE_CHECK(!task_alive(target) && ctx_execution_ended(id,0));
        ROUTE_CHECK(ctl(target,VMCTX_CTL_GETREGS,&registers)==-1 && errno==ESRCH);
        ROUTE_CHECK(ctl(target,VMCTX_CTL_ATTACH,NULL)==-1 && errno==ESRCH);
        /* These controls bypassed ctl() through the COW snapshot wrappers. */
        unsigned char bytes[4096]={0};
        struct vmctx_mem memory={.addr=0x3400000000,.len=4096,.buf=(uintptr_t)bytes};
        ROUTE_CHECK(ctl(target,VMCTX_CTL_MAPOBJ,&memory)==-1 && errno==ESRCH);
        ROUTE_CHECK(ctl(target,VMCTX_CTL_TAKE,&memory)==-1 && errno==ESRCH);
        ROUTE_CHECK(ctl(target,VMCTX_CTL_TAKEOBJ,&memory)==-1 && errno==ESRCH);
        ROUTE_CHECK(ctx_signal(id,SIGKILL)==-1 && errno==ESRCH);
        uint64_t word=UINT64_MAX;
        ROUTE_CHECK(ctx_pagemap_read(target,(uintptr_t)&registers,&word)==-1 && errno==ESRCH && word==UINT64_MAX);
        ROUTE_CHECK(!linux_execution_pagemap_read(&replacement,(uintptr_t)&registers,&word));
        ctx_stop_all();
        ROUTE_CHECK(!linux_execution_signal(&replacement,0) && !linux_execution_ended(&replacement,0));
        int status;
        ROUTE_CHECK(ctx_wait(id,&status)==-1 && errno==ECHILD);
        /* A new native identity must not reuse even an ended source context's
         * immutable routing name. Its valid native object is insufficient. */
        wrong=running_source;wrong.context=initial->source.context;
        ROUTE_CHECK(ctx_register(pid,running_mm,&wrong,&created)==-1 &&
            errno==EEXIST && !created && nctxs==1);
        ROUTE_CHECK(ctx_observe_source(&initial->source)==initial);
        execution_id next=ctx_register(pid,running_mm,&running_source,&created);
        ROUTE_CHECK(next==id+1 && created && nctxs==2);
        running_context=next;
        memory_target next_target=ctx_target(next);
        ROUTE_CHECK(task_alive(next_target) && !task_alive(target));
        ROUTE_CHECK(!ctx_signal(next,0));
        ROUTE_CHECK(!ctx_pagemap_read(next_target,(uintptr_t)&registers,&word));
        for(unsigned i=0;;i++) {
            if(!ctl(next_target,VMCTX_CTL_GETREGS,&registers))break;
            ROUTE_CHECK(i<2000 && (errno==EAGAIN || errno==EBUSY));usleep(1000);
        }
        ROUTE_CHECK(ctx_wait(id,&status)==-1 && errno==ECHILD);
        ROUTE_CHECK(!ctx_signal(next,SIGKILL) && routing_reap(pid));
    }
    pass=1;
done:
    if(maps)fclose(maps);
    if(running_child>0) {kill(running_child,SIGKILL);(void)routing_reap(running_child);}
    linux_execution_close(&replacement);
    /* No other worker in this fixture uses a context after its last assertion.
     * The owner thread has no queued work and touches no published records. */
    for(int i=0;i<nctxs;i++) {
        execution_context_binding_destroy(&ctx_memory[i]);
        linux_execution_close(&ctx_native[i]);
    }
    execution_mm_registry_destroy(&executor_mms);
    if(pass)puts(mode==3 ? "PASS: production proc adapter refuses a mismatched PID-namespace mount" :
        mode==2 ? "PASS: production proc open rejects a task reaped and replaced during the native open" :
        mode==1 ? "PASS: distinct monitor tokens route controls, proc reads, COW wrappers, signals and waits across native PID reuse" :
        "PASS: production context tokens publish once and route controls, exit and wait through retained descriptors");
    return pass ? 0 : 1;
}

int main(int argc,char **argv)
{
    if(argc!=4 || strtol(argv[1],NULL,10)!=__NR_vmctx_run ||
       strtol(argv[2],NULL,10)!=__NR_vmctx_ctl)return 2;
    signal(SIGALRM,routing_deadline);alarm(20);
    if(!strcmp(argv[3],"stdio")) {
        for(unsigned mask=0;mask<8;mask++) {
            pid_t worker=fork();if(worker<0)return 2;
            if(!worker) {
                for(int fd=0;fd<3;fd++)if(mask&(1U<<fd))close(fd);
                _exit(routing_test(0));
            }
            int status;
            if(waitpid(worker,&status,0)!=worker || !WIFEXITED(status) || WEXITSTATUS(status)) {
                fprintf(stderr,"FAIL execution routing with closed standard descriptors mask=%u\n",mask);
                return 1;
            }
        }
        puts("PASS: native execution descriptors stay above standard descriptors for all eight closure combinations");
        return 0;
    }
    if(!strcmp(argv[3],"lifetime"))return routing_test(0);
    if(!strcmp(argv[3],"mm-target"))return routing_test(4);
    int mode=!strcmp(argv[3],"pid-reuse") ? 1 : !strcmp(argv[3],"proc-reuse") ? 2 : !strcmp(argv[3],"proc-namespace") ? 3 : 0;
    if(!mode)return 2;
    if(unshare(CLONE_NEWNS|CLONE_NEWPID) || mount(NULL,"/",NULL,MS_PRIVATE|MS_REC,NULL))return 2;
    pid_t init=fork();if(init<0)return 2;
    if(!init) {
        signal(SIGALRM,routing_deadline);alarm(15);
        if(getpid()!=1 || (mode!=3 && mount("proc","/proc","proc",0,NULL)))_exit(123);
        exit(routing_test(mode));
    }
    int status;
    if(waitpid(init,&status,0)!=init || !WIFEXITED(status))return 1;
    return WEXITSTATUS(status);
}
