/* SPDX-License-Identifier: GPL-2.0 */
/* Production source helpers, with a controlled native boundary that records
 * every memory entry and its MM before permitting an inner commit. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <unistd.h>
static long bound_syscall(long number,...);
#define syscall bound_syscall
#define main vmhome_program_main
#ifndef VMHOME_SOURCE
#define VMHOME_SOURCE "../../user/vmhome.c"
#endif
#include VMHOME_SOURCE
#undef main
#undef syscall

static struct bound_native {int fd;struct vmctx_context info;unsigned char bytes[4096];} model[2];
static unsigned calls,commits;
static int selected,move_at_entry,fail_after_commit,probe_error,mm_ended;
static long bound_syscall(long number,...)
{
    assert(number==999);
    va_list ap;va_start(ap,number);int selector=va_arg(ap,int);unsigned command=va_arg(ap,unsigned);
    void *argument=va_arg(ap,void *);va_end(ap);
    if(!selector) {
        assert(command==VMCTX_CTL_CONTEXT);
        struct vmctx_context *query=argument;assert(query->op==VMCTX_CONTEXT_MMINFO);
        query->flags=mm_ended ? VMCTX_CONTEXT_MM_ENDED : VMCTX_CONTEXT_MM_LIVE;
        return 0;
    }
    int i;for(i=0;i<2;i++)if(selector==-model[i].fd-1)break;assert(i<2);
    if(command==VMCTX_CTL_CONTEXT) {*(struct vmctx_context *)argument=model[i].info;return 0;}
    assert(command==VMCTX_CTL_MEMORY);
    struct vmctx_memory *q=argument;calls++;selected=i;
    assert(q->version==VMCTX_MEMORY_ABI && q->op==VMCTX_MEMORY_CALL);
    if(move_at_entry) {model[i].info.mm_identity++;move_at_entry=0;}
    if(q->mm_identity!=model[i].info.mm_identity) {errno=ESTALE;return -1;}
    if(probe_error) {errno=EIO;return -1;}
    void *inner=(void *)(uintptr_t)q->argument;long result=0;
    switch(q->command) {
    case VMCTX_CTL_LAND: {
        struct vmctx_land *land=inner;memcpy(model[i].bytes,(void *)(uintptr_t)land->buf,4096);
        result=4096;break;
    }
    case VMCTX_CTL_PEEK: {
        struct vmctx_mem *memory=inner;assert(memory->len<=4096);
        memcpy((void *)(uintptr_t)memory->buf,model[i].bytes,memory->len);result=memory->len;break;
    }
    case VMCTX_CTL_SERVE: {
        struct vmctx_serve *serve=inner;serve->state=PG_OURS;serve->gen=73+i;
        serve->status=VMCTX_SERVE_COPIED;serve->class=VMCTX_PGC_VALID|VMCTX_PGC_PRESENT;
        if(!(serve->flags&VMCTX_SERVE_PROBE))memcpy((void *)(uintptr_t)serve->buf,model[i].bytes,4096);
        break;
    }
    case VMCTX_CTL_PGSTATE: {
        struct vmctx_serve *state=inner;state->state=PG_OURS;state->gen=73;break;
    }
    default:assert(0);
    }
    commits++;
    if(fail_after_commit) {errno=EFAULT;return -1;}
    return result;
}
int main(int argc,char **argv)
{
    alarm(10);vmctx_ctl_nr=999;
    uint64_t mm=UINT64_C(0x100000007);source_id ids[2];
    for(unsigned i=0;i<2;i++) {
        model[i].fd=open("/dev/null",O_RDONLY|O_CLOEXEC);assert(model[i].fd>=0);
        model[i].info=(struct vmctx_context){.version=VMCTX_CONTEXT_ABI,.size=sizeof(struct vmctx_context),
            .op=VMCTX_CONTEXT_INFO,.identity=90+i,.mm_identity=mm,.flags=VMCTX_CONTEXT_READY,.fd=-1};
        memset(model[i].bytes,0x51+i,4096);
        struct source_context native={.fd=model[i].fd,.identity=model[i].info.identity};
        ids[i]=source_record_add(&native);assert(ids[i]==(source_id)i+1);
    }
    struct vmr_mm_binding old=source_binding_required(ids[0]);
    model[0].info.mm_identity+=UINT64_C(1)<<32;
    struct vmr_mm_binding current=source_binding_required(ids[0]);assert(current.mm!=old.mm);
    unsigned char page[4096];memset(page,0xa7,sizeof(page));
    assert(ctx_land(&old,0x4000,page,5,1)==4096 && selected==1);
    assert(model[0].bytes[0]==0x51 && model[1].bytes[0]==0xa7);
    memset(page,0xcc,sizeof(page));assert(ctx_peek(&old,0x4000,page,4096)==4096 && selected==1 && page[0]==0xa7);
    struct source_page_receipt first,second;
    struct source_custody custody={0};
    if(argc>1 && !strcmp(argv[1],"bulk")) {
        unsigned before=calls;
        char wide[8192];
        long result=ctx_page(&custody,&old,0x4000,sizeof(wide),wide,sizeof(wide),0,&first);
        if(result!=VMR_CTXPAGE_FAILED || calls!=before) {
            fprintf(stderr,"FAIL: multi-page request entered native memory: result=%ld calls=%u\n",
                result,calls-before);
            return 1;
        }
        puts("PASS: a multi-page request is refused before any source capture or ownership mutation");
        return 0;
    }
    unsigned invalid_before=calls;
    assert(ctx_page(&custody,&old,0x4001,4096,(char *)page,4096,0,&second)==VMR_CTXPAGE_FAILED && calls==invalid_before);
    unsigned before_calls=calls,before_commits=commits;move_at_entry=1;
    assert(ctx_land(&old,0x4000,page,5,1)==-ESTALE && calls==before_calls+1 && commits==before_commits);
    model[1].info.mm_identity=old.mm;fail_after_commit=1;before_calls=calls;
    assert(ctx_land(&old,0x4000,page,5,1)==-EFAULT && calls==before_calls+1 && commits==before_commits+1);
    fail_after_commit=0;probe_error=1;
    assert(pg_state(&old,0x4000)==-1); /* UNKNOWN is not a fresh page. */
    assert(ctx_write_prep(&old,0x4000)==VMR_CTXPAGE_FAILED); /* A failed protection probe grants nothing. */
    probe_error=0;
    move_at_entry=1;
    assert(ctx_page(&custody,&old,0x4000,4096,(char *)page,4096,0,&second)==VMR_CTXPAGE_FAILED);
    model[1].info.mm_identity=old.mm;
    fork_rel=malloc(sizeof(*fork_rel));assert(fork_rel);
    fork_rel[0]=(struct source_lineage){.child=old.mm+1,.parent=old.mm};
    nfork_rel=fork_rel_capacity=1;
    if(argc<2 || strcmp(argv[1],"retired-lineage")) {
        assert(ctx_write_prep(&old,0x4000)==VMR_CTXPAGE_COWBREAK);
        assert(!cow_mm_is_broken(old.mm+1,0x4000));
        assert(ctx_write_prep(&old,0x4000)==VMR_CTXPAGE_COWBREAK);
    }
    if(argc<2 || strcmp(argv[1],"obligation")) {
        mm_ended=1;source_mm_id children[2]={0};
        assert(fork_rel_children(old.mm,children,2)==1 && children[0]==old.mm+1);
    }
    struct vmr_mm_binding forged=old;forged.epoch++;before_calls=calls;
    assert(ctx_land(&forged,0x4000,page,5,1)==-ESTALE && calls==before_calls);
    for(unsigned i=0;i<2;i++) {model[i].info.flags=VMCTX_CONTEXT_ENDED;source_record_release(ids[i]);}
    puts("PASS: production source LAND/PEEK retain old MM across exec, reject raced entry, never replay uncertain commits, and preserve unknown protection/ownership");
    puts("PASS: WRITEPREP retains an uncommitted child-copy obligation and ended intermediate MMs retain their lineage edges");
    return 0;
}
