// SPDX-License-Identifier: GPL-2.0
/* Retain committed native births across parent and child teardown. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sched.h>
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
#include "../../kernel/vmctx_access.h"
static long ctl_nr;
static volatile sig_atomic_t parent, kid;
static struct vmctx_cpu_state cpu;
static void deadline(int signal)
{
	(void)signal;
	if (parent > 0) kill(parent, SIGKILL);
	if (kid > 0) kill(kid, SIGKILL);
	_exit(124);
}
static long ctl(pid_t selector, unsigned cmd, void *arg)
{ return syscall(ctl_nr, selector, cmd, arg); }
static pid_t selector(int fd) { return -(fd + 1); }
static long handles(void)
{
	long count=-1;
	FILE *f=fopen("/sys/module/kernel/parameters/vmctx_context_handles","r");
	if(f) { if(fscanf(f,"%ld",&count)!=1) count=-1; fclose(f); }
	return count;
}
static int context(pid_t target, unsigned op, uint64_t ticket, struct vmctx_context *r)
{
	*r = (struct vmctx_context){.version=VMCTX_CONTEXT_ABI, .size=sizeof(*r), .op=op, .ticket=ticket};
	for (int i=0; i<2000; i++) {
		int ret=ctl(target, VMCTX_CTL_CONTEXT, r);
		if (!ret || errno != EBUSY) return ret;
		usleep(1000);
	}
	errno=ETIMEDOUT; return -1;
}
static int cpuctl(pid_t target, unsigned cmd, void *arg)
{
	for (int i=0;i<2000;i++) {
		int ret=ctl(target,cmd,arg);
		if (!ret || (errno!=EAGAIN && errno!=EBUSY)) return ret;
		usleep(1000);
	}
	errno=ETIMEDOUT; return -1;
}
static int gate(pid_t target, struct vmctx_syscall_gate *g, unsigned op)
{
	g->op=op;
	return ctl(target,VMCTX_CTL_SYSCALL_GATE,g);
}
static int gate_wait(pid_t target, struct vmctx_syscall_gate *g, int birth)
{
	for (int i=0;i<10000;i++) {
		int ret=gate(target,g,VMCTX_GATE_QUERY);
		if (!ret && (birth ? !!g->child_host_pid : g->state==VMCTX_GATE_ADMITTED)) return 0;
		if (ret && errno!=EAGAIN && errno!=EBUSY) return ret;
		usleep(1000);
	}
	errno=ETIMEDOUT; return -1;
}
static int reaped(pid_t pid)
{
	int status;
	pid_t got;
	do { got=waitpid(pid,&status,0); } while (got<0 && errno==EINTR);
	return got==pid && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL;
}
static int zombie(pid_t pid)
{
	char path[80], buf[512];
	snprintf(path,sizeof(path),"/proc/%d/stat",pid);
	for (int i=0;i<3000;i++) {
		int fd=open(path,O_RDONLY);
		ssize_t n=fd<0 ? -1 : read(fd,buf,sizeof(buf)-1);
		if (fd>=0) close(fd);
		if (n>0) { buf[n]=0; char *p=strrchr(buf,')'); if(p && p[2]=='Z') return 1; }
		usleep(1000);
	}
	return 0;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno); goto done; } } while(0)
static int reuse_inner(long run_nr)
{
	int old=-1, fresh=-1, pass=0, status;
	pid_t reused=0;
	uint64_t identity=0;
	struct vmctx_context info;
	struct vmctx_run_config cfg={.flags=VMCTX_FLAG_SERVICE|VMCTX_FLAG_WAIT_MONITOR,.backing_fd=-1,.shared_fd=-1};
	CHECK(getpid()==1);
	for (int generation=0; generation<2; generation++) {
		if (generation) {
			/* This counter belongs only to this test's fresh PID namespace. */
			FILE *f=fopen("/proc/sys/kernel/ns_last_pid","w");
			CHECK(f);
			int written=fprintf(f,"%d\n",reused-1), closed=fclose(f);
			CHECK(written>0 && !closed);
		}
		parent=fork(); CHECK(parent>=0);
		if (!parent) { syscall(run_nr,&cfg); _exit(126); }
		if (!generation) reused=parent;
		CHECK(parent==reused);
		for (int i=0;ctl(parent,VMCTX_CTL_ATTACH,NULL);i++) { CHECK(i<2000); usleep(1000); }
		CHECK(!context(parent,VMCTX_CONTEXT_OPEN,0,&info));
		if (!generation) { old=info.fd; identity=info.identity; }
		else {
			fresh=info.fd;
			CHECK(info.identity && info.identity!=identity);
			CHECK(!context(selector(old),VMCTX_CONTEXT_INFO,0,&info));
			CHECK(info.identity==identity && info.flags==VMCTX_CONTEXT_ENDED && info.exit_status==SIGKILL);
			CHECK(ctl(selector(old),VMCTX_CTL_GETCPU,&cpu)==-1 && errno==ESRCH);
			CHECK(!context(selector(fresh),VMCTX_CONTEXT_INFO,0,&info));
			CHECK(info.identity!=identity && !(info.flags & VMCTX_CONTEXT_ENDED));
		}
		CHECK(!kill(parent,SIGKILL)); CHECK(reaped(parent)); parent=0;
	}
	CHECK(!close(old)); old=-1;
	CHECK(!close(fresh)); fresh=-1;
	CHECK(handles()==0);
	printf("PASS: two source tasks reused namespace PID %d; the retained descriptor still names only the original terminal context\n",reused);
	pass=1;
done:
	if(parent>0) { kill(parent,SIGKILL); while(waitpid(parent,&status,0)<0 && errno==EINTR) {} parent=0; }
	if(old>=0)close(old);
	if(fresh>=0)close(fresh);
	return pass ? 0 : 1;
}
static int reuse_namespace(long run_nr)
{
	if (unshare(CLONE_NEWPID)) { perror("unshare PID namespace"); return 1; }
	parent=fork();
	if (parent<0) return 1;
	if (!parent) { alarm(25); return reuse_inner(run_nr); }
	int status;
	pid_t got;
	do { got=waitpid(parent,&status,0); } while(got<0 && errno==EINTR);
	parent=0;
	return got>0 && WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
int main(int argc, char **argv)
{
	if (argc!=4 || (strcmp(argv[3],"parent-first") && strcmp(argv[3],"child-first") && strcmp(argv[3],"vfork") && strcmp(argv[3],"pid-reuse"))) return 2;
	long run_nr=strtol(argv[1],NULL,10); ctl_nr=strtol(argv[2],NULL,10);
	if (!strcmp(argv[3],"pid-reuse")) { signal(SIGALRM,deadline); alarm(30); return reuse_namespace(run_nr); }
	int child_first=!strcmp(argv[3],"child-first"), vfork_mode=!strcmp(argv[3],"vfork");
	int fd=-1, duplicate=-1, child_fd=-1, fake=-1, pass=0;
	struct vmctx_context info, child_info;
	void *bad=MAP_FAILED;
	signal(SIGALRM,deadline); alarm(25);
	CHECK(!prctl(PR_SET_CHILD_SUBREAPER,1));
	pid_t monitor=getpid();
	struct vmctx_run_config cfg={.flags=VMCTX_FLAG_SERVICE|VMCTX_FLAG_WAIT_MONITOR,.backing_fd=-1,.shared_fd=-1};
	parent=fork(); CHECK(parent>=0);
	if (!parent) {
		if (prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=monitor) _exit(125);
		syscall(run_nr,&cfg); _exit(126);
	}
	for (int i=0;ctl(parent,VMCTX_CTL_ATTACH,NULL);i++) { CHECK(i<2000); usleep(1000); }
	CHECK(!context(parent,VMCTX_CONTEXT_OPEN,0,&info)); fd=info.fd;
	uint64_t identity=info.identity;
	CHECK(identity && info.native_pid==(unsigned)parent && fcntl(fd,F_GETFD)==FD_CLOEXEC);
	CHECK(!context(parent,VMCTX_CONTEXT_OPEN,0,&info)); duplicate=info.fd;
	CHECK(info.identity==identity && duplicate!=fd && handles()==2);
	bad=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); CHECK(bad!=MAP_FAILED);
	*(struct vmctx_context *)bad=(struct vmctx_context){.version=VMCTX_CONTEXT_ABI,.size=sizeof(info),.op=VMCTX_CONTEXT_OPEN};
	CHECK(!mprotect(bad,4096,PROT_READ));
	CHECK(ctl(parent,VMCTX_CTL_CONTEXT,bad)==-1 && errno==EFAULT);
	CHECK(handles()==2);
	fake=memfd_create("not-a-context",MFD_CLOEXEC); CHECK(fake>=0);
	CHECK(context(selector(fake),VMCTX_CONTEXT_INFO,0,&info)==-1 && errno==EBADF);
	pid_t other=fork(); CHECK(other>=0);
	if (!other) _exit(context(selector(fd),VMCTX_CONTEXT_INFO,0,&info)==-1 && errno==EPERM ? 0 : 1);
	int status; CHECK(waitpid(other,&status,0)==other && WIFEXITED(status) && !WEXITSTATUS(status));
	/* Preserve the saved root UID only to restore this test's credentials.
	 * The live descriptor must not bypass the real-credential ptrace check. */
	CHECK(!setresuid(65534,65534,0));
	int denied=context(selector(fd),VMCTX_CONTEXT_INFO,0,&info), denial=errno;
	CHECK(!setresuid(0,0,0));
	CHECK(denied==-1 && denial==EPERM);
	struct vmctx_cpu_model model;
	CHECK(!ctl(0,VMCTX_CTL_CPU_CAPS,&model));
	CHECK(!cpuctl(selector(fd),VMCTX_CTL_CPU_MODEL,&model));
	CHECK(!cpuctl(selector(fd),VMCTX_CTL_GETCPU,&cpu));
	CHECK(!context(selector(fd),VMCTX_CONTEXT_INFO,0,&info));
	uint64_t mm_identity=info.mm_identity;
	fprintf(stderr,"context ready: id=%llu expected=%llu native_pid=%u mm=%llu flags=%u\n",
		(unsigned long long)info.identity,(unsigned long long)identity,info.native_pid,
		(unsigned long long)mm_identity,info.flags);
	CHECK(info.identity==identity && mm_identity && info.flags==VMCTX_CONTEXT_READY);
	struct vmctx_access_log access={.version=VMCTX_ACCESS_ABI,.size=sizeof(access)};
	CHECK(!ctl(selector(fd),VMCTX_CTL_ACCESS_LOG,&access) && access.mm_id==mm_identity);
	cpu.regs.orig_rax=vfork_mode ? SYS_vfork : SYS_fork;
	CHECK(!cpuctl(selector(fd),VMCTX_CTL_SETCPU,&cpu));
	struct vmctx_syscall_gate g={.version=VMCTX_SYSCALL_GATE_ABI,.size=sizeof(g),.ticket=1};
	CHECK(!gate(selector(fd),&g,VMCTX_GATE_BEGIN));
	CHECK(!gate_wait(selector(fd),&g,0) && !g.child_host_pid);
	CHECK(!gate(selector(fd),&g,VMCTX_GATE_COMMIT));
	CHECK(!gate_wait(selector(fd),&g,1)); kid=g.child_host_pid;
	CHECK(kid>0 && g.child_shared_mm==(unsigned)vfork_mode);
	pid_t original_parent=parent, original_child=kid;
	if (child_first) { CHECK(!kill(kid,SIGKILL)); CHECK(zombie(kid)); }
	CHECK(!kill(parent,SIGKILL)); CHECK(reaped(parent)); parent=0;
	if (child_first) { CHECK(reaped(kid)); kid=0; }
	CHECK(!context(selector(fd),VMCTX_CONTEXT_INFO,0,&info));
	CHECK(info.identity==identity && info.native_pid==(unsigned)original_parent &&
	      info.flags==VMCTX_CONTEXT_ENDED && info.exit_status==SIGKILL && info.mm_identity==mm_identity);
	CHECK(!setresuid(65534,65534,0));
	int terminal=context(selector(fd),VMCTX_CONTEXT_INFO,0,&info);
	CHECK(!setresuid(0,0,0));
	CHECK(!terminal && info.identity==identity && info.flags==VMCTX_CONTEXT_ENDED);
	other=fork(); CHECK(other>=0);
	if (!other) _exit(context(selector(fd),VMCTX_CONTEXT_INFO,0,&info)==-1 && errno==EPERM ? 0 : 1);
	CHECK(waitpid(other,&status,0)==other && WIFEXITED(status) && !WEXITSTATUS(status));
	CHECK(ctl(selector(fd),VMCTX_CTL_GETCPU,&cpu)==-1 && errno==ESRCH);
	CHECK(ctl(selector(fd),VMCTX_CTL_SETCPU,&cpu)==-1 && errno==ESRCH);
	CHECK(ctl(selector(fd),VMCTX_CTL_RESUME,NULL)==-1 && errno==ESRCH);
	CHECK(gate(selector(fd),&g,VMCTX_GATE_BEGIN)==-1 && errno==ESRCH);
	CHECK(gate(selector(fd),&g,VMCTX_GATE_COMMIT)==-1 && errno==ESRCH);
	CHECK(gate(selector(fd),&g,VMCTX_GATE_CANCEL)==-1 && errno==ESRCH);
	CHECK(context(selector(fd),VMCTX_CONTEXT_CHILD,2,&info)==-1 && errno==ESTALE);
	CHECK(!gate(selector(fd),&g,VMCTX_GATE_QUERY) && g.child_host_pid==(unsigned)original_child);
	CHECK(!mprotect(bad,4096,PROT_READ|PROT_WRITE));
	*(struct vmctx_context *)bad=(struct vmctx_context){.version=VMCTX_CONTEXT_ABI,.size=sizeof(info),.op=VMCTX_CONTEXT_CHILD,.ticket=1};
	CHECK(!mprotect(bad,4096,PROT_READ));
	CHECK(ctl(selector(fd),VMCTX_CTL_CONTEXT,bad)==-1 && errno==EFAULT);
	CHECK(handles()==2);
	CHECK(!context(selector(fd),VMCTX_CONTEXT_CHILD,1,&child_info)); child_fd=child_info.fd;
	CHECK(child_info.identity && child_info.identity!=identity && child_info.native_pid==(unsigned)original_child);
	CHECK(!close(fd)); fd=-1;
	CHECK(!close(duplicate)); duplicate=-1;
	CHECK(!context(selector(child_fd),VMCTX_CONTEXT_INFO,0,&child_info));
	CHECK(child_info.native_pid==(unsigned)original_child && child_info.mm_identity &&
	      ((child_info.mm_identity==mm_identity)==vfork_mode));
	if (!child_first) { CHECK(!kill(kid,SIGKILL)); CHECK(reaped(kid)); kid=0; }
	CHECK(!context(selector(child_fd),VMCTX_CONTEXT_INFO,0,&child_info));
	CHECK(child_info.flags==VMCTX_CONTEXT_ENDED && child_info.exit_status==SIGKILL);
	CHECK(ctl(selector(child_fd),VMCTX_CTL_GETCPU,&cpu)==-1 && errno==ESRCH);
	CHECK(!close(child_fd)); int closed=child_fd; child_fd=-1;
	CHECK(context(selector(closed),VMCTX_CONTEXT_INFO,0,&info)==-1 && errno==EBADF);
	CHECK(handles()==0);
	printf("PASS: context descriptors preserve identity and committed %s birth after parent/child reap, reject foreign owners and bad descriptors, and survive failed copyout\n",argv[3]);
	pass=1;
done:
	if(parent>0)kill(parent,SIGKILL);
	if(kid>0)kill(kid,SIGKILL);
	while(waitpid(-1,&status,0)>0 || errno==EINTR) {}
	if(fd>=0)close(fd);
	if(duplicate>=0)close(duplicate);
	if(child_fd>=0)close(child_fd);
	if(fake>=0)close(fake);
	if(bad!=MAP_FAILED)munmap(bad,4096);
	alarm(0);
	return pass ? 0 : 1;
}
