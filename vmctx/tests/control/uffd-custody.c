/* SPDX-License-Identifier: GPL-2.0 */
/* A remotely owned page is logically present even though TAKE removed its
 * source PTE. UFFD must not treat that ownership marker as an empty slot. */
#define _GNU_SOURCE
#define main vmhome_program_main
#include "../../user/vmhome.c"
#undef main
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>

static pid_t uffd_child;
static volatile sig_atomic_t uffd_expired;
static void uffd_deadline(int sig)
{ (void)sig; uffd_expired=1; if(uffd_child>0)kill(uffd_child,SIGKILL); }
static int uffd_present(pid_t pid,uint64_t address)
{
    char name[80];uint64_t pte=0;
    snprintf(name,sizeof(name),"/proc/%d/pagemap",pid);
    int fd=open(name,O_RDONLY|O_CLOEXEC);if(fd<0)return -1;
    ssize_t n=pread(fd,&pte,sizeof(pte),(off_t)(address/4096*8));close(fd);
    return n==sizeof(pte) ? !!(pte&(UINT64_C(1)<<63)):-1;
}
static int uffd_send(int sock,int fd)
{
    char byte='f',control[CMSG_SPACE(sizeof(fd))]={0};
    struct iovec iov={&byte,1};
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,
        .msg_control=control,.msg_controllen=sizeof(control)};
    struct cmsghdr *c=CMSG_FIRSTHDR(&msg);
    c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(fd));
    memcpy(CMSG_DATA(c),&fd,sizeof(fd));
    return sendmsg(sock,&msg,MSG_NOSIGNAL)==1 ? 0:-1;
}
static int uffd_receive(int sock)
{
    int fd=-1;char byte,control[CMSG_SPACE(sizeof(fd))]={0};
    struct iovec iov={&byte,1};
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,
        .msg_control=control,.msg_controllen=sizeof(control)};
    if(recvmsg(sock,&msg,MSG_CMSG_CLOEXEC)!=1 || msg.msg_flags&(MSG_CTRUNC|MSG_TRUNC))return -1;
    struct cmsghdr *c=CMSG_FIRSTHDR(&msg);
    if(!c || c->cmsg_level!=SOL_SOCKET || c->cmsg_type!=SCM_RIGHTS ||
       c->cmsg_len!=CMSG_LEN(sizeof(fd)))return -1;
    memcpy(&fd,CMSG_DATA(c),sizeof(fd));return fd;
}
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno);goto done;}}while(0)
int main(int argc,char **argv)
{
    if(argc!=4)return 2;
    vmctx_ctl_nr=strtol(argv[2],NULL,10);
    int zero=!strcmp(argv[3],"zero"),pass=0,status,uffd=-1,sock[2]={-1,-1};
    void *page=MAP_FAILED;pid_t parent=getpid();source_id id=0;
    unsigned char bytes[4096],replacement[4096];
    struct source_custody custody={0};struct source_page_receipt receipt={0};
    struct sigaction sa={.sa_handler=uffd_deadline};
    CHECK(zero || !strcmp(argv[3],"copy"));CHECK(!sigaction(SIGALRM,&sa,NULL));
    CHECK(!source_memory_capabilities() && !source_transfer_caps());
    CHECK(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,sock));
    page=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(page!=MAP_FAILED);memset(page,0x73,4096);memset(replacement,0xb6,sizeof(replacement));
    alarm(3);uffd_child=fork();CHECK(uffd_child>=0);
    if(!uffd_child) {
        close(sock[0]);
        if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent)_exit(125);
        int fd=syscall(SYS_userfaultfd,O_CLOEXEC|O_NONBLOCK|UFFD_USER_MODE_ONLY);
        struct uffdio_api api={.api=UFFD_API};
        struct uffdio_register reg={.range={(uintptr_t)page,4096},.mode=UFFDIO_REGISTER_MODE_MISSING};
        if(fd<0 || ioctl(fd,UFFDIO_API,&api) || ioctl(fd,UFFDIO_REGISTER,&reg) ||
           uffd_send(sock[1],fd) || ptrace(PTRACE_TRACEME,0,NULL,NULL))_exit(125);
        close(fd);close(sock[1]);raise(SIGSTOP);_exit(126);
    }
    close(sock[1]);sock[1]=-1;uffd=uffd_receive(sock[0]);CHECK(uffd>=0);
    CHECK(waitpid(uffd_child,&status,0)==uffd_child && WIFSTOPPED(status));
    CHECK(!source_control_raw(uffd_child,VMCTX_CTL_ADOPT,NULL));
    CHECK(!ptrace(PTRACE_DETACH,uffd_child,NULL,NULL));
    struct source_context handle={.fd=-1};CHECK(!source_context_open(uffd_child,&handle));
    id=source_record_add(&handle);CHECK(id>0);
    struct vmr_mm_binding target=source_binding_required(id);
    CHECK(ctx_page(&custody,&target,(uintptr_t)page,4096,(char *)bytes,sizeof(bytes),0,&receipt)==4096);
    CHECK(pg_state(&target,(uintptr_t)page)==PG_INTRANSIT && !uffd_present(uffd_child,(uintptr_t)page));
    CHECK(!ctx_page_ack(&custody,&target,(uintptr_t)page,receipt.episode));
    CHECK(pg_state(&target,(uintptr_t)page)==PG_THEIRS);
    /* No guest access or page fault is involved: the ioctl directly installs
     * a PTE in an mm whose sole authoritative bytes are held by this test. */
    struct uffdio_copy copy={.dst=(uintptr_t)page,.src=(uintptr_t)replacement,.len=4096};
    struct uffdio_zeropage zp={.range={(uintptr_t)page,4096}};
    errno=0;int rc=ioctl(uffd,zero?UFFDIO_ZEROPAGE:UFFDIO_COPY,zero?(void *)&zp:(void *)&copy);
    int error=errno,present=uffd_present(uffd_child,(uintptr_t)page),state=pg_state(&target,(uintptr_t)page);
    printf("OBSERVED: UFFD %s rc=%d errno=%d bytes=%lld record=%d source_pte_present=%d\n",
           argv[3],rc,error,(long long)(zero?zp.zeropage:copy.copy),state,present);
    CHECK(rc==-1 && error==EEXIST && (zero?zp.zeropage:copy.copy)==-EEXIST);
    CHECK(state==PG_THEIRS && !present);
    pass=1;
done:
    if(custody.count && receipt.episode)
        if(ctx_page_ack(&custody,&receipt.target,receipt.base,receipt.episode))pass=0;
    if(custody.owned && source_custody_abandon(&custody))pass=0;
    if(uffd_child>0) {
        kill(uffd_child,SIGKILL);pid_t reaped;
        do {reaped=waitpid(uffd_child,&status,0);}while(reaped<0 && errno==EINTR);
        if(reaped!=uffd_child || !WIFSIGNALED(status) || WTERMSIG(status)!=SIGKILL)pass=0;
    }
    if(id)source_record_release(id);
    if(uffd>=0)close(uffd);
    for(unsigned i=0;i<2;i++)if(sock[i]>=0)close(sock[i]);
    if(page!=MAP_FAILED)munmap(page,4096);
    alarm(0);if(uffd_expired)pass=0;
    printf("%s: UFFD %s respects native remote custody\n",pass?"PASS":"FAIL",argv[3]);
    return pass?0:1;
}
