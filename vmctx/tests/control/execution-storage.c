/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <errno.h>
static int punch_error, punch_interrupt;
static int storage_fallocate(int fd,int mode,off_t offset,off_t length)
{
    if(punch_interrupt) {punch_interrupt=0;errno=EINTR;return -1;}
    if(punch_error) {errno=punch_error;return -1;}
    return fallocate(fd,mode,offset,length);
}
#define fallocate storage_fallocate
#define main vmremote_program_main
#ifndef VMREMOTE_SOURCE
#define VMREMOTE_SOURCE "../../user/vmremote.c"
#endif
#include VMREMOTE_SOURCE
#undef main
#undef fallocate
#define TEST_MM_REGISTRY executor_mms
#include "execution-fixture.h"

static int discard_failure(void)
{
    memory_target target=test_target(7);
    unsigned char bytes[4096];memset(bytes,0x6b,sizeof(bytes));
    assert(obj_write(target,0x4000,bytes,sizeof(bytes))==sizeof(bytes));
    const int failures[]={ENOSYS,EIO,EBADF};
    for(unsigned i=0;i<sizeof(failures)/sizeof(*failures);i++) {
        pid_t child=fork();assert(child>=0);
        if(!child) {
            struct rlimit zero={0,0};assert(!setrlimit(RLIMIT_CORE,&zero));
            punch_error=failures[i];obj_forget(target,0x4000,4096);_exit(0);
        }
        int status;assert(waitpid(child,&status,0)==child);
        if(!WIFSIGNALED(status) || WTERMSIG(status)!=SIGABRT) {
            fprintf(stderr,"FAIL: failed discard returned to mapping publication errno=%d status=%d\n",failures[i],status);
            return 1;
        }
    }
    punch_interrupt=1;obj_forget(target,0x4000,4096);
    assert(!punch_interrupt);
    assert(pread(backing_find(target),bytes,sizeof(bytes),0x4000)==sizeof(bytes));
    for(unsigned i=0;i<sizeof(bytes);i++)assert(!bytes[i]);
    puts("PASS: failed hole punching stops publication; an interrupted successful discard removes every stale byte");
    return 0;
}

static int short_preview(void)
{
    memory_target target=test_target(7);
    unsigned char *pages=mmap(NULL,8192,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(pages!=MAP_FAILED && !mprotect(pages+4096,4096,PROT_NONE));
    pages[4095]=0xa7;traced_page=0x4000;
    assert(obj_write(target,0x4000,pages+4095,1)==1);
    assert(poke_as(target,0x4001,pages+4095,1)==1);
    unsigned char result[2];
    assert(pread(backing_find(target),result,sizeof(result),0x4000)==sizeof(result));
    assert(result[0]==0xa7 && result[1]==0xa7);
    assert(!munmap(pages,8192));
    puts("PASS: both object write diagnostics respect a one-byte buffer adjacent to inaccessible memory");
    return 0;
}

static int detached_region(void)
{
    memory_target first=test_target(7),other=test_target(UINT64_C(0x100000007));
    region_add(as_id(first),0x1000,0x9000,PROT_READ);
    region_add(as_id(other),0x1000,0x9000,PROT_WRITE);
#ifdef STORAGE_BASELINE
    const struct region *saved=region_of(first,0x2000);
#else
    struct region copy;
    assert(region_snapshot(first,0x2000,&copy));
    const struct region *saved=&copy;
    int count;struct region *all=region_snapshot_all(first,&count);
    assert(count==1 && all[0].as==as_id(first));
#endif
    assert(saved);
    region_forget_range(first,0x3000,0x5000);
    if(saved->start!=0x1000 || saved->end!=0x9000 || saved->prot!=PROT_READ) {
        fputs("FAIL: a retained region changed during a callback\n",stderr);return 1;
    }
#ifndef STORAGE_BASELINE
    assert(all[0].start==0x1000 && all[0].end==0x9000);free(all);
    assert(!region_snapshot(first,0x4000,&copy));
    assert(region_snapshot(other,0x4000,&copy) && copy.prot==PROT_WRITE);
    assert(region_snapshot(first,0x6000,&copy) && copy.start==0x5000 && copy.end==0x9000);
#endif
    puts("PASS: copied layout survives mutation, preserves split tails and isolates MMs sharing low identity bits");
    return 0;
}

static int replace_object(void *opaque,int fd,uint64_t expected,uint64_t *next)
{
    int *storage=opaque;*storage=fd;*next=expected+1;return 0;
}
static int retained_landing(void)
{
    memory_target old=test_target(7);
    const uint64_t next_mm=UINT64_C(0x100000007),page=0x4000;
    struct vmr_mm_binding source=old->source;source.mm=next_mm;source.epoch++;
    struct execution_mm *mm=source_mm_resolve(&source);assert(mm);
    int native=backing_find(old);
    memory_target next=execution_target_publish(old->context,mm,&source,replace_object,&native);
    assert(next && native==backing_find(next) && next->view.epoch==old->view.epoch+1);
    unsigned char before[4096],after[4096];memset(before,0x6b,sizeof(before));
    assert(obj_write(old,page,before,sizeof(before))==sizeof(before));
    assert(pread(backing_find(next),after,sizeof(after),page)==sizeof(after));
    for(unsigned i=0;i<sizeof(after);i++)assert(!after[i]);
    assert(pread(backing_find(old),after,sizeof(after),page)==sizeof(after) && !memcmp(before,after,sizeof(after)));
    page_installed(old,page);page_installed(next,page);mark_handed_over(old,page);
    assert(!page_is_installed(old,page) && page_is_installed(next,page));
    page_lend(old,page,41);page_lend(next,page,42);
    assert(page_holder(old,page)==41 && page_holder(next,page)==42);
    obj_forget(old,page,4096);
    assert(page_holder(next,page)==42 && page_is_installed(next,page));
    puts("PASS: a landing retained across exec writes only the old MM; old retirement preserves the new binding's installation and loan");
    return 0;
}
int main(int argc,char **argv)
{
    alarm(15);coh_lock_init();
    if(argc!=2)return 2;
    if(!strcmp(argv[1],"discard"))return discard_failure();
    if(!strcmp(argv[1],"preview"))return short_preview();
    if(!strcmp(argv[1],"region"))return detached_region();
    if(!strcmp(argv[1],"landing"))return retained_landing();
    return 2;
}
