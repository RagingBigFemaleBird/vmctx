/* SPDX-License-Identifier: GPL-2.0 */
/* Production source adapter with faults injected at the native boundary.
 * Count destructive captures independently of user-visible syscall results. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <unistd.h>
static long transfer_syscall(long number,...);
#define syscall transfer_syscall
#define main vmhome_program_main
#include "../../user/vmhome.c"
#undef main
#undef syscall
#include "../../user/source-custody.h"

static struct native_context {int fd;struct vmctx_context info;} contexts[2];
static struct native_ticket {
    uint64_t id,mm,address;
    int captured,read,settled;
    unsigned status,flags,state;
} tickets[96];
static unsigned infos,begins,captures,reads,acks,abandoned;
static int selected,drift,fail_op=-1,bad_read,bad_caps,invalidated;
static uint64_t next_ticket=UINT64_C(0x8000000100000000);
static unsigned capture_status=VMCTX_SERVE_TAKEN,capture_flags,capture_state=VMCTX_PG_HOME;
static int capture_busy,close_trace=-1;

static long fail(int error) {errno=error;return -1;}
static int fail_once(unsigned op)
{if(fail_op!=(int)op)return 0;fail_op=-1;return 1;}
static long transfer_syscall(long number,...)
{
    assert(number==999);
    va_list ap;va_start(ap,number);int selector=va_arg(ap,int);
    unsigned command=va_arg(ap,unsigned);void *argument=va_arg(ap,void *);va_end(ap);
    if(selector) {
        unsigned i;for(i=0;i<2;i++)if(selector==-contexts[i].fd-1)break;assert(i<2);
        if(command==VMCTX_CTL_CONTEXT) {
            struct vmctx_context *q=argument;
            if(q->op==VMCTX_CONTEXT_SIGNAL && close_trace>=0)
                assert(write(close_trace,"S",1)==1);
            *(struct vmctx_context *)argument=contexts[i].info;infos++;return 0;
        }
        assert(command==VMCTX_CTL_MEMORY);
        struct vmctx_memory *memory=argument;
        assert(memory->command==VMCTX_CTL_TRANSFER && memory->op==VMCTX_MEMORY_CALL);
        struct vmctx_transfer *q=(void *)(uintptr_t)memory->argument;
        assert(q->op==VMCTX_TRANSFER_BEGIN && q->mm_identity==memory->mm_identity);
        begins++;selected=i;
        if(drift) {contexts[i].info.mm_identity++;drift=0;}
        if(memory->mm_identity!=contexts[i].info.mm_identity)return fail(ESTALE);
        if(fail_once(VMCTX_TRANSFER_BEGIN))return fail(EFAULT);
        unsigned slot;for(slot=0;slot<96;slot++)if(!tickets[slot].id)break;assert(slot<96);
        tickets[slot]=(struct native_ticket){.id=++next_ticket,.mm=q->mm_identity,.address=q->address};
        q->ticket=tickets[slot].id;return 0;
    }
    assert(command==VMCTX_CTL_TRANSFER);
    struct vmctx_transfer *q=argument;
    if(q->op==VMCTX_TRANSFER_CAPS) {
        q->features=bad_caps ? VMCTX_TRANSFER_EXACT:VMCTX_TRANSFER_FEATURES;return 0;
    }
    if(q->op==VMCTX_TRANSFER_ABANDON) {
        assert(!q->mm_identity && !q->address && !q->ticket && !q->buf);
        if(fail_once(q->op))return fail(EIO);
        if(close_trace>=0)assert(write(close_trace,"A",1)==1);
        for(unsigned i=0;i<96;i++) {
            if(tickets[i].id && tickets[i].captured && !tickets[i].settled)abandoned++;
            tickets[i]=(struct native_ticket){0};
        }
        return 0;
    }
    unsigned i;for(i=0;i<96;i++)if(q->ticket==tickets[i].id)break;
    if(i==96)return fail(ENOENT);
    struct native_ticket *t=&tickets[i];
    assert(q->mm_identity==t->mm && q->address==t->address);
    if(q->op==VMCTX_TRANSFER_CAPTURE) {
        assert(!t->settled);
        if(capture_busy) {capture_busy=0;return fail(EBUSY);}
        if(!t->captured) {
            t->captured=1;captures++;
            t->status=capture_status;t->flags=capture_flags;t->state=capture_state;
        }
    } else if(q->op==VMCTX_TRANSFER_READ) {
        assert(t->captured && !t->settled);t->read=1;reads++;
        if(t->status==VMCTX_SERVE_TAKEN || t->status==VMCTX_SERVE_COPIED)
            memset((void *)(uintptr_t)q->buf,0x51+(t->address==0x5000),4096);
    } else if(q->op==VMCTX_TRANSFER_ACK) {
        assert(t->read);t->settled=1;acks++;
        if(invalidated)return fail(ESTALE);
    } else if(q->op==VMCTX_TRANSFER_CANCEL) {
        if(t->captured && (t->status==VMCTX_SERVE_TAKEN || t->flags&VMCTX_SERVE_GRANT) &&
           !t->settled && !invalidated)return fail(EBUSY);
        t->settled=1;
    } else {
        assert(q->op==VMCTX_TRANSFER_FORGET && t->settled);
        if(fail_once(q->op))return fail(EIO);
        *t=(struct native_ticket){0};return 0;
    }
    if(fail_once(q->op))return fail(EFAULT);
    q->features=VMCTX_TRANSFER_FEATURES;
    q->status=t->captured ? t->status:0;q->flags=t->flags;
    q->class=t->captured ? VMCTX_PGC_VALID|
        (t->status==VMCTX_SERVE_COPIED ? VMCTX_PGC_RO:VMCTX_PGC_WRITE):0;
    q->state=t->state;q->gen=17;q->sum=42;
    if(q->op==VMCTX_TRANSFER_READ && bad_read)q->gen++;
    return 0;
}

static void *foreign_custody_thread(void *opaque)
{
    struct source_custody *custody=opaque;
    assert(source_custody_abandon(custody)==-1 && errno==EPERM);
    return NULL;
}

static void connection_custody(const struct source_memory_target *current,
        const struct source_memory_target *old)
{
    invalidated=0;contexts[1].info.mm_identity=old->mm;
    const struct vmr_mm_binding binding[2]={
        {.context=current->context,.mm=current->mm,.epoch=current->epoch},
        {.context=old->context,.mm=old->mm,.epoch=old->epoch}};
    struct source_custody custody={0};struct source_custody_entry *entry;
    unsigned char bytes[4096];uint64_t ids[80];
    unsigned initial_captures=captures,initial_acks=acks;
    for(unsigned i=0;i<80;i++) {
        uint64_t address=0x10000+(i/2)*4096;
        assert(!source_custody_begin(&custody,&binding[i&1],address,&entry));
        ids[i]=entry->transfer.ticket;
        assert(!source_transfer_capture(&entry->transfer));
        assert(!source_transfer_read(&entry->transfer,bytes));
        assert(ids[i]>UINT32_MAX && custody.count==i+1);
    }
    assert(captures==initial_captures+80 && acks==initial_acks);
    pthread_t thread;assert(!pthread_create(&thread,NULL,foreign_custody_thread,&custody));
    assert(!pthread_join(thread,NULL) && custody.count==80);
    /* Matching address/checksum/low identity bits cannot retire another MM
     * or another episode. Validate the complete saved binding as well. */
    struct vmr_mm_binding forged=binding[0];forged.epoch++;
    assert(source_custody_ack(&custody,&forged,0x10000,ids[0])==-1 && errno==ESTALE);
    forged=binding[0];forged.parent_mm=99;
    assert(source_custody_ack(&custody,&forged,0x10000,ids[0])==-1 && errno==ESTALE);
    assert(source_custody_ack(&custody,&binding[1],0x10000,ids[0])==-1 && errno==ESTALE);
    assert(source_custody_ack(&custody,&binding[0],0x11000,ids[0])==-1 && errno==ESTALE);
    assert(source_custody_ack(&custody,&binding[0],0x10000,(uint32_t)ids[0])==-1 && errno==ENOENT);
    assert(custody.count==80 && acks==initial_acks);
    for(unsigned i=80;i--;) {
        uint64_t address=0x10000+(i/2)*4096;
        assert(!source_custody_ack(&custody,&binding[i&1],address,ids[i]));
        assert(custody.count==i);
    }
    assert(!custody.pending && acks==initial_acks+80);
    assert(source_custody_ack(&custody,&binding[0],0x10000,ids[0])==-1 && errno==ENOENT);
    fail_op=VMCTX_TRANSFER_BEGIN;
    assert(source_custody_begin(&custody,&binding[0],0x4000,&entry)==-1 && !entry && !custody.count);
    assert(!source_custody_begin(&custody,&binding[0],0x4000,&entry));
    uint64_t id=entry->transfer.ticket;
    fail_op=VMCTX_TRANSFER_CAPTURE;
    assert(source_transfer_capture(&entry->transfer)==-1 && errno==EFAULT);
    assert(custody.count==1 && entry->transfer.ticket==id);
    assert(source_custody_cancel(&custody,&binding[0],0x4000,id)==-1 && errno==EBUSY);
    assert(!source_transfer_capture(&entry->transfer));
    assert(source_custody_ack(&custody,&binding[0],0x4000,id)==-1 && errno==EINVAL);
    assert(!source_transfer_read(&entry->transfer,bytes));
    fail_op=VMCTX_TRANSFER_ACK;
    assert(source_custody_ack(&custody,&binding[0],0x4000,id)==-1 && errno==EFAULT && custody.count==1);
    fail_op=VMCTX_TRANSFER_FORGET;
    assert(source_custody_ack(&custody,&binding[0],0x4000,id)==-1 && errno==EIO && custody.count==1);
    assert(entry->transfer.settled);unsigned settled_acks=acks;
    assert(!source_custody_ack(&custody,&binding[0],0x4000,id) && !custody.count && acks==settled_acks);
    assert(!source_custody_begin(&custody,&binding[0],0x4000,&entry));id=entry->transfer.ticket;
    assert(!source_transfer_capture(&entry->transfer) && !source_transfer_read(&entry->transfer,bytes));
    invalidated=1;
    assert(source_custody_ack(&custody,&binding[0],0x4000,id)==-1 && errno==ESTALE && !custody.count);
    invalidated=0;
    assert(!source_custody_begin(&custody,&binding[0],0x4000,&entry));
    assert(!source_transfer_capture(&entry->transfer) && !source_transfer_read(&entry->transfer,bytes));
    unsigned before_abandon=abandoned;fail_op=VMCTX_TRANSFER_ABANDON;
    assert(source_custody_abandon(&custody)==-1 && errno==EIO && custody.count==1 && custody.closed);
    assert(source_custody_begin(&custody,&binding[0],0x5000,&entry)==-1 && errno==ESHUTDOWN);
    assert(!source_custody_abandon(&custody) && !custody.count && !custody.pending && abandoned==before_abandon+1);
    assert(!source_custody_abandon(&custody) && abandoned==before_abandon+1);
    puts("PASS: connection custody retains 80 full-identity receipts, rejects cross-MM/thread ACKs, preserves uncertain settlement and stops new work before abandonment cleanup");
}

/* The production CTXPAGE/ACK helpers, not only the custody component. Native
 * facts are selected independently, and every issued handle is accounted for. */
static void wire_custody(const struct source_memory_target *current,
        const struct source_memory_target *old)
{
    invalidated=0;contexts[1].info.mm_identity=old->mm;
    const struct vmr_mm_binding targets[2]={
        {.context=old->context,.mm=old->mm,.epoch=old->epoch},
        {.context=current->context,.mm=current->mm,.epoch=current->epoch}};
    struct source_custody custody={0};struct source_page_receipt receipt[2];
    char bytes[4096];unsigned before=acks;
    for(unsigned i=0;i<2;i++) {
        assert(ctx_page(&custody,&targets[i],0x4000,4096,bytes,sizeof(bytes),0,&receipt[i])==4096);
        assert(selected==(i ? 0:1) && receipt[i].count==1 && receipt[i].taken==1);
        assert(vmr_binding_equal(&receipt[i].target,&targets[i]) && receipt[i].generation[0]==17);
        assert(receipt[i].episode>UINT32_MAX && custody.count==i+1 && bytes[0]==0x51 && acks==before);
    }
    assert(receipt[0].episode!=receipt[1].episode);
    assert(ctx_page_ack(&custody,&targets[0],0x4000,(uint32_t)receipt[0].episode)==-ENOENT);
    assert(ctx_page_ack(&custody,&targets[1],0x4000,receipt[0].episode)==-ESTALE);
    assert(ctx_page_ack(&custody,&targets[0],0x4001,receipt[0].episode)==-EINVAL);
    assert(custody.count==2 && acks==before);
    fail_op=VMCTX_TRANSFER_ACK;
    assert(ctx_page_ack(&custody,&targets[0],0x4000,receipt[0].episode)==-EFAULT && custody.count==2);
    assert(!ctx_page_ack(&custody,&targets[0],0x4000,receipt[0].episode));
    assert(!ctx_page_ack(&custody,&targets[1],0x4000,receipt[1].episode) && !custody.count);
    assert(ctx_page_ack(&custody,&targets[0],0x4000,receipt[0].episode)==-ENOENT);

    capture_status=VMCTX_SERVE_COPIED;before=acks;
    assert(ctx_page(&custody,&targets[0],0x4000,4096,bytes,sizeof(bytes),0,&receipt[0])==4096);
    assert(custody.count==1 && receipt[0].episode && receipt[0].taken==1 && acks==before);
    invalidated=1;
    assert(ctx_page_ack(&custody,&targets[0],0x4000,receipt[0].episode)==-ESTALE && !custody.count);
    invalidated=0;

    capture_status=VMCTX_SERVE_ABSENT;capture_flags=VMCTX_SERVE_GRANT;capture_state=VMCTX_PG_NONE;
    memset(bytes,0xcc,sizeof(bytes));
    assert(ctx_page(&custody,&targets[0],0x4000,4096,bytes,sizeof(bytes),0,&receipt[0])==4096);
    for(unsigned i=0;i<sizeof(bytes);i++)assert(!bytes[i]);
    assert(custody.count==1 && receipt[0].episode && !ctx_page_ack(&custody,&targets[0],0x4000,receipt[0].episode));
    capture_flags=0;
    const unsigned statuses[]={VMCTX_SERVE_ABSENT,VMCTX_SERVE_CLAIMING,VMCTX_SERVE_DENIED,VMCTX_SERVE_NOMAP};
    const long results[]={VMR_CTXPAGE_NOTHOLDER,VMR_CTXPAGE_CLAIMING,-EACCES,-EFAULT};
    for(unsigned i=0;i<4;i++) {
        capture_status=statuses[i];capture_state=VMCTX_PG_REMOTE;
        unsigned old_reads=reads;memset(bytes,0x63,sizeof(bytes));
        assert(ctx_page(&custody,&targets[0],0x4000,4096,bytes,sizeof(bytes),0,&receipt[0])==results[i]);
        assert(!custody.count && !receipt[0].episode && !receipt[0].count && reads==old_reads);
        for(unsigned j=0;j<sizeof(bytes);j++)assert(bytes[j]==0x63);
    }
    capture_status=VMCTX_SERVE_TAKEN;capture_state=VMCTX_PG_HOME;capture_busy=1;
    assert(ctx_page(&custody,&targets[0],0x4000,4096,bytes,sizeof(bytes),0,&receipt[0])==VMR_CTXPAGE_CLAIMING && !custody.count);
    fail_op=VMCTX_TRANSFER_READ;memset(bytes,0x63,sizeof(bytes));
    assert(ctx_page(&custody,&targets[0],0x4000,4096,bytes,sizeof(bytes),0,&receipt[0])==VMR_CTXPAGE_FAILED);
    assert(custody.count==1 && !receipt[0].episode);
    for(unsigned i=0;i<sizeof(bytes);i++)assert(bytes[i]==0x63);
    int pipefd[2];assert(!pipe(pipefd));pid_t child=fork();assert(child>=0);
    if(!child) {
        close(pipefd[0]);close_trace=pipefd[1];
        source_connection_close(&custody);_exit(0);
    }
    close(pipefd[1]);char trace[8]={0};size_t total=0;ssize_t got;
    while((got=read(pipefd[0],trace+total,sizeof(trace)-total))>0)total+=(size_t)got;
    close(pipefd[0]);int status;assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==98 && total==3 && !memcmp(trace,"ASS",3));
    assert(!source_custody_abandon(&custody) && !custody.count);
    puts("PASS: production CTXPAGE carries exact full64 custody through old-MM routing, read-only copies, zero grants, rejected ACKs and cleanup before session cancellation");
}

int main(void)
{
    alarm(10);vmctx_ctl_nr=999;
    const uint64_t mm=UINT64_C(0x100000007);
    source_id ids[2];
    for(unsigned i=0;i<2;i++) {
        contexts[i].fd=open("/dev/null",O_RDONLY|O_CLOEXEC);assert(contexts[i].fd>=0);
        contexts[i].info=(struct vmctx_context){.version=VMCTX_CONTEXT_ABI,
            .size=sizeof(struct vmctx_context),.op=VMCTX_CONTEXT_INFO,.identity=30+i,
            .mm_identity=mm,.flags=VMCTX_CONTEXT_READY,.fd=-1};
        struct source_context handle={.fd=contexts[i].fd,.identity=contexts[i].info.identity};
        ids[i]=source_record_add(&handle);assert(ids[i]>0 && handle.fd==-1);
    }
    struct source_memory_target target,current;
    assert(!source_memory_target_capture(ids[0],&target));
    bad_caps=1;assert(source_transfer_caps()==-1 && errno==EPROTO);
    bad_caps=0;assert(!source_transfer_caps());
    struct source_transfer first={0},second={0};
    assert(source_transfer_capture(NULL)==-1 && errno==EINVAL);
    contexts[0].info.mm_identity+=UINT64_C(1)<<32;
    assert(!source_memory_target_capture(ids[0],&current));
    drift=1;
    assert(source_transfer_begin(&target,0x4000,&first)==-1 && errno==ESTALE && !first.begun);
    assert(begins==1 && !captures);
    contexts[1].info.mm_identity=mm;
    fail_op=VMCTX_TRANSFER_BEGIN;
    assert(source_transfer_begin(&target,0x4000,&first)==-1 && errno==EFAULT && !first.begun);
    assert(!source_transfer_begin(&target,0x4000,&first) && selected==1 && first.ticket>UINT32_MAX);
    uint64_t saved_id=first.ticket;
    unsigned saved_infos=infos,saved_begins=begins;
    contexts[1].info.mm_identity+=UINT64_C(1)<<33; /* No later operation consults INFO. */
    assert(source_transfer_begin(&current,0x4000,&first)==-1 && errno==EINVAL);
    fail_op=VMCTX_TRANSFER_CAPTURE;
    assert(source_transfer_capture(&first)==-1 && errno==EFAULT && !first.captured && first.ticket==saved_id);
    assert(!source_transfer_capture(&first) && first.captured && captures==1);
    assert(source_transfer_ack(&first)==-1 && errno==EINVAL && !acks);
    assert(source_transfer_cancel(&first)==-1 && errno==EBUSY && !first.settled);
    unsigned char bytes[4096],before[4096];memset(bytes,0xcc,sizeof(bytes));memcpy(before,bytes,sizeof(bytes));
    fail_op=VMCTX_TRANSFER_READ;
    assert(source_transfer_read(&first,bytes)==-1 && errno==EFAULT && !first.read);
    assert(!memcmp(bytes,before,sizeof(bytes)));
    bad_read=1;
    assert(source_transfer_read(&first,bytes)==-1 && errno==EPROTO && !first.read);
    assert(!memcmp(bytes,before,sizeof(bytes)) && first.result.gen==17);
    bad_read=0;assert(!source_transfer_read(&first,bytes) && first.read);
    for(unsigned i=0;i<sizeof(bytes);i++)assert(bytes[i]==0x51);
    assert(!source_transfer_capture(&first) && captures==1);
    assert(infos==saved_infos && begins==saved_begins);
    fail_op=VMCTX_TRANSFER_ACK;
    assert(source_transfer_ack(&first)==-1 && errno==EFAULT && !first.settled && first.ticket==saved_id);
    assert(!source_transfer_ack(&first) && first.settled && acks==2);
    assert(!source_transfer_ack(&first) && acks==2);
    fail_op=VMCTX_TRANSFER_FORGET;
    assert(source_transfer_forget(&first)==-1 && errno==EIO && first.ticket==saved_id);
    assert(!source_transfer_begin(&current,0x5000,&second) && first.ticket==saved_id && second.ticket!=saved_id);
    assert(!source_transfer_cancel(&second) && !source_transfer_forget(&second));
    assert(!source_transfer_forget(&first) && !first.begun && !first.ticket);
    assert(!source_transfer_begin(&current,0x5000,&second));
    assert(!source_transfer_capture(&second) && !source_transfer_read(&second,bytes));
    invalidated=1;assert(!source_transfer_cancel(&second) && second.cancelled);
    unsigned before_acks=acks;
    assert(source_transfer_ack(&second)==-1 && errno==ECANCELED && acks==before_acks);
    assert(!source_transfer_forget(&second));
    assert(!source_transfer_begin(&current,0x5000,&second));
    assert(!source_transfer_capture(&second) && !source_transfer_read(&second,bytes));
    assert(!source_transfer_abandon() && abandoned==1);
    assert(!source_transfer_abandon() && abandoned==1);
    assert(source_transfer_read(&second,bytes)==-1 && errno==ENOENT);
    connection_custody(&current,&target);
    wire_custody(&current,&target);
    for(unsigned i=0;i<2;i++) {
        contexts[i].info.flags=VMCTX_CONTEXT_ENDED;source_record_release(ids[i]);
        assert(fcntl(contexts[i].fd,F_GETFD)==-1 && errno==EBADF);
    }
    puts("PASS: native custody adapter retains exact MM/tickets across exec and failed capture/read/ACK/FORGET, stages copyout and rejects receipt drift");
    return 0;
}
