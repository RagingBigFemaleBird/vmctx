/* SPDX-License-Identifier: GPL-2.0 */
/* Linux execution task adapter. The caller supplies execution_control_raw().
 * Opening is restricted to a newly created, attached, unreaped child owned by
 * this monitor. That creation scope pins the numeric name until both native
 * descriptors are acquired. Published records never reopen controls by PID.
 * Keep both descriptors until all workers have joined: the context descriptor
 * retains backend identity, while pidfd observes and signals native task exit
 * even after the backend has already ended. Neither owns source wait status.
 */
#ifndef VMR_LINUX_EXECUTION_CONTEXT_H
#define VMR_LINUX_EXECUTION_CONTEXT_H
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/magic.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>
#include "vmctx_context.h"
#include "linux-execution-fds.h"

struct linux_execution_context {
	int fd, exit_fd;
	pid_t pid; /* native lookup candidate for the proc adapter only */
	uint64_t identity;
};

static inline int linux_execution_selector(const struct linux_execution_context *context)
{ return -context->fd-1; }

static inline int linux_execution_capabilities(void)
{
	struct vmctx_context q={.version=VMCTX_CONTEXT_ABI,.size=sizeof(q),
		.op=VMCTX_CONTEXT_CAPS};
	if(execution_control_raw(0,VMCTX_CTL_CONTEXT,&q))return -1;
	const unsigned required=VMCTX_CONTEXT_CAP_REFERENCE|VMCTX_CONTEXT_CAP_EXECUTOR|VMCTX_CONTEXT_CAP_MMINFO;
	if(q.version!=VMCTX_CONTEXT_ABI || q.size!=sizeof(q) ||
	   q.op!=VMCTX_CONTEXT_CAPS || (q.flags&required)!=required ||
	   q.ticket || q.identity || q.mm_identity || q.fd ||
	   q.native_pid || q.native_tgid || q.exit_status) {errno=EPROTO;return -1;}
	return 0;
}

static inline void linux_execution_close(struct linux_execution_context *context)
{
	if(context->fd>=0)close(context->fd);
	if(context->exit_fd>=0)close(context->exit_fd);
	*context=(struct linux_execution_context){.fd=-1,.exit_fd=-1};
}

static inline int linux_execution_open_child(pid_t pid,struct linux_execution_context *context)
{
	*context=(struct linux_execution_context){.fd=-1,.exit_fd=-1};
	if(pid<=0) {errno=EINVAL;return -1;}
	struct vmctx_context q={.version=VMCTX_CONTEXT_ABI,.size=sizeof(q),
		.op=VMCTX_CONTEXT_OPEN};
	if(execution_control_raw(pid,VMCTX_CTL_CONTEXT,&q))return -1;
	if(q.fd<0 || !q.identity || q.version!=VMCTX_CONTEXT_ABI ||
	   q.size!=sizeof(q) || q.op!=VMCTX_CONTEXT_OPEN ||
	   q.native_pid!=(uint32_t)pid || q.flags&VMCTX_CONTEXT_ENDED) {
		if(q.fd>=0)close(q.fd);
		errno=EPROTO;return -1;
	}
	context->fd=linux_execution_private_fd(q.fd);
	if(context->fd<0)return -1;
	context->identity=q.identity;context->pid=pid;
	context->exit_fd=linux_execution_private_fd(syscall(SYS_pidfd_open,pid,0));
	if(context->exit_fd<0) {
		int saved=errno;linux_execution_close(context);errno=saved;return -1;
	}
	return 0;
}

static inline long linux_execution_control(const struct linux_execution_context *context,
		unsigned command,void *argument)
{
	if(context->fd<0 || !context->identity) {errno=EBADF;return -1;}
	return execution_control_raw(linux_execution_selector(context),command,argument);
}

static inline int linux_execution_signal(const struct linux_execution_context *context,int sig)
{
	if(context->exit_fd<0 || !context->identity) {errno=EBADF;return -1;}
	return syscall(SYS_pidfd_send_signal,context->exit_fd,sig,NULL,0);
}

/* 1 ended, 0 no exit event, -1 error. Elapsed time or a backend failure is
 * never proof of native task exit. poll's EINTR preserves that distinction. */
static inline int linux_execution_ended(const struct linux_execution_context *context,int timeout)
{
	if(context->exit_fd<0 || !context->identity) {errno=EBADF;return -1;}
	struct pollfd fd={.fd=context->exit_fd,.events=POLLIN};
	int n=poll(&fd,1,timeout);
	if(n<=0)return n;
	if(fd.revents&POLLIN)return 1;
	errno=EIO;return -1;
}

/* Reap the owned native task through its pidfd. A repeated/late wait cannot
 * reap a new child at a reused PID. Translate native siginfo only inside this
 * adapter; this status describes the executor task, not source program exit. */
static inline int linux_execution_wait(const struct linux_execution_context *context,int *status)
{
	if(context->exit_fd<0 || !context->identity) {errno=EBADF;return -1;}
	siginfo_t info={0};
	int result;
	do {result=waitid(P_PIDFD,context->exit_fd,&info,WEXITED);}while(result<0 && errno==EINTR);
	if(result<0)return -1;
	if(info.si_pid!=context->pid) {errno=EPROTO;return -1;}
	switch(info.si_code) {
	case CLD_EXITED: *status=(info.si_status&255)<<8;break;
	case CLD_KILLED: *status=info.si_status&127;break;
	case CLD_DUMPED: *status=(info.si_status&127)|128;break;
	default: errno=EPROTO;return -1;
	}
	return 0;
}

static inline int linux_execution_named_mm(const struct linux_execution_context *context,
		pid_t pid,uint64_t *mm)
{
	struct vmctx_context q={.version=VMCTX_CONTEXT_ABI,.size=sizeof(q),
		.op=VMCTX_CONTEXT_INFO};
	if(linux_execution_control(context,VMCTX_CTL_CONTEXT,&q))return -1;
	if(q.identity!=context->identity || q.version!=VMCTX_CONTEXT_ABI ||
	   q.size!=sizeof(q) || q.op!=VMCTX_CONTEXT_INFO) {errno=EPROTO;return -1;}
	if(q.flags&VMCTX_CONTEXT_ENDED) {errno=ESRCH;return -1;}
	if(q.native_pid!=(uint32_t)pid || !q.mm_identity) {errno=ESTALE;return -1;}
	*mm=q.mm_identity;return 0;
}

#ifndef LINUX_EXECUTION_PROC_OBSERVE
#define LINUX_EXECUTION_PROC_OBSERVE(phase,pid) ((void)0)
#endif

/* Only these proc files retain the MM selected by open. Validate the retained
 * context on both sides of that open and consume no bytes until the second
 * validation succeeds. A pinned proc root and PID-namespace check exclude
 * lookups in a different native namespace. Rebinding an execution object in
 * the same native MM needs the separate execution_binding guard as well. */
static inline int linux_execution_proc_open(const struct linux_execution_context *context,
		pid_t pid,const char *leaf)
{
	if(pid<=0 || (strcmp(leaf,"maps") && strcmp(leaf,"pagemap") && strcmp(leaf,"mem"))) {
		errno=EINVAL;return -1;
	}
	uint64_t before,after;
	if(linux_execution_named_mm(context,pid,&before))return -1;
	int proc=open("/proc",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
	if(proc<0)return -1;
	struct statfs fs;
	struct stat ours,mounted;
	int own_ns=openat(proc,"self/ns/pid",O_RDONLY|O_CLOEXEC);
	int mount_ns=openat(proc,"1/ns/pid",O_RDONLY|O_CLOEXEC);
	int matching=own_ns>=0 && mount_ns>=0 && !fstatfs(proc,&fs) &&
		fs.f_type==PROC_SUPER_MAGIC && !fstat(own_ns,&ours) &&
		!fstat(mount_ns,&mounted) && ours.st_dev==mounted.st_dev && ours.st_ino==mounted.st_ino;
	if(own_ns>=0)close(own_ns);
	if(mount_ns>=0)close(mount_ns);
	if(!matching) {close(proc);errno=EXDEV;return -1;}
	LINUX_EXECUTION_PROC_OBSERVE(0,pid);
	char path[64];
	int length=snprintf(path,sizeof(path),"%d/%s",pid,leaf);
	if(length<0 || (size_t)length>=sizeof(path)) {close(proc);errno=ENAMETOOLONG;return -1;}
	int fd=openat(proc,path,O_RDONLY|O_CLOEXEC),saved=errno;
	close(proc);
	if(fd<0) {errno=saved;return -1;}
	LINUX_EXECUTION_PROC_OBSERVE(1,pid);
	if(linux_execution_named_mm(context,pid,&after)) {
		saved=errno;close(fd);errno=saved;return -1;
	}
	if(after!=before) {close(fd);errno=ESTALE;return -1;}
	return fd;
}
#endif
