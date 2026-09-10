// SPDX-License-Identifier: GPL-2.0
/* Native TAKE must privately detach file-cache pages before destructive
 * removal. An unrelated mapper can arrive after the mapcount precheck. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>
static long run_nr, ctl_nr;
static pid_t child;
static int filefd;
static unsigned stop, toggles;
static unsigned char bytes[4096];
static unsigned char *result;
static void *aliases(void *unused)
{
    (void)unused;
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
        int flags = unused ? MAP_SHARED : MAP_PRIVATE;
        volatile unsigned char *p = mmap(NULL, 4096, PROT_READ, flags, filefd, 0);
        if (p == MAP_FAILED) abort();
        if (*p != 0x69) abort();
        __atomic_add_fetch(&toggles, 1, __ATOMIC_RELAXED);
        if (munmap((void *)p, 4096)) abort();
    }
    return NULL;
}
static long ctl(unsigned op, void *arg) { return syscall(ctl_nr, child, op, arg); }
static int source_present(uint64_t addr)
{
    char path[80];uint64_t pte=0;
    snprintf(path,sizeof(path),"/proc/%d/pagemap",child);
    int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0)return -1;
    ssize_t n=pread(fd,&pte,sizeof(pte),(off_t)(addr/4096*8));close(fd);
    return n==sizeof(pte) ? !!(pte&(UINT64_C(1)<<63)):-1;
}
static void deadline(int sig) { (void)sig; if (child > 0) kill(child, SIGKILL); }
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s errno=%d rounds=%u\n",__LINE__,#x,errno,rounds); goto done; } } while(0)
int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    run_nr = strtol(argv[1],NULL,10); ctl_nr = strtol(argv[2],NULL,10);
    unsigned rounds = 0, transferred = 0, busy = 0;
    int pass = 0, threaded = 0; pthread_t thread[2];
    filefd = memfd_create("take-file-race", 0);
    memset(bytes, 0x69, sizeof(bytes));
    CHECK(filefd >= 0 && write(filefd, bytes, sizeof(bytes)) == sizeof(bytes));
    result = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    CHECK(result != MAP_FAILED);
    struct vmctx_run_config cfg = {.flags=VMCTX_FLAG_SERVICE|VMCTX_FLAG_WAIT_MONITOR,.backing_fd=-1,.shared_fd=-1};
    pid_t parent = getpid(); child = fork(); CHECK(child >= 0);
    if (!child) {
        if (prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent) _exit(125);
        syscall(run_nr,&cfg); _exit(126);
    }
    struct sigaction sa = {.sa_handler=deadline}; CHECK(!sigaction(SIGALRM,&sa,NULL)); alarm(30);
    for (unsigned i=0; ctl(VMCTX_CTL_ATTACH,NULL); i++) { CHECK(i<2000); usleep(1000); }
    struct vmctx_cpu_model model;
    CHECK(!syscall(ctl_nr,0,VMCTX_CTL_CPU_CAPS,&model) && !ctl(VMCTX_CTL_CPU_MODEL,&model));
    for (unsigned i=0; i<2; i++) {
        CHECK(!pthread_create(&thread[i],NULL,aliases,(void *)(uintptr_t)i)); threaded++;
    }
    for (rounds=0; rounds<1000; rounds++) {
        struct vmctx_syscall call = {.nr=SYS_mmap,.args={0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE,filefd,0}};
        CHECK(!ctl(VMCTX_CTL_SYSCALL,&call) && call.ret>0); uint64_t addr=call.ret;
        struct vmctx_mem m = {.addr=addr,.buf=(uintptr_t)bytes,.len=4096};
        CHECK(ctl(VMCTX_CTL_PEEK,&m)==4096);
        /* Force copyout to fault in a fresh monitor page every round. */
        CHECK(!madvise(result,4096,MADV_DONTNEED)); m.buf=(uintptr_t)result;
        long r = ctl(VMCTX_CTL_TAKE,&m);
        if (r<0 && errno==EBUSY) { busy++; CHECK(ctl(VMCTX_CTL_PEEK,&m)==4096); }
        else { CHECK(r==4096); transferred++; }
        for (unsigned j=0;j<sizeof(bytes);j++) CHECK(result[j]==0x69);
        call=(struct vmctx_syscall){.nr=SYS_munmap,.args={addr,4096}};
        CHECK(!ctl(VMCTX_CTL_SYSCALL,&call) && !call.ret);
        /* Private read-only mappings now transfer through native COW.
         * Shared mappings still refuse private TAKE without losing bytes. */
        if (!(rounds % 10)) for (unsigned shared=0; shared<2; shared++) {
            call=(struct vmctx_syscall){.nr=SYS_mmap,
                .args={0,4096,PROT_READ,shared?MAP_SHARED:MAP_PRIVATE,filefd,0}};
            CHECK(!ctl(VMCTX_CTL_SYSCALL,&call) && call.ret>0); addr=call.ret;
            m=(struct vmctx_mem){.addr=addr,.buf=(uintptr_t)result,.len=4096};
            CHECK(ctl(VMCTX_CTL_PEEK,&m)==4096);
            CHECK(source_present(addr)==1);
            long taken=ctl(VMCTX_CTL_TAKE,&m);
            if(shared) {
                CHECK(taken==-1 && errno==EBUSY && source_present(addr)==1);
                memset(result,0xa5,4096); CHECK(ctl(VMCTX_CTL_PEEK,&m)==4096);
            } else {
                CHECK(taken==4096 && source_present(addr)==0);
                unsigned char original[4096];
                CHECK(pread(filefd,original,4096,0)==4096);
                for(unsigned j=0;j<4096;j++)CHECK(original[j]==0x69);
            }
            for (unsigned j=0;j<4096;j++) CHECK(result[j]==0x69);
            call=(struct vmctx_syscall){.nr=SYS_munmap,.args={addr,4096}};
            CHECK(!ctl(VMCTX_CTL_SYSCALL,&call) && !call.ret);
        }
    }
    CHECK(transferred && __atomic_load_n(&toggles,__ATOMIC_RELAXED)>100);
    printf("PASS: private file TAKE survives competing mappings (%u transfers, %u intact refusals, %u aliases)\n",
           transferred,busy,__atomic_load_n(&toggles,__ATOMIC_RELAXED)); pass=1;
done:
    __atomic_store_n(&stop,1,__ATOMIC_RELEASE);
    for (int i=0;i<threaded;i++) pthread_join(thread[i],NULL);
    if (child>0) {int status;kill(child,SIGKILL);while(waitpid(child,&status,0)<0&&errno==EINTR){}}
    alarm(0);if(result && result!=MAP_FAILED)munmap(result,4096);
    if(filefd>=0)close(filefd);
    return pass?0:1;
}
