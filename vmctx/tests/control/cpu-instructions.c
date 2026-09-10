// SPDX-License-Identifier: GPL-2.0
/* Local backend control: execute the negotiated CPU contract on real hardware.
 * No guest syscall is emulated. UD2 terminates the probe at a checked address. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>

extern const char cpu_probe[], cpu_probe_avx[], cpu_probe_done[];
asm(".text\n"
    ".global cpu_probe,cpu_probe_avx,cpu_probe_done\n"
    "cpu_probe:\n"
    "mov %rdi,%r12\n"
    "xor %eax,%eax; xor %ecx,%ecx; cpuid\n"
    "mov %eax,0(%r12); mov %ebx,4(%r12); mov %ecx,8(%r12); mov %edx,12(%r12)\n"
    "mov $1,%eax; xor %ecx,%ecx; .byte 0x66; cpuid\n"
    "mov %eax,16(%r12); mov %ebx,20(%r12); mov %ecx,24(%r12); mov %edx,28(%r12)\n"
    "mov $7,%eax; xor %ecx,%ecx; cpuid\n"
    "mov %eax,32(%r12); mov %ebx,36(%r12); mov %ecx,40(%r12); mov %edx,44(%r12)\n"
    "mov $13,%eax; xor %ecx,%ecx; cpuid\n"
    "mov %eax,48(%r12); mov %ebx,52(%r12); mov %ecx,56(%r12); mov %edx,60(%r12)\n"
    "xor %ecx,%ecx; xgetbv\n"
    "mov %eax,64(%r12); mov %edx,68(%r12)\n"
    "xsave 128(%r12)\n"
    "mov $0x8000000a,%eax; xor %ecx,%ecx; cpuid\n"
    "mov %eax,80(%r12); mov %ebx,84(%r12); mov %ecx,88(%r12); mov %edx,92(%r12)\n"
    "cpu_probe_avx: vpxor %ymm0,%ymm0,%ymm0\n"
    "cpu_probe_done: ud2\n");

static volatile sig_atomic_t child, expired;
static long ctl_nr;
static void deadline(int sig)
{
	(void)sig;
	expired = 1;
	if (child > 0) kill(child, SIGKILL);
}
static int control(unsigned op, void *arg)
{
	for (unsigned i=0; i<10000 && !expired; i++) {
		int r=syscall(ctl_nr,child,op,arg);
		if (!r || errno!=EAGAIN) return r;
		usleep(100);
	}
	errno=ETIMEDOUT; return -1;
}
static int run(long run_nr, int baseline)
{
	struct vmctx_cpu_model model;
	struct vmctx_cpu_state state;
	struct vmctx_run_config config={.flags=VMCTX_FLAG_USERCODE|VMCTX_FLAG_WAIT_MONITOR|
		VMCTX_FLAG_RESTORE|VMCTX_FLAG_REDIRECT_FAULT|VMCTX_FLAG_REDIRECT_SYSCALL,
		.backing_fd=-1,.shared_fd=-1,.max_exits=128};
	if (syscall(ctl_nr,0,VMCTX_CTL_CPU_CAPS,&model) || model.xcr0!=7) return 2;
	/* Model a peer that lacks SHA; baseline also models no YMM support. */
	model.leaf7_ebx &= ~(1U<<29);
	if (baseline) { model.xcr0=3; vmctx_cpu_model_trim(&model); }
	unsigned char *report=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
	if (report==MAP_FAILED) return 2;
	memset(report,0xcc,4096);
	int status, passed=0;
	pid_t parent=getpid();
	child=fork();
	if (child<0) { munmap(report,4096); return 2; }
	if (!child) {
		if (prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=parent) _exit(125);
		syscall(run_nr,&config);
		_exit(126);
	}
	expired=0; alarm(15);
	int attached=0;
	for (unsigned i=0;i<2000 && !expired;i++) {
		if (!control(VMCTX_CTL_ATTACH,NULL)) { attached=1; break; }
		usleep(1000);
	}
	if (!attached || control(VMCTX_CTL_GETCPU,&state)) goto done;
	memset(state.xstate,0,sizeof(state.xstate));
	uint16_t fcw=0x37f; uint32_t mxcsr=0x1f80; uint64_t active=3;
	memcpy(state.xstate,&fcw,2); memcpy(state.xstate+24,&mxcsr,4);
	memcpy(state.xstate+512,&active,8); state.xstate_size=576;
	state.regs.rip=(uintptr_t)cpu_probe;
	state.regs.rdi=(uintptr_t)report;
	state.regs.rsp=(uintptr_t)report+4096;
	state.regs.rflags=0x202; state.regs.orig_rax=~0ULL;
	if (control(VMCTX_CTL_CPU_MODEL,&model) || control(VMCTX_CTL_SETCPU,&state)) goto done;
	struct vmctx_reply reply={.action=VMCTX_ACT_SELF};
	if (control(VMCTX_CTL_RESUME,&reply)) goto done;
	for (unsigned i=0;i<128 && !expired;i++) {
		struct vmctx_event event;
		if (control(VMCTX_CTL_WAIT,&event)) goto done;
		if (event.type!=VMCTX_EV_FAULT) goto done;
		if (event.nr==14) {
			/* These are this fixture's own preexisting local mappings. */
			if (control(VMCTX_CTL_RESUME,&reply)) goto done;
			continue;
		}
		if (event.nr!=6 || event.rip!=(uintptr_t)(baseline?cpu_probe_avx:cpu_probe_done)) {
			fprintf(stderr,"unexpected vector=%llu rip=%llx baseline=%d\n",
				(unsigned long long)event.nr,(unsigned long long)event.rip,baseline);
			goto done;
		}
		__u32 expected[4];
		const unsigned leaves[]={0,1,7,13};
		for (unsigned j=0;j<4;j++) {
			vmctx_cpu_model_cpuid(&model,leaves[j],0,&expected[0],&expected[1],&expected[2],&expected[3]);
			if (memcmp(report+16*j,expected,16)) {
				uint32_t actual[4];
				memcpy(actual, report+16*j, sizeof(actual));
				fprintf(stderr, "CPUID leaf %x: actual %x %x %x %x; expected %x %x %x %x\n",
					leaves[j], actual[0], actual[1], actual[2], actual[3],
					expected[0], expected[1], expected[2], expected[3]);
				goto done;
			}
		}
		uint64_t xcr0;
		memcpy(&xcr0,report+64,8);
		if (xcr0!=model.xcr0) goto done;
		for (unsigned j=80;j<96;j++) if (report[j]) goto done;
		unsigned size=baseline?576:832;
		for (unsigned j=128+size;j<128+1024;j++) if (report[j]!=0xcc) goto done;
		passed=1; break;
	}
done:
	if (!passed) fprintf(stderr,"FAIL: hardware CPU model baseline=%d errno=%d expired=%d\n",baseline,errno,expired);
	kill(child,SIGKILL);
	if (waitpid(child,&status,0)!=child || !WIFSIGNALED(status) || WTERMSIG(status)!=SIGKILL) passed=0;
	child=0; alarm(0); munmap(report,4096);
	if (passed) printf("PASS: hardware CPUID/XCR0/XSAVE with %s model%s\n",
		baseline?"SSE":"AVX",baseline?", unadvertised AVX raises #UD":"");
	return passed?0:1;
}
int main(int argc,char **argv)
{
	struct sigaction action={.sa_handler=deadline};
	struct vmctx_cpu_model before, after;
	if (argc!=3 || sigaction(SIGALRM,&action,NULL)) return 2;
	ctl_nr=strtol(argv[2],NULL,10);
	if (syscall(ctl_nr,0,VMCTX_CTL_CPU_CAPS,&before)) return 2;
	int result=run(strtol(argv[1],NULL,10),0);
	if (!result) result=run(strtol(argv[1],NULL,10),1);
	/* CPU_CAPS samples XCR0 on every online CPU, including the CPU that
	 * ran the restricted probe. A leaked guest XCR0 must shrink this set. */
	if (syscall(ctl_nr,0,VMCTX_CTL_CPU_CAPS,&after) ||
	    memcmp(&before,&after,sizeof(before))) {
		fprintf(stderr,"FAIL: host CPU capabilities changed after guest execution\n");
		return 1;
	}
	if (!result) puts("PASS: host XCR0 restored on all online CPUs");
	return result;
}
