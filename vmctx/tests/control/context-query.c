// SPDX-License-Identifier: GPL-2.0
/* Context metadata must remain readable while native work waits for I/O. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>

static long ctl_nr;
static volatile sig_atomic_t child;
static int context_fd = -1, read_result;
static struct vmctx_syscall read_call;
static struct vmctx_cpu_state cpu;
static char byte;
static void deadline(int sig)
{
	(void)sig;
	if (child > 0) kill(child, SIGKILL);
}
static long ctl(unsigned op, void *arg)
{ return syscall(ctl_nr, -(context_fd + 1), op, arg); }
static void *reader(void *unused)
{
	(void)unused;
	read_result = ctl(VMCTX_CTL_SYSCALL, &read_call);
	return NULL;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno); goto done; } } while (0)
int main(int argc, char **argv)
{
	int pipefd[2], status, pass=0, started=0, busy=0;
	pthread_t thread;
	struct vmctx_context info={.version=VMCTX_CONTEXT_ABI,
		.size=sizeof(info),.op=VMCTX_CONTEXT_OPEN};
	struct vmctx_run_config cfg={.flags=VMCTX_FLAG_SERVICE|VMCTX_FLAG_WAIT_MONITOR,
		.backing_fd=-1,.shared_fd=-1};
	if (argc!=3 || pipe(pipefd)) return 2;
	long run_nr=strtol(argv[1],NULL,10);
	ctl_nr=strtol(argv[2],NULL,10);
	struct sigaction sa={.sa_handler=deadline};
	if (sigaction(SIGALRM,&sa,NULL)) return 2;
	alarm(12);
	pid_t monitor=getpid();
	child=fork(); CHECK(child>=0);
	if (!child) {
		if (prctl(PR_SET_PDEATHSIG,SIGKILL) || getppid()!=monitor) _exit(125);
		syscall(run_nr,&cfg); _exit(126);
	}
	for (int i=0;syscall(ctl_nr,child,VMCTX_CTL_ATTACH,NULL);i++) {
		CHECK(i<2000); usleep(1000);
	}
	CHECK(!syscall(ctl_nr,child,VMCTX_CTL_CONTEXT,&info));
	context_fd=info.fd;
	uint64_t identity=info.identity;
	for (int i=0;ctl(VMCTX_CTL_GETCPU,&cpu);i++) {
		CHECK(i<2000 && errno==EAGAIN); usleep(1000);
	}
	read_call=(struct vmctx_syscall){.nr=SYS_read,
		.args={pipefd[0],(uintptr_t)&byte,1}};
	CHECK(!pthread_create(&thread,NULL,reader,NULL)); started=1;
	for (int i=0;i<2000;i++) {
		if (ctl(VMCTX_CTL_GETCPU,&cpu)==-1 && errno==EBUSY) { busy=1; break; }
		usleep(1000);
	}
	CHECK(busy);
	/* No sleep assumes that read() has reached its wait. CPU exclusion
	 * proves the assisted caller is holding the competing control lock. */
	info=(struct vmctx_context){.version=VMCTX_CONTEXT_ABI,
		.size=sizeof(info),.op=VMCTX_CONTEXT_INFO};
	CHECK(!ctl(VMCTX_CTL_CONTEXT,&info));
	CHECK(info.identity==identity && info.mm_identity && info.flags==VMCTX_CONTEXT_READY);
	info=(struct vmctx_context){.version=VMCTX_CONTEXT_ABI,
		.size=sizeof(info),.op=VMCTX_CONTEXT_OPEN};
	CHECK(!syscall(ctl_nr,child,VMCTX_CTL_CONTEXT,&info));
	CHECK(info.identity==identity);
	CHECK(!close(info.fd));
	CHECK(write(pipefd[1],"x",1)==1);
	CHECK(!pthread_join(thread,NULL)); started=0;
	CHECK(!read_result && read_call.ret==1);
	puts("PASS: retained context INFO and duplicate OPEN remain available during a blocked native assisted read");
	pass=1;
done:
	if (child>0) {
		kill(child,SIGKILL);
		while (waitpid(child,&status,0)<0 && errno==EINTR) {}
		child=0;
	}
	if (started) pthread_join(thread,NULL);
	if (context_fd>=0) close(context_fd);
	close(pipefd[0]); close(pipefd[1]); alarm(0);
	return pass ? 0 : 1;
}
