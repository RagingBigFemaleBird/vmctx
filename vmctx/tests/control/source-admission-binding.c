/* SPDX-License-Identifier: GPL-2.0 */
/* Drive production admission/completion with controlled native state changes.
 * Every assertion is at the protocol boundary or an observed native entry. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <unistd.h>
static long admission_syscall(long number,...);
#define syscall admission_syscall
#define main vmhome_program_main
#ifndef VMHOME_SOURCE
#define VMHOME_SOURCE "../../user/vmhome.c"
#endif
#include VMHOME_SOURCE
#undef main
#undef syscall

static struct admission_native {int fd;struct vmctx_context info;} model[3];
static struct vmctx_syscall_gate native_gate;
static unsigned dispatches,access_entries,access_commits;
static int move_at_access;
static long admission_syscall(long number,...)
{
    assert(number==999);
    va_list ap;va_start(ap,number);int selector=va_arg(ap,int);
    unsigned command=va_arg(ap,unsigned);void *arg=va_arg(ap,void *);va_end(ap);
    int i;for(i=0;i<3;i++)if(selector==-model[i].fd-1)break;assert(i<3);
    if(command==VMCTX_CTL_CONTEXT) {
        struct vmctx_context *q=arg;
        if(q->op==VMCTX_CONTEXT_SIGNAL)return 0;
        if(q->op==VMCTX_CONTEXT_CHILD) {*q=model[1].info;q->fd=model[1].fd;return 0;}
        assert(q->op==VMCTX_CONTEXT_INFO);*q=model[i].info;return 0;
    }
    if(command==VMCTX_CTL_SYSCALL_GATE) {
        struct vmctx_syscall_gate *q=arg;
        assert(q->ticket==native_gate.ticket);
        if(q->op==VMCTX_GATE_COMMIT) {dispatches++;native_gate.state=VMCTX_GATE_RUNNING;}
        else assert(q->op==VMCTX_GATE_QUERY);
        *q=native_gate;return 0;
    }
    if(command==VMCTX_CTL_GETCPU) {
        *(struct vmr_cpu_state *)arg=(struct vmr_cpu_state){.xstate_size=576};return 0;
    }
    uint64_t expected=0;
    if(command==VMCTX_CTL_MEMORY) {
        struct vmctx_memory *memory=arg;
        assert(memory->op==VMCTX_MEMORY_CALL);
        expected=memory->mm_identity;command=memory->command;arg=(void *)(uintptr_t)memory->argument;
    }
    if(command==VMCTX_CTL_ACCESS_LOG) {
        access_entries++;assert(i==0);
        if(move_at_access) {model[0].info.mm_identity+=100;move_at_access=0;}
    }
    if(expected && expected!=model[i].info.mm_identity) {errno=ESTALE;return -1;}
    if(command==VMCTX_CTL_MMLOG) {
        struct vmctx_mmlog *q=arg;q->cur=8;q->n=0;return 0;
    }
    if(command==VMCTX_CTL_ACCESS_LOG) {
        struct vmctx_access_log *q=arg;q->mm_id=model[i].info.mm_identity;
        q->construction=9;access_commits++;return 0;
    }
    assert(0);return -1;
}
int main(int argc,char **argv)
{
    assert(argc==2);alarm(10);vmctx_ctl_nr=999;
    const char *mode=argv[1];const uint64_t old_mm=UINT64_C(0x100000007);
    for(unsigned i=0;i<3;i++) {
        model[i].fd=open("/dev/null",O_RDONLY|O_CLOEXEC);assert(model[i].fd>=0);
        model[i].info=(struct vmctx_context){.version=VMCTX_CONTEXT_ABI,.size=sizeof(struct vmctx_context),
            .op=VMCTX_CONTEXT_INFO,.identity=90+i,.mm_identity=old_mm+(i==1),
            .flags=VMCTX_CONTEXT_READY,.fd=-1};
    }
    struct source_context parent={.fd=model[0].fd,.identity=90};
    source_id id=source_record_add(&parent);assert(id==1);
    struct source_context peer={.fd=model[2].fd,.identity=92};assert(source_record_add(&peer)==2);
    struct source_admission call={.active=1,.begun=1,.next=1,
        .gate={.ticket=1}};
    native_gate=(struct vmctx_syscall_gate){.version=VMCTX_SYSCALL_GATE_ABI,
        .size=sizeof(native_gate),.ticket=1,.state=VMCTX_GATE_ADMITTED,
        .call={.nr=SYS_mmap,.args={0,4096,PROT_READ,MAP_PRIVATE,UINT64_MAX,0}}};
    if(!strcmp(mode,"birth"))native_gate.call.nr=SYS_fork;
    if(!strcmp(mode,"exec"))native_gate.call.nr=SYS_execve;
    struct vmr_req request={.nr=VMR_OP_CALL_POLL,.ticket=1};
    struct vmr_rsp reply;struct vmr_cpu_state cpu={0};
    assert(source_admission_step(id,&call,&request,&cpu,&reply)==0);
    assert(reply.call_state==VMR_CALL_ADMITTED && dispatches==0);
    request.nr=VMR_OP_EXECUTE;
    if(!strcmp(mode,"binding-drift") || !strcmp(mode,"args-drift")) {
        if(!strcmp(mode,"binding-drift"))model[0].info.mm_identity+=100;
        else native_gate.call.args[1]=8192;
        assert(source_admission_step(id,&call,&request,&cpu,&reply)==-1 && errno==ESTALE);
        assert(dispatches==0);
    } else {
        assert(source_admission_step(id,&call,&request,&cpu,&reply)==0 && dispatches==1);
        request.nr=VMR_OP_CALL_POLL;
        if(!strcmp(mode,"birth")) {
            model[0].info.mm_identity+=100;
            native_gate.child_host_pid=native_gate.child_pid=99;
            assert(source_admission_step(id,&call,&request,&cpu,&reply)==0);
            assert(reply.effects==VMR_EFFECT_CHILD && reply.child_binding.mm==old_mm+1);
            assert(reply.child_binding.parent_mm==old_mm && fork_parent_mm(old_mm+1)==old_mm);
        } else {
            native_gate.state=VMCTX_GATE_COMPLETE;native_gate.dispatch_ret=0x4000;
            if(!strcmp(mode,"exec")) {
                native_gate.dispatch_ret=0;model[0].info.mm_identity+=100;
            }
            assert(source_admission_step(id,&call,&request,&cpu,&reply)==1);
            move_at_access=!strcmp(mode,"construction");
            int result=source_admission_complete(id,&call,&reply,&cpu,0);
            if(!strcmp(mode,"construction")) {
                assert(result==-1 && errno==ESTALE && access_entries==1 && access_commits==0);
            } else {
                assert(result==0);
                if(!strcmp(mode,"exec")) {
                    assert(reply.effects&VMR_EFFECT_IMAGE);
                    assert(reply.binding.mm==old_mm+100 && !reply.mapping.kind);
                } else {
                    assert(reply.mapping.kind==VMR_MAP_SET && reply.mmseq==9);
                }
                if(!strcmp(mode,"publication")) {
                    model[0].info.mm_identity+=100;
                    assert(source_publish_reply(id,&reply)==-1 && errno==ESTALE);
                    assert(reply.binding.mm==old_mm);
                } else {
                    assert(!source_publish_reply(id,&reply));
                    assert(reply.binding.mm==model[0].info.mm_identity);
                }
            }
        }
    }
    for(size_t n=0;n<source_records_n;n++) {
        for(unsigned i=0;i<3;i++)model[i].info.flags=VMCTX_CONTEXT_ENDED;
        source_record_release((source_id)n+1);
    }
    if(strcmp(mode,"birth"))close(model[1].fd);
    printf("PASS: admission binding %s preserves captured MM, task and operation identity\n",mode);
    return 0;
}
