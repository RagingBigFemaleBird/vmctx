/* SPDX-License-Identifier: GPL-2.0 */
/* Actual native custody: retained bytes and exact idempotent receipts, with
 * copyout failure, repeated content, invalidation and monitor teardown. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>
#include <linux/vmctx_recall.h>
#include <linux/vmctx_memory.h>
#include "../../kernel/vmctx_access.h"
#include "../../kernel/vmctx_transfer.h"
static volatile sig_atomic_t child,expired;
static long ctl_nr;
static long source_control_raw(pid_t selector,unsigned command,void *argument)
{return syscall(ctl_nr,selector,command,argument);}
#include "../../user/source-custody.h"
static uint64_t mm,addr;
static unsigned char expected[4096],received[4096];
static int worker_result;
static void deadline(int sig) {(void)sig;expired=1;if(child>0)kill(child,SIGKILL);}
static long control(unsigned op,void *p) {return syscall(ctl_nr,child,op,p);}
static long begin_at(struct vmctx_transfer *t,uint64_t address)
{
    *t=(struct vmctx_transfer){.version=VMCTX_TRANSFER_ABI,.size=sizeof(*t),
        .op=VMCTX_TRANSFER_BEGIN,.address=address,.mm_identity=mm};
    struct vmctx_memory memory={.version=VMCTX_MEMORY_ABI,.size=sizeof(memory),
        .op=VMCTX_MEMORY_CALL,.command=VMCTX_CTL_TRANSFER,.mm_identity=mm,.argument=(uintptr_t)t};
    return control(VMCTX_CTL_MEMORY,&memory);
}
static long transfer(struct vmctx_transfer *t,unsigned op,void *buffer)
{
    t->op=op;t->buf=(uintptr_t)buffer;
    return syscall(ctl_nr,0,VMCTX_CTL_TRANSFER,t);
}
static int state(unsigned want)
{
    struct vmctx_serve s={.addr=addr};
    return !control(VMCTX_CTL_PGSTATE,&s) && s.state==want;
}
static long counter(const char *name)
{
    char path[256];snprintf(path,sizeof(path),"/sys/module/kernel/parameters/%s",name);
    FILE *file=fopen(path,"r");long result=-1;if(!file)return result;
    if(fscanf(file,"%ld",&result)!=1)result=-1;
    fclose(file);return result;
}
static long assisted(long nr,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f)
{
    struct vmctx_syscall call={.nr=nr,.args={a,b,c,d,e,f}};
    return control(VMCTX_CTL_SYSCALL,&call) ? -10000 : call.ret;
}
static void *abandon(void *opaque)
{
    (void)opaque;struct vmctx_transfer t;
    worker_result=begin_at(&t,addr) || transfer(&t,VMCTX_TRANSFER_CAPTURE,NULL) ||
        transfer(&t,VMCTX_TRANSFER_READ,received);
    return NULL;
}
static void *steal(void *opaque)
{
    struct vmctx_transfer t=*(struct vmctx_transfer *)opaque;
    worker_result=transfer(&t,VMCTX_TRANSFER_CANCEL,NULL)==-1 && errno==ENOENT ? 0:1;
    return NULL;
}
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"FAIL line %d: %s errno=%d expired=%d\n",__LINE__,#x,errno,(int)expired);goto done;}}while(0)
int main(int argc,char **argv)
{
    if(argc!=4)return 2;
    ctl_nr=strtol(argv[2],NULL,10);const char *mode=argv[3];
    int shared=!strcmp(mode,"stale-ack"),pass=0,status=0,fd=-1;
    source_id adapter_source=0;
    size_t mapping_size=(!strcmp(mode,"scale") || !strcmp(mode,"connection")) ? 80*4096 : 8192;
    struct source_custody connection={0};
    pid_t parent=getpid(),reaped=-1;void *mapping=MAP_FAILED,*headers=MAP_FAILED;
    struct vmctx_transfer first={0},second={0};pthread_t worker;
    struct sigaction sa={.sa_handler=deadline};if(sigaction(SIGALRM,&sa,NULL))return 2;
    struct vmctx_transfer caps={.version=VMCTX_TRANSFER_ABI,.size=sizeof(caps)};
    CHECK(!syscall(ctl_nr,0,VMCTX_CTL_TRANSFER,&caps));
    CHECK(caps.features==VMCTX_TRANSFER_FEATURES);
    long live_before=counter("vmctx_transfer_live"),captures_before=counter("vmctx_transfer_captures");
    CHECK(live_before==0 && captures_before>=0);
    memset(expected,0x63,sizeof(expected));
    if(shared) {fd=memfd_create("custody-control",MFD_CLOEXEC);CHECK(fd>=0 && !ftruncate(fd,8192));}
    mapping=mmap(NULL,mapping_size,PROT_READ|PROT_WRITE,shared?MAP_SHARED:MAP_PRIVATE|MAP_ANONYMOUS,fd,0);
    CHECK(mapping!=MAP_FAILED);memcpy(mapping,expected,4096);addr=(uintptr_t)mapping;
    if(!strcmp(mode,"scale") || !strcmp(mode,"connection"))memset(mapping,0x63,mapping_size);
    headers=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(headers!=MAP_FAILED);
    child=fork();CHECK(child>=0);
    if(!child) {
        if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent || ptrace(PTRACE_TRACEME,0,NULL,NULL))_exit(125);
        raise(SIGSTOP);_exit(126);
    }
    alarm(20);CHECK(waitpid(child,&status,0)==child && WIFSTOPPED(status));
    CHECK(!control(VMCTX_CTL_ADOPT,NULL));CHECK(!ptrace(PTRACE_DETACH,child,NULL,NULL));
    struct vmctx_access_log info={.version=VMCTX_ACCESS_ABI,.size=sizeof(info)};
    CHECK(!control(VMCTX_CTL_ACCESS_LOG,&info));mm=info.mm_id;CHECK(mm);
    if(!strcmp(mode,"abandon") || !strcmp(mode,"connection-abandon"))
        CHECK(assisted(SYS_set_tid_address,addr,0,0,0,0,0)==child);
    if(!strcmp(mode,"connection") || !strcmp(mode,"connection-abandon")) {
        struct source_context handle={.fd=-1};struct source_memory_target target;
        CHECK(!source_context_open(child,&handle));
        adapter_source=source_record_add(&handle);CHECK(adapter_source>0 && handle.fd==-1);
        CHECK(!source_memory_target_capture(adapter_source,&target) && target.mm==mm);
        struct vmr_mm_binding binding={.context=adapter_source,.mm=mm,.epoch=target.epoch};
        struct source_custody_entry *entry;uint64_t ids[80];
        unsigned count=!strcmp(mode,"connection") ? 80:1;
        for(unsigned i=0;i<count;i++) {
            CHECK(!source_custody_begin(&connection,&binding,addr+i*4096,&entry));
            ids[i]=entry->transfer.ticket;
            CHECK(!source_transfer_capture(&entry->transfer));
            CHECK(source_custody_ack(&connection,&binding,addr+i*4096,ids[i])==-1 && errno==EINVAL);
            CHECK(!source_transfer_read(&entry->transfer,received));
            CHECK(!memcmp(expected,received,sizeof(expected)));
        }
        CHECK(connection.count==count && counter("vmctx_transfer_live")==count);
        CHECK(counter("vmctx_transfer_captures")==captures_before+count);
        if(!strcmp(mode,"connection-abandon")) {
            long abandoned=counter("vmctx_transfer_abandoned");
            CHECK(!source_custody_abandon(&connection) && connection.closed && !connection.count);
            CHECK(counter("vmctx_transfer_live")==0 && counter("vmctx_transfer_abandoned")==abandoned+1);
            CHECK(!source_custody_abandon(&connection) && counter("vmctx_transfer_abandoned")==abandoned+1);
            do {reaped=waitpid(child,&status,0);}while(reaped<0 && errno==EINTR);
            CHECK(reaped==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
            CHECK(source_custody_begin(&connection,&binding,addr,&entry)==-1 && errno==ESHUTDOWN);
            pass=1;goto done;
        }
        struct vmr_mm_binding forged=binding;forged.mm+=UINT64_C(1)<<32;
        CHECK(source_custody_ack(&connection,&forged,addr,ids[0])==-1 && errno==ESTALE);
        CHECK(connection.count==80 && state(VMCTX_PG_TRANSIT));
        for(unsigned i=count;i--;)CHECK(!source_custody_ack(&connection,&binding,addr+i*4096,ids[i]));
        CHECK(!connection.count && !connection.pending && state(VMCTX_PG_REMOTE));
        CHECK(assisted(SYS_mmap,addr,4096,PROT_READ|PROT_WRITE,
            MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,UINT64_MAX,0)==(long)addr);
        CHECK(!source_custody_begin(&connection,&binding,addr,&entry));
        uint64_t fresh=entry->transfer.ticket;CHECK(fresh!=ids[0]);
        CHECK(!source_transfer_capture(&entry->transfer));
        CHECK(entry->transfer.result.status==VMCTX_SERVE_ABSENT && (entry->transfer.result.flags&VMCTX_SERVE_GRANT));
        CHECK(!source_transfer_read(&entry->transfer,received));
        CHECK(source_custody_ack(&connection,&binding,addr,ids[0])==-1 && errno==ENOENT);
        CHECK(state(VMCTX_PG_TRANSIT) && connection.count==1);
        CHECK(!source_custody_ack(&connection,&binding,addr,fresh));
        CHECK(!source_custody_begin(&connection,&binding,addr+4096,&entry));fresh=entry->transfer.ticket;
        CHECK(!source_transfer_capture(&entry->transfer) && !source_transfer_read(&entry->transfer,received));
        CHECK(!assisted(SYS_munmap,addr+4096,4096,0,0,0,0));
        CHECK(source_custody_ack(&connection,&binding,addr+4096,fresh)==-1 && errno==ESTALE);
        CHECK(!connection.count && !connection.pending && counter("vmctx_transfer_live")==0);
        pass=1;goto done;
    }
    if(!strcmp(mode,"adapter")) {
        struct source_context handle={.fd=-1};
        struct source_memory_target target;
        struct source_transfer ticket={0};
        CHECK(!source_context_open(child,&handle));
        adapter_source=source_record_add(&handle);CHECK(adapter_source>0 && handle.fd==-1);
        CHECK(!source_memory_target_capture(adapter_source,&target) && target.mm==mm);
        CHECK(!source_transfer_caps());
        CHECK(!source_transfer_begin(&target,addr,&ticket));
        CHECK(!source_transfer_capture(&ticket));
        CHECK(source_transfer_ack(&ticket)==-1 && errno==EINVAL && state(VMCTX_PG_TRANSIT));
        CHECK(!source_transfer_read(&ticket,received));
        CHECK(!memcmp(expected,received,sizeof(expected)));
        CHECK(!source_transfer_capture(&ticket));
        CHECK(counter("vmctx_transfer_captures")==captures_before+1);
        CHECK(!source_transfer_ack(&ticket) && state(VMCTX_PG_REMOTE));
        CHECK(!source_transfer_ack(&ticket));
        CHECK(!source_transfer_forget(&ticket) && !ticket.ticket);
        CHECK(!source_transfer_begin(&target,addr+4096,&ticket));
        CHECK(!source_transfer_capture(&ticket));
        CHECK(!source_transfer_read(&ticket,received));
        CHECK(!assisted(SYS_munmap,addr+4096,4096,0,0,0,0));
        CHECK(!source_transfer_cancel(&ticket) && ticket.cancelled);
        CHECK(source_transfer_ack(&ticket)==-1 && errno==ECANCELED);
        CHECK(!source_transfer_forget(&ticket));
        CHECK(counter("vmctx_transfer_live")==0);pass=1;goto done;
    }
    if(!strcmp(mode,"owner") || !strcmp(mode,"owner-clear-tid")) {
        if(!strcmp(mode,"owner-clear-tid"))
            CHECK(assisted(SYS_set_tid_address,addr,0,0,0,0,0)==child);
        CHECK(!pthread_create(&worker,NULL,abandon,NULL));CHECK(!pthread_join(worker,NULL) && !worker_result);
        do {reaped=waitpid(child,&status,0);}while(reaped<0 && errno==EINTR);
        CHECK(reaped==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
        CHECK(counter("vmctx_transfer_live")==0);pass=1;goto done;
    }
    if(!strcmp(mode,"scale")) {
        struct vmctx_transfer tickets[80];
        for(unsigned i=0;i<80;i++) {
            CHECK(!begin_at(&tickets[i],addr+i*4096));
            CHECK(!transfer(&tickets[i],VMCTX_TRANSFER_CAPTURE,NULL));
            CHECK(!transfer(&tickets[i],VMCTX_TRANSFER_READ,received));
            CHECK(!memcmp(expected,received,sizeof(expected)));
        }
        CHECK(counter("vmctx_transfer_live")==80);
        for(unsigned i=80;i-->0;) {
            CHECK(!transfer(&tickets[i],VMCTX_TRANSFER_READ,received));
            CHECK(!memcmp(expected,received,sizeof(expected)));
            CHECK(!transfer(&tickets[i],VMCTX_TRANSFER_ACK,NULL));
            CHECK(!transfer(&tickets[i],VMCTX_TRANSFER_ACK,NULL));
            CHECK(!transfer(&tickets[i],VMCTX_TRANSFER_FORGET,NULL));
        }
        CHECK(counter("vmctx_transfer_live")==0);
        CHECK(counter("vmctx_transfer_captures")==captures_before+80);
        pass=1;goto done;
    }
    if(!strcmp(mode,"absent"))addr+=4096;
    if(!strcmp(mode,"replay")) {
        /* BEGIN copyout failure allocated no page custody and leaks nothing. */
        struct vmctx_transfer *bad=headers;
        *bad=(struct vmctx_transfer){.version=VMCTX_TRANSFER_ABI,.size=sizeof(*bad),
            .op=VMCTX_TRANSFER_BEGIN,.address=addr,.mm_identity=mm};
        CHECK(!mprotect(headers,4096,PROT_READ));
        struct vmctx_memory memory={.version=VMCTX_MEMORY_ABI,.size=sizeof(memory),
            .op=VMCTX_MEMORY_CALL,.command=VMCTX_CTL_TRANSFER,.mm_identity=mm,.argument=(uintptr_t)bad};
        CHECK(control(VMCTX_CTL_MEMORY,&memory)==-1 && errno==EFAULT);
        CHECK(counter("vmctx_transfer_live")==0);CHECK(!mprotect(headers,4096,PROT_READ|PROT_WRITE));
    }
    CHECK(!begin_at(&first,addr) && first.ticket && counter("vmctx_transfer_live")==1);
    CHECK(!pthread_create(&worker,NULL,steal,&first));CHECK(!pthread_join(worker,NULL) && !worker_result);
    if(!strcmp(mode,"cancel")) {
        CHECK(!transfer(&first,VMCTX_TRANSFER_CANCEL,NULL));CHECK(!transfer(&first,VMCTX_TRANSFER_FORGET,NULL));
        CHECK(transfer(&first,VMCTX_TRANSFER_FORGET,NULL)==-1 && errno==ENOENT);
        CHECK(counter("vmctx_transfer_captures")==captures_before);pass=1;goto done;
    }
    if(!strcmp(mode,"replay")) {
        struct vmctx_transfer *bad=headers;*bad=first;bad->op=VMCTX_TRANSFER_CAPTURE;
        CHECK(!mprotect(headers,4096,PROT_READ));
        CHECK(syscall(ctl_nr,0,VMCTX_CTL_TRANSFER,bad)==-1 && errno==EFAULT);
        CHECK(!mprotect(headers,4096,PROT_READ|PROT_WRITE));
    }
    CHECK(!transfer(&first,VMCTX_TRANSFER_CAPTURE,NULL));
    CHECK(counter("vmctx_transfer_captures")==captures_before+1);
    CHECK(state(VMCTX_PG_TRANSIT));
    CHECK(transfer(&first,VMCTX_TRANSFER_CANCEL,NULL)==-1 && errno==EBUSY);
    CHECK(transfer(&first,VMCTX_TRANSFER_FORGET,NULL)==-1 && errno==EBUSY);
    CHECK(transfer(&first,VMCTX_TRANSFER_ACK,NULL)==-1 && errno==EINVAL);
    struct vmctx_pgack legacy={.addr=addr,.sum=first.sum};
    CHECK(control(VMCTX_CTL_PGACK,&legacy)==-1 && errno==EBUSY);
    if(!strcmp(mode,"replay")) {
        CHECK(transfer(&first,VMCTX_TRANSFER_READ,(void *)1)==-1 && errno==EFAULT);
        CHECK(!kill(child,0) && state(VMCTX_PG_TRANSIT));
        usleep(250000);struct vmctx_serve stale={.addr=addr,.buf=(uintptr_t)received};
        CHECK(!control(VMCTX_CTL_SERVE,&stale) && stale.status==VMCTX_SERVE_CLAIMING);
    }
    if(!strcmp(mode,"invalidate")) {
        CHECK(!assisted(SYS_munmap,addr,4096,0,0,0,0));
        CHECK(assisted(SYS_mmap,addr,4096,PROT_READ|PROT_WRITE,MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,UINT64_MAX,0)==(long)addr);
        memset(received,0xa5,sizeof(received));
        CHECK(transfer(&first,VMCTX_TRANSFER_READ,received)==-1 && errno==ESTALE);
        for(unsigned i=0;i<sizeof(received);i++)CHECK(received[i]==0xa5);
        CHECK(!transfer(&first,VMCTX_TRANSFER_CANCEL,NULL));CHECK(!transfer(&first,VMCTX_TRANSFER_FORGET,NULL));
        CHECK(state(VMCTX_PG_NONE));pass=1;goto done;
    }
    memset(received,0xa5,sizeof(received));CHECK(!transfer(&first,VMCTX_TRANSFER_READ,received));
    if(!strcmp(mode,"absent")) {
        CHECK(first.status==VMCTX_SERVE_ABSENT && (first.flags&VMCTX_SERVE_GRANT));
        for(unsigned i=0;i<sizeof(received);i++)CHECK(received[i]==0xa5);
    } else CHECK(!memcmp(expected,received,sizeof(expected)));
    CHECK(!transfer(&first,VMCTX_TRANSFER_READ,received));
    CHECK(!transfer(&first,VMCTX_TRANSFER_CAPTURE,NULL));
    CHECK(counter("vmctx_transfer_captures")==captures_before+1);
    if(!strcmp(mode,"abandon")) {
        struct vmctx_transfer bad={.version=VMCTX_TRANSFER_ABI,.size=sizeof(bad),
            .op=VMCTX_TRANSFER_ABANDON,.ticket=first.ticket};
        CHECK(syscall(ctl_nr,0,VMCTX_CTL_TRANSFER,&bad)==-1 && errno==EINVAL);
        CHECK(counter("vmctx_transfer_live")==1 && state(VMCTX_PG_TRANSIT));
        long abandoned=counter("vmctx_transfer_abandoned");
        CHECK(!source_transfer_abandon());
        CHECK(counter("vmctx_transfer_live")==0);
        CHECK(counter("vmctx_transfer_abandoned")==abandoned+1);
        CHECK(!source_transfer_abandon());
        CHECK(counter("vmctx_transfer_abandoned")==abandoned+1);
        CHECK(transfer(&first,VMCTX_TRANSFER_READ,received)==-1 && errno==ENOENT);
        /* The creating monitor thread is still alive here. Its source's
         * clear-TID access must finish before any thread-exit fallback. */
        do {reaped=waitpid(child,&status,0);}while(reaped<0 && errno==EINTR);
        CHECK(reaped==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
        pass=1;goto done;
    }
    if(!strcmp(mode,"ack-copyout")) {
        struct vmctx_transfer *bad=headers;*bad=first;bad->op=VMCTX_TRANSFER_ACK;bad->buf=0;
        CHECK(!mprotect(headers,4096,PROT_READ));
        CHECK(syscall(ctl_nr,0,VMCTX_CTL_TRANSFER,bad)==-1 && errno==EFAULT);
        CHECK(state(VMCTX_PG_REMOTE));
        CHECK(!mprotect(headers,4096,PROT_READ|PROT_WRITE));
    }
    CHECK(!transfer(&first,VMCTX_TRANSFER_ACK,NULL));CHECK(state(VMCTX_PG_REMOTE));
    CHECK(transfer(&first,VMCTX_TRANSFER_READ,received)==-1 && errno==EALREADY);
    if(shared) {
        struct vmctx_recall recall={.addr=addr};CHECK(!control(VMCTX_CTL_RECALL,&recall));
        recall.op=VMCTX_RECALL_COMMIT;recall.buf=(uintptr_t)expected;recall.gen=73;
        CHECK(syscall(ctl_nr,0,VMCTX_CTL_RECALL,&recall)==4096);
        CHECK(!begin_at(&second,addr) && second.ticket!=first.ticket);
        CHECK(!transfer(&second,VMCTX_TRANSFER_CAPTURE,NULL));CHECK(!transfer(&second,VMCTX_TRANSFER_READ,received));
        CHECK(second.sum==first.sum && state(VMCTX_PG_TRANSIT));
        CHECK(!transfer(&first,VMCTX_TRANSFER_ACK,NULL));CHECK(state(VMCTX_PG_TRANSIT));
        CHECK(control(VMCTX_CTL_PGACK,&legacy)==-1 && errno==EBUSY);
        struct vmctx_transfer forged=second;forged.mm_identity++;
        CHECK(transfer(&forged,VMCTX_TRANSFER_ACK,NULL)==-1 && errno==ESTALE && state(VMCTX_PG_TRANSIT));
        CHECK(!transfer(&second,VMCTX_TRANSFER_ACK,NULL));CHECK(state(VMCTX_PG_REMOTE));
        CHECK(!transfer(&second,VMCTX_TRANSFER_FORGET,NULL));
    }
    CHECK(!transfer(&first,VMCTX_TRANSFER_FORGET,NULL));CHECK(counter("vmctx_transfer_live")==0);pass=1;
done:
    if(connection.owned && source_custody_abandon(&connection))pass=0;
    if(child>0 && reaped!=child) {kill(child,SIGKILL);do {reaped=waitpid(child,&status,0);}while(reaped<0 && errno==EINTR);}
    if(adapter_source)source_record_release(adapter_source);
    alarm(0);if(mapping!=MAP_FAILED)munmap(mapping,mapping_size);if(headers!=MAP_FAILED)munmap(headers,4096);if(fd>=0)close(fd);
    printf("%s: native transfer custody %s\n",pass?"PASS":"FAIL",mode);
    return pass && !expired && reaped==child ? 0:1;
}
