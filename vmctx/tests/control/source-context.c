// SPDX-License-Identifier: GPL-2.0
/* Exercise the actual source adapter's proc-handle identity checks with PID
 * reuse between validation and open, and between open and validation. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>

static long run_nr, ctl_nr;
static volatile sig_atomic_t child;
static int inject_phase=-1, hook_failed;
static long source_control_raw(pid_t pid, unsigned op, void *arg)
{ return syscall(ctl_nr,pid,op,arg); }
static void observe_open(int phase, pid_t pid);
#define SOURCE_CONTEXT_OPEN_OBSERVE(phase,pid) observe_open(phase,pid)
#include "../../user/source-context.h"
#include "../../user/source-registry.h"

static void deadline(int sig)
{
	(void)sig;
	if (child>0) kill(child,SIGKILL);
	_exit(124);
}
static int reap(void)
{
	int status;
	pid_t pid=child, got;
	if (pid<=0 || kill(pid,SIGKILL)) return -1;
	do { got=waitpid(pid,&status,0); } while (got<0 && errno==EINTR);
	child=0;
	return got==pid && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL ? 0 : -1;
}
static int spawn(void)
{
	struct vmctx_run_config cfg={.flags=VMCTX_FLAG_SERVICE|VMCTX_FLAG_WAIT_MONITOR,
		.backing_fd=-1,.shared_fd=-1};
	child=fork();
	if (child<0) return -1;
	if (!child) {
		if (prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=1) _exit(125);
		syscall(run_nr,&cfg); _exit(126);
	}
	for (int i=0;i<2000;i++) {
		if (!source_control_raw(child,VMCTX_CTL_ATTACH,NULL)) return 0;
		usleep(1000);
	}
	return -1;
}
static void observe_open(int phase, pid_t pid)
{
	if (phase!=inject_phase) return;
	inject_phase=-1;
	if (child!=pid || reap()) { hook_failed=1; return; }
	FILE *f=fopen("/proc/sys/kernel/ns_last_pid","w");
	if (!f) { hook_failed=1; return; }
	int n=fprintf(f,"%d\n",pid-1), closed=fclose(f);
	if (n<=0 || closed || spawn() || child!=pid) hook_failed=1;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno); goto done; } } while (0)

static int gate_call(const struct source_context *s, unsigned op,
		struct vmctx_syscall_gate *gate)
{
	gate->op=op;
	return (int)source_control_raw(source_context_selector(s),VMCTX_CTL_SYSCALL_GATE,gate);
}
static int gate_wait(const struct source_context *s,
		struct vmctx_syscall_gate *gate, unsigned state)
{
	for (int i=0;i<10000;i++) {
		int r=gate_call(s,VMCTX_GATE_QUERY,gate);
		if (!r && gate->state==state) return 0;
		if (r && errno!=EBUSY && errno!=EAGAIN) return -1;
		usleep(1000);
	}
	errno=ETIMEDOUT; return -1;
}
static int get_cpu(const struct source_context *s, struct vmctx_cpu_state *cpu)
{
	for (int i=0;i<2000;i++) {
		int r=source_control_raw(source_context_selector(s),VMCTX_CTL_GETCPU,cpu);
		if (!r) return 0;
		if (errno!=EBUSY && errno!=EAGAIN) return -1;
		usleep(1000);
	}
	errno=ETIMEDOUT; return -1;
}
static char executable[]="/bin/true";
static char *exec_argv[]={executable,NULL}, *exec_env[]={NULL};
static int exec_identity(void)
{
	struct source_context leader={.fd=-1}, worker={.fd=-1};
	struct vmctx_context info;
	struct vmctx_cpu_state cpu;
	struct vmctx_cpu_model model;
	struct vmctx_syscall_gate gate={.version=VMCTX_SYSCALL_GATE_ABI,
		.size=sizeof(gate),.ticket=1};
	int procfd=-1, pass=0;
	void *stack=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
	CHECK(stack!=MAP_FAILED);
	CHECK(!spawn());
	CHECK(!source_context_open(child,&leader));
	CHECK(!source_control_raw(0,VMCTX_CTL_CPU_CAPS,&model));
	CHECK(!source_control_raw(source_context_selector(&leader),VMCTX_CTL_CPU_MODEL,&model));
	CHECK(!get_cpu(&leader,&cpu));
	cpu.regs.orig_rax=SYS_clone;
	cpu.regs.rdi=CLONE_VM|CLONE_SIGHAND|CLONE_THREAD;
	cpu.regs.rsi=(uintptr_t)stack+65536;
	cpu.regs.rdx=cpu.regs.r10=cpu.regs.r8=0;
	CHECK(!source_control_raw(source_context_selector(&leader),VMCTX_CTL_SETCPU,&cpu));
	CHECK(!gate_call(&leader,VMCTX_GATE_BEGIN,&gate));
	CHECK(!gate_wait(&leader,&gate,VMCTX_GATE_ADMITTED));
	CHECK(!gate_call(&leader,VMCTX_GATE_COMMIT,&gate));
	CHECK(!gate_wait(&leader,&gate,VMCTX_GATE_COMPLETE) && gate.child_pid>0 && gate.child_shared_mm);
	CHECK(!source_context_child(&leader,gate.ticket,&worker));
	CHECK(worker.pid==(pid_t)gate.child_pid && worker.pid!=child);
	CHECK(!source_context_group(&worker) && worker.tgid==child && worker.pid!=child);
	CHECK(!source_context_info(&worker,&info));
	uint64_t identity=info.identity, old_mm=info.mm_identity;
	CHECK(source_mm_live(old_mm)==1);
	CHECK(!get_cpu(&worker,&cpu));
	cpu.regs.orig_rax=SYS_execve;
	cpu.regs.rdi=(uintptr_t)executable;
	cpu.regs.rsi=(uintptr_t)exec_argv;
	cpu.regs.rdx=(uintptr_t)exec_env;
	CHECK(!source_control_raw(source_context_selector(&worker),VMCTX_CTL_SETCPU,&cpu));
	gate=(struct vmctx_syscall_gate){.version=VMCTX_SYSCALL_GATE_ABI,.size=sizeof(gate),.ticket=1};
	CHECK(!gate_call(&worker,VMCTX_GATE_BEGIN,&gate));
	CHECK(!gate_wait(&worker,&gate,VMCTX_GATE_ADMITTED));
	CHECK(!gate_call(&worker,VMCTX_GATE_COMMIT,&gate));
	CHECK(!gate_wait(&worker,&gate,VMCTX_GATE_COMPLETE) && gate.dispatch_ret==0);
	CHECK(!source_context_info(&worker,&info));
	CHECK(info.identity==identity && info.mm_identity && info.mm_identity!=old_mm);
	CHECK(info.native_pid==(unsigned)worker.tgid && info.native_tgid==(unsigned)worker.tgid);
	CHECK(source_mm_live(old_mm)==0 && source_mm_live(info.mm_identity)==1);
	uint64_t mm; pid_t named;
	procfd=source_context_proc_open(&worker,"maps",&mm,&named);
	CHECK(procfd>=0 && named==worker.tgid && named!=worker.pid && mm==info.mm_identity);
	char maps[8192]; CHECK(read(procfd,maps,sizeof(maps))>0);
	CHECK(!close(procfd)); procfd=-1;
	/* A distro can make true a symlink (this host uses gnutrue). Compare
	 * the retained executable file to the requested program's inode. */
	procfd=source_context_proc_open(&worker,"exe",&mm,&named);
	CHECK(procfd>=0 && named==worker.tgid && mm==info.mm_identity);
	struct stat actual, expected;
	CHECK(!fstat(procfd,&actual) && !stat(executable,&expected));
	CHECK(actual.st_dev==expected.st_dev && actual.st_ino==expected.st_ino);
	CHECK(!source_context_info(&leader,&info));
	CHECK(info.identity!=identity && info.flags==VMCTX_CONTEXT_ENDED && info.mm_identity==old_mm);
	puts("PASS: non-leader exec preserves context identity, changes MM identity, and validates the new native PID before opening its maps");
	pass=1;
done:
	if (child>0) reap();
	if (procfd>=0) close(procfd);
	source_context_close(&leader); source_context_close(&worker);
	if (stack!=MAP_FAILED) munmap(stack,65536);
	return pass ? 0 : 1;
}

/* CLONE_VM without CLONE_THREAD: the parent can die while a different thread
 * group still holds the same MM, before any userspace child registration. */
static int pending_mm_lifetime(void)
{
	struct source_context parent={.fd=-1}, birth={.fd=-1};
	struct vmctx_context info;
	struct vmctx_cpu_state cpu;
	struct vmctx_cpu_model model;
	struct vmctx_syscall_gate gate={.version=VMCTX_SYSCALL_GATE_ABI,
		.size=sizeof(gate),.ticket=1};
	pid_t born=0;
	int pass=0;
	void *stack=mmap(NULL,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
	CHECK(stack!=MAP_FAILED && !spawn());
	CHECK(!source_context_open(child,&parent));
	CHECK(!source_control_raw(0,VMCTX_CTL_CPU_CAPS,&model));
	CHECK(!source_control_raw(source_context_selector(&parent),VMCTX_CTL_CPU_MODEL,&model));
	CHECK(!get_cpu(&parent,&cpu));
	CHECK(!source_context_info(&parent,&info));
	uint64_t mm=info.mm_identity;
	CHECK(mm && source_mm_live(mm)==1);
	CHECK(source_mm_live(0)==-1 && errno==EINVAL);
	CHECK(source_mm_live(UINT64_MAX)==-1 && errno==ENOENT);
	CHECK(!setresuid(65534,65534,0));
	int denied=source_mm_live(mm), denial=errno;
	CHECK(!setresuid(0,0,0));
	CHECK(denied==-1 && denial==EPERM);
	cpu.regs.orig_rax=SYS_clone;
	cpu.regs.rdi=CLONE_VM|SIGCHLD;
	cpu.regs.rsi=(uintptr_t)stack+65536;
	cpu.regs.rdx=cpu.regs.r10=cpu.regs.r8=0;
	CHECK(!source_control_raw(source_context_selector(&parent),VMCTX_CTL_SETCPU,&cpu));
	CHECK(!gate_call(&parent,VMCTX_GATE_BEGIN,&gate));
	CHECK(!gate_wait(&parent,&gate,VMCTX_GATE_ADMITTED));
	CHECK(!gate_call(&parent,VMCTX_GATE_COMMIT,&gate));
	CHECK(!gate_wait(&parent,&gate,VMCTX_GATE_COMPLETE));
	born=(pid_t)gate.child_pid;
	CHECK(born>0 && gate.child_shared_mm==1);
	CHECK(!reap());
	/* The monitor has only the dead parent's handle; the pending child's
	 * native MM reference is already counted. Closing this handle cannot
	 * manufacture an MM death, either. */
	CHECK(!source_context_info(&parent,&info) && info.flags==VMCTX_CONTEXT_ENDED);
	CHECK(source_mm_live(mm)==1);
	CHECK(!source_context_child(&parent,gate.ticket,&birth));
	CHECK(birth.pid==born && birth.tgid==born);
	CHECK(!source_context_info(&birth,&info) && info.mm_identity==mm);
	source_context_close(&parent);
	CHECK(source_mm_live(mm)==1);
	CHECK(!source_context_signal(&birth,0));
	CHECK(!source_context_signal(&birth,SIGKILL));
	int status;
	CHECK(waitpid(born,&status,0)==born && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
	born=0;
	CHECK(source_mm_live(mm)==0);
	CHECK(source_context_signal(&birth,SIGKILL)==-1 && errno==ESRCH);
	puts("PASS: a pending CLONE_VM child keeps its MM live after parent death; exact-handle signalling retires only the retained task");
	pass=1;
done:
	if (child>0) reap();
	if (born>0) { kill(born,SIGKILL); waitpid(born,NULL,0); }
	source_context_close(&parent); source_context_close(&birth);
	if (stack!=MAP_FAILED) munmap(stack,65536);
	return pass ? 0 : 1;
}
static int exercise(void)
{
	struct source_context original={.fd=-1}, fresh={.fd=-1};
	struct vmctx_context info;
	uint64_t mm;
	pid_t named;
	int procfd=-1, pass=0;
	source_id retired=0, current=0;
	int retired_owned=0;
	struct source_record *borrowed=NULL;
	CHECK(getpid()==1);
	/* The inherited proc mount uses the outer namespace. The adapter must
	 * reject it before reading any named task, even though the ctl PID lookup
	 * finds this namespace's source context. Mount a matching proc instance
	 * only inside the test's private mount namespace for the remaining cases. */
	CHECK(!spawn());
	CHECK(!source_context_open(child,&original));
	procfd=source_context_proc_open(&original,"maps",&mm,&named);
	CHECK(procfd==-1 && errno==EXDEV);
	CHECK(!reap()); source_context_close(&original);
	CHECK(!mount(NULL,"/",NULL,MS_REC|MS_PRIVATE,NULL));
	CHECK(!mount("proc","/proc","proc",MS_NOSUID|MS_NODEV|MS_NOEXEC,NULL));
	for (int phase=0;phase<2;phase++) {
		CHECK(!spawn());
		CHECK(!source_context_open(child,&original));
		CHECK(!source_context_group(&original) && original.tgid==child);
		CHECK(!source_context_info(&original,&info));
		uint64_t identity=info.identity, original_mm=info.mm_identity;
		CHECK(identity && original_mm);
		procfd=source_context_proc_open(&original,"maps",&mm,&named);
		CHECK(procfd>=0 && mm==original_mm && named==child);
		CHECK(fcntl(procfd,F_GETFD)==FD_CLOEXEC);
		char map[4096]; CHECK(read(procfd,map,sizeof(map))>0);
		CHECK(!close(procfd)); procfd=-1;
		inject_phase=phase;
		procfd=source_context_proc_open(&original,"maps",&mm,&named);
		CHECK(inject_phase==-1 && !hook_failed);
		CHECK(procfd==-1 && errno==ESTALE);
		CHECK(source_context_signal(&original,SIGKILL)==-1 && errno==ESRCH);
		CHECK(kill(child,0)==0); /* the reused PID's fresh task was not signalled */
		CHECK(!source_context_info(&original,&info));
		CHECK(info.identity==identity && info.flags==VMCTX_CONTEXT_ENDED && info.exit_status==SIGKILL);
		CHECK(!source_context_open(child,&fresh));
		CHECK(!source_context_info(&fresh,&info));
		CHECK(info.identity!=identity && info.mm_identity!=original_mm);
		procfd=source_context_proc_open(&fresh,"maps",&mm,&named);
		CHECK(procfd>=0 && named==child && mm==info.mm_identity);
		CHECK(!close(procfd)); procfd=-1;
		CHECK(!reap());
		source_context_close(&original); source_context_close(&fresh);
	}
	puts("PASS: source proc adapter rejects PID reuse before and after open; fresh context identity and MM remain distinct");
	CHECK(!exec_identity());
	CHECK(!pending_mm_lifetime());
	CHECK(!spawn());
	CHECK(!source_context_open(child,&original));
	retired=source_record_add(&original); CHECK(retired>0 && original.fd==-1);
	retired_owned=1;
	borrowed=source_record_get(retired); CHECK(borrowed);
	CHECK(!source_record_info(retired,&info));
	uint64_t retired_identity=info.identity;
	CHECK(!reap());
	source_record_release(retired);
	retired_owned=0;
	/* A fault service's independent reference keeps the native handle
	 * usable after its connection releases the original reference. */
	CHECK(!source_context_info(&borrowed->native,&info) && info.identity==retired_identity);
	source_record_put(borrowed); borrowed=NULL;
	CHECK(!source_record_info(retired,&info) && info.identity==retired_identity && info.flags==VMCTX_CONTEXT_ENDED);
	CHECK(!spawn());
	CHECK(!source_context_open(child,&fresh));
	current=source_record_add(&fresh); CHECK(current>retired && fresh.fd==-1);
	CHECK(!source_record_info(current,&info) && info.identity!=retired_identity);
	CHECK(!source_record_info(retired,&info) && info.identity==retired_identity && info.flags==VMCTX_CONTEXT_ENDED);
	CHECK(!source_record_get(retired) && errno==ESRCH);
	struct vmctx_cpu_state cpu;
	CHECK(source_record_ctl(retired,VMCTX_CTL_GETCPU,&cpu)==-1 && errno==ESRCH);
	CHECK(!reap()); source_record_release(current); current=0;
	retired=0;
	FILE *counts=fopen("/sys/module/kernel/parameters/vmctx_context_handles","r");
	CHECK(counts);
	long handles=-1; int scanned=fscanf(counts,"%ld",&handles), closed=fclose(counts);
	CHECK(scanned==1 && !closed && handles==0);
	puts("PASS: source registry retires native handles, keeps terminal metadata, and never reuses an old context token");
	pass=1;
done:
	if (child>0) reap();
	if (procfd>=0) close(procfd);
	source_context_close(&original); source_context_close(&fresh);
	if (current) source_record_release(current);
	if (retired_owned) source_record_release(retired);
	if (borrowed) source_record_put(borrowed);
	return pass ? 0 : 1;
}
int main(int argc, char **argv)
{
	if (argc!=3) return 2;
	run_nr=strtol(argv[1],NULL,10); ctl_nr=strtol(argv[2],NULL,10);
	signal(SIGALRM,deadline); alarm(25);
	if (unshare(CLONE_NEWPID|CLONE_NEWNS)) { perror("unshare test namespaces"); return 1; }
	child=fork();
	if (child<0) return 1;
	if (!child) { alarm(20); return exercise(); }
	int status; pid_t got;
	do { got=waitpid(child,&status,0); } while (got<0 && errno==EINTR);
	child=0; alarm(0);
	return got>0 && WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
