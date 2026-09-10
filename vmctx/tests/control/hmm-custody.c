/* SPDX-License-Identifier: GPL-2.0 */
/* Native substrate experiment, using the unmodified Linux test_hmm driver.
 * These checks validate HMM residency, not vmctx/network integration. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <lib/test_hmm_uapi.h>
#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif
#define P 4096UL
#define HUGE (2UL * 1024 * 1024)
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s errno=%d\n", \
    __LINE__, #x, errno); exit(1); } } while (0)

static void pattern(unsigned char *p, unsigned salt)
{
    for (unsigned i=0; i<P; i++) p[i]=(unsigned char)(salt+i*37+(i>>3));
}

static unsigned snapshot(int fd, void *address)
{
    unsigned char state=0xff;
    struct hmm_dmirror_cmd cmd={.addr=(uintptr_t)address,.ptr=(uintptr_t)&state,.npages=1};
    CHECK(!ioctl(fd,HMM_DMIRROR_SNAPSHOT,&cmd) && cmd.cpages==1);
    return state;
}

static void transfer(int fd, void *address, unsigned char *copy)
{
    struct hmm_dmirror_cmd cmd={.addr=(uintptr_t)address,.ptr=(uintptr_t)copy,.npages=1};
    CHECK(!ioctl(fd,HMM_DMIRROR_MIGRATE_TO_DEV,&cmd) && cmd.cpages==1);
    CHECK((snapshot(fd,address)&0x30)==HMM_DMIRROR_PROT_DEV_PRIVATE_LOCAL);
}

static void device_write(int fd, void *address, const unsigned char *data)
{
    struct hmm_dmirror_cmd cmd={.addr=(uintptr_t)address,.ptr=(uintptr_t)data,.npages=1};
    CHECK(!ioctl(fd,HMM_DMIRROR_WRITE,&cmd) && cmd.cpages==1);
}

int main(int argc,char **argv)
{
    CHECK(argc==2 && sysconf(_SC_PAGESIZE)==P);
    const char *mode=argv[1];
    alarm(20);
    int fd=open("/dev/hmm_dmirror0",O_RDWR|O_CLOEXEC); CHECK(fd>=0);
    int collapse=!strcmp(mode,"collapse");
    size_t size=collapse ? HUGE : P;
    unsigned char *raw=mmap(NULL,size+HUGE,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS,-1,0); CHECK(raw!=MAP_FAILED);
    uintptr_t aligned=((uintptr_t)raw+HUGE-1)&~(HUGE-1);
    unsigned char *area=(void *)aligned, *page=area+(collapse ? 17*P : 0);
    CHECK(!madvise(area,size,MADV_NOHUGEPAGE));
    memset(area,0x2a,size);
    unsigned char original[P],changed[P],copy[P];
    pattern(original,0x13); pattern(changed,0xb6); memcpy(page,original,P);
    int uffd=-1;
    if(!strncmp(mode,"uffd-",5)) {
        uffd=syscall(SYS_userfaultfd,O_CLOEXEC|O_NONBLOCK); CHECK(uffd>=0);
        struct uffdio_api api={.api=UFFD_API}; CHECK(!ioctl(uffd,UFFDIO_API,&api));
        struct uffdio_register reg={.range={.start=(uintptr_t)page,.len=P},
            .mode=UFFDIO_REGISTER_MODE_MISSING};
        CHECK(!ioctl(uffd,UFFDIO_REGISTER,&reg));
    }
    transfer(fd,page,copy); CHECK(!memcmp(copy,original,P));
    if(!strcmp(mode,"fork")) {
        int ready[2],go[2]; CHECK(!pipe(ready) && !pipe(go));
        pid_t parent=getpid(),child=fork(); CHECK(child>=0);
        if(!child) {
            CHECK(!prctl(PR_SET_PDEATHSIG,SIGKILL) && getppid()==parent);
            close(ready[0]);close(go[1]);
            CHECK(write(ready[1],"r",1)==1 && read(go[0],copy,1)==1);
            CHECK(!memcmp(page,original,P)); _exit(0);
        }
        close(ready[1]);close(go[0]);
        CHECK(read(ready[0],copy,1)==1);
        device_write(fd,page,changed);
        CHECK(write(go[1],"g",1)==1);
        int status; CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
        close(ready[0]);close(go[1]); CHECK(!memcmp(page,changed,P));
    } else if(!strcmp(mode,"remap") || !strcmp(mode,"dontunmap")) {
        device_write(fd,page,changed);
        void *dest=mmap(NULL,P,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); CHECK(dest!=MAP_FAILED);
        int keep=!strcmp(mode,"dontunmap");
        CHECK(mremap(page,P,P,MREMAP_MAYMOVE|MREMAP_FIXED|(keep?MREMAP_DONTUNMAP:0),dest)==dest);
        CHECK((snapshot(fd,dest)&0x30)==HMM_DMIRROR_PROT_DEV_PRIVATE_LOCAL);
        CHECK(!memcmp(dest,changed,P));
        if(keep) { memset(copy,0,P); CHECK(!memcmp(page,copy,P)); }
        CHECK(!munmap(dest,P));
    } else if(uffd>=0) {
        int rc;
        if(!strcmp(mode,"uffd-copy")) {
            struct uffdio_copy op={.src=(uintptr_t)changed,.dst=(uintptr_t)page,.len=P};
            rc=ioctl(uffd,UFFDIO_COPY,&op);
        } else {
            CHECK(!strcmp(mode,"uffd-zero"));
            struct uffdio_zeropage op={.range={.start=(uintptr_t)page,.len=P}};
            rc=ioctl(uffd,UFFDIO_ZEROPAGE,&op);
        }
        CHECK(rc==-1 && errno==EEXIST);
        CHECK((snapshot(fd,page)&0x30)==HMM_DMIRROR_PROT_DEV_PRIVATE_LOCAL);
        CHECK(!memcmp(page,original,P)); close(uffd);
    } else if(collapse) {
        device_write(fd,page,changed); CHECK(!madvise(area,size,MADV_HUGEPAGE));
        int rc=madvise(area,size,MADV_COLLAPSE),error=errno;
        printf("OBSERVED: collapse=%d errno=%d residency=0x%x\n",rc,rc ? error:0,snapshot(fd,page));
        CHECK(!rc || error==EAGAIN || error==EBUSY || error==ENOMEM);
        CHECK(!memcmp(page,changed,P));
        for(size_t i=0;i<size;i++)
            if(i<17*P || i>=18*P) CHECK(area[i]==0x2a);
    } else if(!strcmp(mode,"discard") || !strcmp(mode,"replace")) {
        if(!strcmp(mode,"discard")) CHECK(!madvise(page,P,MADV_DONTNEED));
        else CHECK(mmap(page,P,PROT_READ|PROT_WRITE,MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==page);
        CHECK(snapshot(fd,page)==HMM_DMIRROR_PROT_NONE);
        memset(copy,0,P); CHECK(!memcmp(page,copy,P));
    } else if(!strcmp(mode,"protect")) {
        CHECK(!mprotect(page,P,PROT_READ));
        unsigned state=snapshot(fd,page);
        CHECK((state&0x30)==HMM_DMIRROR_PROT_DEV_PRIVATE_LOCAL && !(state&HMM_DMIRROR_PROT_WRITE));
        CHECK(!mprotect(page,P,PROT_READ|PROT_WRITE));
        device_write(fd,page,changed); CHECK(!memcmp(page,changed,P));
    } else if(!strcmp(mode,"exit")) {
        /* Exit with the residency entry still live: native exit_mmap owns
         * the mapping release. Driver close separately evicts its pool. */
        printf("PASS: HMM substrate exit with live private residency\n");
        return 0;
    } else {
        CHECK(!strcmp(mode,"recall")); device_write(fd,page,changed);
        int pipefd[2]; CHECK(!pipe(pipefd));
        CHECK(write(pipefd[1],page,P)==P && read(pipefd[0],copy,P)==P);
        CHECK(!memcmp(copy,changed,P)); close(pipefd[0]);close(pipefd[1]);
    }
    CHECK(!munmap(raw,size+HUGE)); close(fd); alarm(0);
    printf("PASS: HMM substrate %s retains the page through native MM operations\n",mode);
    return 0;
}
