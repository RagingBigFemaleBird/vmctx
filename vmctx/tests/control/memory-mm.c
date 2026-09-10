/* SPDX-License-Identifier: GPL-2.0 */
/* Stall the inner argument copy, after the envelope has captured its MM.
 * Exec proceeds while that copy waits on userfaultfd. No timing guess decides
 * which address space the resumed operation must access. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <linux/userfaultfd.h>
#include <linux/vmctx.h>
#include "../../kernel/vmctx_memory.h"
#include "../../kernel/vmctx_recall.h"
#include "../../kernel/vmctx_mapping.h"
#include "../../kernel/vmctx_access.h"
static long run_nr,ctl_nr;
static volatile sig_atomic_t child;
static long source_control_raw(pid_t target,unsigned op,void *arg)
{return syscall(ctl_nr,target,op,arg);}
#include "../../user/source-memory-target.h"

#define ADDRESS UINT64_C(0x3400000000)
#define VALUE UINT64_C(0x123456789abcdef0)
static char executable[]="/bin/true";
static char *exec_argv[]={executable,NULL},*exec_env[]={NULL};
struct pending {int selector,legacy,error;source_id source;struct source_memory_target target;uint64_t mm,value;void *argument;long result;};
static void *read_memory(void *opaque)
{
	struct pending *request=opaque;
	request->result=request->legacy ? source_control_raw(request->selector,VMCTX_CTL_PEEK,request->argument) :
		source_memory_target_call(&request->target,VMCTX_CTL_PEEK,request->argument);
	request->error=errno;return NULL;
}
static void deadline(int signal_number)
{(void)signal_number;if(child>0)kill(child,SIGKILL);_exit(124);}
static int gate_step(const struct source_context *context,struct vmctx_syscall_gate *gate,unsigned op)
{gate->op=op;return source_control_raw(source_context_selector(context),VMCTX_CTL_SYSCALL_GATE,gate);}
static int gate_wait(const struct source_context *context,struct vmctx_syscall_gate *gate,unsigned state)
{
	for(unsigned i=0;i<3000;i++) {
		int result=gate_step(context,gate,VMCTX_GATE_QUERY);
		if(!result && gate->state==state)return 0;
		if(result && errno!=EAGAIN && errno!=EBUSY)return -1;
		usleep(1000);
	}
	errno=ETIMEDOUT;return -1;
}
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"FAIL memory-mm line %d: %s errno=%d\n",__LINE__,#x,errno);goto done;}}while(0)
int main(int argc,char **argv)
{
	if(argc!=4 || (strcmp(argv[3],"legacy") && strcmp(argv[3],"fenced")))return 2;
	run_nr=strtol(argv[1],NULL,10);ctl_nr=strtol(argv[2],NULL,10);
	int legacy=!strcmp(argv[3],"legacy"),pass=0,uffd=-1,started=0,registered=0;
	void *argument=MAP_FAILED,*bytes=MAP_FAILED,*page=MAP_FAILED;
	struct source_context context={.fd=-1};
	source_id registered_source=0;
	struct vmctx_context info;
	pthread_t thread;
	struct pending request={.legacy=legacy};
	signal(SIGALRM,deadline);alarm(15);
	if(!legacy) {
		CHECK(!source_memory_capabilities());
		struct source_context invalid={.fd=INT_MAX,.identity=1};
		CHECK(source_context_selector(&invalid)==INT_MIN);
		CHECK(source_record_memory(0,1,VMCTX_CTL_PEEK,(void *)1)==-1 && errno==EINVAL);
	}
	page=mmap((void *)ADDRESS,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
	CHECK(page==(void *)ADDRESS);*(uint64_t *)page=VALUE;
	pid_t monitor=getpid();child=fork();CHECK(child>=0);
	if(!child) {
		if(prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=monitor)_exit(125);
		struct vmctx_run_config cfg={.flags=VMCTX_FLAG_SERVICE|VMCTX_FLAG_WAIT_MONITOR,.backing_fd=-1,.shared_fd=-1};
		syscall(run_nr,&cfg);_exit(126);
	}
	for(unsigned i=0;source_control_raw(child,VMCTX_CTL_ATTACH,NULL);i++) {CHECK(i<2000);usleep(1000);}
	CHECK(!source_context_open(child,&context));CHECK(!source_context_info(&context,&info));
	request.mm=info.mm_identity;request.selector=source_context_selector(&context);CHECK(request.mm);
	struct source_context owned={.fd=-1};
	CHECK(!source_context_open(child,&owned));
	registered_source=source_record_add(&owned);CHECK(registered_source);
	request.source=registered_source;
	CHECK(!source_memory_target_capture(request.source,&request.target) && request.target.mm==request.mm);
	struct vmctx_cpu_model model;
	CHECK(!source_control_raw(0,VMCTX_CTL_CPU_CAPS,&model));
	CHECK(!source_control_raw(request.selector,VMCTX_CTL_CPU_MODEL,&model));
	struct vmctx_cpu_state cpu;
	for(unsigned i=0;source_control_raw(request.selector,VMCTX_CTL_GETCPU,&cpu);i++) {CHECK(i<2000 && (errno==EAGAIN || errno==EBUSY));usleep(1000);}
	if(!legacy) {
		uint64_t original=0;
		struct vmctx_mem read={.addr=ADDRESS,.len=8,.buf=(uintptr_t)&original};
		CHECK(source_record_memory(request.source,request.mm,VMCTX_CTL_PEEK,&read)==8 && original==VALUE);
		unsigned commands[]={VMCTX_CTL_PEEK,VMCTX_CTL_POKE,VMCTX_CTL_TAKE,VMCTX_CTL_SERVE,
			VMCTX_CTL_PGSTATE,VMCTX_CTL_LAND,VMCTX_CTL_PGACK,VMCTX_CTL_PGSCAN,VMCTX_CTL_PGSET,
			VMCTX_CTL_PROTECT,VMCTX_CTL_TAKEOBJ,VMCTX_CTL_RECALL,VMCTX_CTL_MAPPING,
			VMCTX_CTL_ACCESS_LOG,VMCTX_CTL_MMLOG,VMCTX_CTL_TRYFAULT,VMCTX_CTL_FUTEXWAKE,VMCTX_CTL_BREAKCOW};
		for(unsigned i=0;i<sizeof(commands)/sizeof(*commands);i++)
			CHECK(source_record_memory(request.source,request.mm^UINT64_C(0x8000000000000000),commands[i],(void *)1)==-1 && errno==ESTALE);
		CHECK(source_record_memory(request.source,request.mm,VMCTX_CTL_SETCPU,(void *)1)==-1 && errno==EINVAL);
	}
	uffd=syscall(SYS_userfaultfd,O_CLOEXEC|O_NONBLOCK);CHECK(uffd>=0);
	struct uffdio_api api={.api=UFFD_API};CHECK(!ioctl(uffd,UFFDIO_API,&api));
	argument=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
	bytes=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
	CHECK(argument!=MAP_FAILED && bytes!=MAP_FAILED);
	struct uffdio_register registration={.range={.start=(uintptr_t)argument,.len=4096},.mode=UFFDIO_REGISTER_MODE_MISSING};
	CHECK(!ioctl(uffd,UFFDIO_REGISTER,&registration));registered=1;
	*(struct vmctx_mem *)bytes=(struct vmctx_mem){.addr=ADDRESS,.len=8,.buf=(uintptr_t)&request.value};
	request.argument=argument;CHECK(!pthread_create(&thread,NULL,read_memory,&request));started=1;
	struct pollfd event={.fd=uffd,.events=POLLIN};CHECK(poll(&event,1,3000)==1 && (event.revents&POLLIN));
	struct uffd_msg message;CHECK(read(uffd,&message,sizeof(message))==sizeof(message));
	CHECK(message.event==UFFD_EVENT_PAGEFAULT && (message.arg.pagefault.address&~UINT64_C(4095))==(uintptr_t)argument);
	cpu.regs.orig_rax=SYS_execve;cpu.regs.rdi=(uintptr_t)executable;
	cpu.regs.rsi=(uintptr_t)exec_argv;cpu.regs.rdx=(uintptr_t)exec_env;
	CHECK(!source_control_raw(request.selector,VMCTX_CTL_SETCPU,&cpu));
	struct vmctx_syscall_gate gate={.version=VMCTX_SYSCALL_GATE_ABI,.size=sizeof(gate),.ticket=1};
	CHECK(!gate_step(&context,&gate,VMCTX_GATE_BEGIN));CHECK(!gate_wait(&context,&gate,VMCTX_GATE_ADMITTED));
	CHECK(!gate_step(&context,&gate,VMCTX_GATE_COMMIT));CHECK(!gate_wait(&context,&gate,VMCTX_GATE_COMPLETE) && !gate.dispatch_ret);
	CHECK(!source_context_info(&context,&info) && info.mm_identity && info.mm_identity!=request.mm);
	int old_live=source_mm_live(request.mm);
	struct uffdio_copy copy={.dst=(uintptr_t)argument,.src=(uintptr_t)bytes,.len=4096};
	CHECK(!ioctl(uffd,UFFDIO_COPY,&copy));CHECK(!pthread_join(thread,NULL));started=0;
	fprintf(stderr,"memory-mm legacy=%d old=%llu new=%llu old_live_during_copy=%d returned=%ld errno=%d value=%llx\n",
		legacy,(unsigned long long)request.mm,(unsigned long long)info.mm_identity,old_live,
		request.result,request.error,(unsigned long long)request.value);
	CHECK(old_live==1 && request.result==8 && request.value==VALUE);
	CHECK(source_mm_live(request.mm)==0);
	CHECK(source_record_memory(request.source,request.mm,VMCTX_CTL_PEEK,(void *)1)==-1 && errno==ESTALE);
	struct source_memory_target current,historical;
	CHECK(!source_memory_target_capture(request.source,&current) && current.mm==info.mm_identity && current.epoch==request.target.epoch+1);
	CHECK(!source_memory_target_for_mm(request.source,request.mm,&historical) && historical.epoch==request.target.epoch);
	CHECK(source_memory_target_call(&historical,VMCTX_CTL_PEEK,(void *)1)==-1 && errno==ESRCH);
	puts("PASS: source memory controls retain the exact captured MM and metadata through exec, and reject wrong identities before touching inner arguments");pass=1;
done:
	/* Unregister first: a pending monitor copy must be released before its
	 * thread is joined or the source's final MM references are inspected. */
	if(registered) {struct uffdio_range range={.start=(uintptr_t)argument,.len=4096};(void)ioctl(uffd,UFFDIO_UNREGISTER,&range);}
	if(started)pthread_join(thread,NULL);
	if(child>0) {kill(child,SIGKILL);int status;while(waitpid(child,&status,0)<0 && errno==EINTR){} child=0;}
	if(registered_source)source_record_release(registered_source);
	source_context_close(&context);
	if(uffd>=0)close(uffd);
	if(argument!=MAP_FAILED)munmap(argument,4096);
	if(bytes!=MAP_FAILED)munmap(bytes,4096);
	if(page!=MAP_FAILED)munmap(page,4096);
	alarm(0);return pass?0:1;
}
