/* SPDX-License-Identifier: GPL-2.0 */
/* Exercise production exception reply construction at the native boundary.
 * A failed control operation is independent of retained terminal publication. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <unistd.h>
static long exception_syscall(long number,...);
#define syscall exception_syscall
#define main vmhome_program_main
#include "../../user/vmhome.c"
#undef main
#undef syscall

static struct vmctx_context native;
static int native_fd,failed_command,terminal,delay,failed;
static unsigned entries,queries,deliveries;
static long exception_syscall(long number,...)
{
    assert(number==999);
    va_list ap;va_start(ap,number);int selector=va_arg(ap,int);
    unsigned command=va_arg(ap,unsigned);void *arg=va_arg(ap,void *);va_end(ap);
    assert(selector==-native_fd-1);
    if(command==VMCTX_CTL_CONTEXT) {
        struct vmctx_context *q=arg;assert(q->op==VMCTX_CONTEXT_INFO);queries++;
        if(failed && terminal && !delay--)native.flags=VMCTX_CONTEXT_ENDED;
        *q=native;return 0;
    }
    entries++;
    if(command==(unsigned)failed_command) {failed=1;errno=ETIMEDOUT;return -1;}
    if(command==VMCTX_CTL_EXCEPTION) {
        struct vmctx_syscall *call=arg;assert(call->nr==14);deliveries++;
        call->ret=0;call->regs.rip=0x5678;return 0;
    }
    if(command==VMCTX_CTL_GETCPU) {
        *(struct vmr_cpu_state *)arg=(struct vmr_cpu_state){.xstate_size=576};return 0;
    }
    assert(command==VMCTX_CTL_SETCPU || command==VMCTX_CTL_RUNTIME);return 0;
}

int main(int argc,char **argv)
{
    assert(argc==2);alarm(10);vmctx_ctl_nr=999;
    const char *mode=argv[1];terminal=strcmp(mode,"live")!=0;
    failed_command=VMCTX_CTL_EXCEPTION;
    if(!strcmp(mode,"runtime"))failed_command=VMCTX_CTL_RUNTIME;
    if(!strcmp(mode,"setcpu"))failed_command=VMCTX_CTL_SETCPU;
    if(!strcmp(mode,"getcpu"))failed_command=VMCTX_CTL_GETCPU;
    if(!strcmp(mode,"handler") || !strcmp(mode,"drift"))failed_command=0;
    if(!strcmp(mode,"delayed"))delay=3;
    native_fd=open("/dev/null",O_RDONLY|O_CLOEXEC);assert(native_fd>=0);
    native=(struct vmctx_context){.version=VMCTX_CONTEXT_ABI,.size=sizeof(native),
        .op=VMCTX_CONTEXT_INFO,.identity=91,.mm_identity=UINT64_C(0x8000000100000007),
        .flags=VMCTX_CONTEXT_READY,.fd=-1,.exit_status=SIGSEGV|128};
    struct source_context context={.fd=native_fd,.identity=native.identity};
    source_id id=source_record_add(&context);assert(id==1);
    struct vmr_cpu_state cpu={.xstate_size=576};
    struct vmr_req request={.nr=VMR_NR_EXCEPTION,.args={14,4,0},
        .datalen=vmr_cpu_wire_size(&cpu),.execution_epoch=1,.execution_ns=17};
    if(!strcmp(mode,"malformed"))request.datalen--;
    struct vmr_rsp reply;
    unsigned before=queries;
    assert(!source_exception_reply(id,&request,&cpu,&reply));
    if(!strcmp(mode,"malformed")) {
        assert(reply.retval==-EINVAL && !reply.ended && !reply.datalen);
        assert(!entries && queries==before);
    } else if(!strcmp(mode,"handler") || !strcmp(mode,"drift")) {
        assert(!reply.retval && !reply.ended && deliveries==1);
        assert(reply.datalen==vmr_cpu_wire_size(&cpu) && cpu.regs.rip==0x5678);
    } else if(!terminal) {
        assert(reply.retval==-ETIMEDOUT && !reply.ended && !reply.datalen);
        assert(queries>=before+100);
    } else {
        assert(!reply.retval && reply.ended==1 && !reply.datalen);
        assert(reply.ended_status==(SIGSEGV|128));
        assert(queries>=before+(!strcmp(mode,"delayed") ? 5:2));
    }
    if(!strcmp(mode,"drift")) {
        uint64_t saved=reply.binding.mm;native.mm_identity++;
        assert(source_publish_reply(id,&reply)==-1 && errno==ESTALE);
        assert(reply.binding.mm==saved);
    } else assert(!source_publish_reply(id,&reply));
    native.flags=VMCTX_CONTEXT_ENDED;source_record_release(id);
    printf("PASS: source exception %s requires native terminal proof and preserves CPU/binding outcome\n",mode);
    return 0;
}
