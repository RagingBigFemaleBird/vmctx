/* SPDX-License-Identifier: GPL-2.0 */
/* Linux source adapter only. The caller supplies source_control_raw(), which
 * addresses native PIDs or descriptor selectors directly, never a user registry.
 * Each source_context owns its descriptor; copies must not independently close
 * it. Native names are lookup candidates, not authority to access a task. */
#ifndef VMR_SOURCE_CONTEXT_H
#define VMR_SOURCE_CONTEXT_H
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <linux/magic.h>
#include <unistd.h>
#include "vmctx_context.h"

struct source_context {
	int fd;
	pid_t pid, tgid;
	uint64_t identity;
};

static inline int source_context_selector(const struct source_context *s)
{ return -s->fd - 1; }

static inline void source_context_close(struct source_context *s)
{
	if (s->fd >= 0) close(s->fd);
	*s = (struct source_context){.fd=-1};
}

static inline int source_context_request(pid_t selector, unsigned op,
		uint64_t ticket, struct vmctx_context *q)
{
	*q = (struct vmctx_context){.version=VMCTX_CONTEXT_ABI,
		.size=sizeof(*q),.op=op,.ticket=ticket};
	return (int)source_control_raw(selector, VMCTX_CTL_CONTEXT, q);
}

static inline int source_context_info(const struct source_context *s,
		struct vmctx_context *q)
{
	if (s->fd < 0) { errno=EBADF; return -1; }
	if (source_context_request(source_context_selector(s),VMCTX_CONTEXT_INFO,0,q))
		return -1;
	if (!s->identity || q->identity!=s->identity) { errno=EPROTO; return -1; }
	return 0;
}

static inline int source_context_open(pid_t pid, struct source_context *s)
{
	struct vmctx_context q;
	*s = (struct source_context){.fd=-1};
	if (pid<=0) { errno=EINVAL; return -1; }
	if (source_context_request(pid,VMCTX_CONTEXT_OPEN,0,&q)) return -1;
	if (q.fd<0 || !q.identity) {
		if (q.fd>=0) close(q.fd);
		errno=EPROTO; return -1;
	}
	/* The input PID is in this monitor's namespace. q.native_pid is a
	 * source-kernel diagnostic and need not use this namespace. */
	*s = (struct source_context){.fd=q.fd,.pid=pid,.tgid=q.native_tgid,
		.identity=q.identity};
	return 0;
}

/* CHILD exports the retained birth, even if it died before publication. Its
 * native names use this monitor's namespace in ABI 2, and remain candidates.
 * Never reopen a child by a syscall return value or by a saved native PID. */
static inline int source_context_child(const struct source_context *parent,
		uint64_t ticket, struct source_context *child)
{
	struct vmctx_context q;
	*child=(struct source_context){.fd=-1};
	if (source_context_request(source_context_selector(parent),
			VMCTX_CONTEXT_CHILD,ticket,&q)) return -1;
	if (q.fd<0 || !q.identity) {
		if (q.fd>=0) close(q.fd);
		errno=EPROTO; return -1;
	}
	*child=(struct source_context){.fd=q.fd,.pid=q.native_pid,
		.tgid=q.native_tgid,.identity=q.identity};
	return 0;
}

static inline int source_context_signal(const struct source_context *s, int sig)
{
	struct vmctx_context q={.version=VMCTX_CONTEXT_ABI,.size=sizeof(q),
		.op=VMCTX_CONTEXT_SIGNAL,.exit_status=(uint32_t)sig};
	return (int)source_control_raw(source_context_selector(s),VMCTX_CTL_CONTEXT,&q);
}

/* 1 live, 0 ended, -1 unknown/error. An error never proves death. */
static inline int source_mm_live(uint64_t identity)
{
	struct vmctx_context q={.version=VMCTX_CONTEXT_ABI,.size=sizeof(q),
		.op=VMCTX_CONTEXT_MMINFO,.mm_identity=identity};
	if (source_control_raw(0,VMCTX_CTL_CONTEXT,&q)) return -1;
	if (q.flags==VMCTX_CONTEXT_MM_LIVE) return 1;
	if (q.flags==VMCTX_CONTEXT_MM_ENDED) return 0;
	errno=EPROTO; return -1;
}

/* Prove that a native name still identifies the retained context and one MM.
 * A temporary descriptor closes on every outcome. No caller may consume a
 * resource opened by name before the second successful identity check. */
static inline int source_context_validate(const struct source_context *s,
		pid_t pid, uint64_t expected_mm, uint64_t *actual_mm)
{
	struct source_context probe={.fd=-1};
	struct vmctx_context q;
	int result=-1, saved;
	if (source_context_open(pid,&probe)) return -1;
	if (probe.identity!=s->identity) { errno=ESTALE; goto done; }
	if (source_context_info(&probe,&q)) goto done;
	if ((q.flags & VMCTX_CONTEXT_ENDED) || !q.mm_identity) {
		errno=ESRCH; goto done;
	}
	if (expected_mm && q.mm_identity!=expected_mm) { errno=ESTALE; goto done; }
	*actual_mm=q.mm_identity;
	result=0;
done:
	saved=errno;
	source_context_close(&probe);
	errno=saved;
	return result;
}

#ifndef SOURCE_CONTEXT_OPEN_OBSERVE
#define SOURCE_CONTEXT_OPEN_OBSERVE(phase, pid) ((void)0)
#endif

/* maps, mem and pagemap bind their mm at open. A non-leader exec can move
 * this task to its group's PID, so both names are candidates, each subject to
 * the same opaque-identity checks. Never substitute a new task at either name.
 * Other proc files can resolve a task again at read time; use read_proc below. */
static inline int source_context_proc_open(const struct source_context *s,
		const char *leaf, uint64_t *mm_id, pid_t *opened_pid)
{
	pid_t names[2]={s->pid,s->tgid};
	int saved=ESRCH;
	if (s->fd<0 || !s->identity || strchr(leaf,'/') || !*leaf) {
		errno=EINVAL; return -1;
	}
	/* A proc mount can name another PID namespace. Bracketing numeric
	 * lookups is only a proof when both lookup mechanisms use our namespace.
	 * Pin the root so a later overmount cannot change which proc instance
	 * openat uses. PID 1 belongs to that proc instance's namespace; self
	 * identifies our namespace. Compare retained namespace objects, not
	 * numeric PIDs, which can coincide in different namespaces. */
	int proc=open("/proc",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
	if (proc<0) return -1;
	struct statfs fs;
	struct stat ours, mounted;
	int own_ns=openat(proc,"self/ns/pid",O_RDONLY|O_CLOEXEC);
	int mount_ns=openat(proc,"1/ns/pid",O_RDONLY|O_CLOEXEC);
	int matching=own_ns>=0 && mount_ns>=0 && !fstatfs(proc,&fs) &&
		fs.f_type==PROC_SUPER_MAGIC && !fstat(own_ns,&ours) &&
		!fstat(mount_ns,&mounted) && ours.st_dev==mounted.st_dev &&
		ours.st_ino==mounted.st_ino;
	if (own_ns>=0) close(own_ns);
	if (mount_ns>=0) close(mount_ns);
	if (!matching) { close(proc); errno=EXDEV; return -1; }
	for (unsigned i=0;i<2;i++) {
		char path[96];
		uint64_t before, after;
		pid_t pid=names[i];
		if (pid<=0 || (i && pid==names[0])) continue;
		if (source_context_validate(s,pid,0,&before)) { saved=errno; continue; }
		SOURCE_CONTEXT_OPEN_OBSERVE(0,pid);
		int n=snprintf(path,sizeof(path),"%d/%s",pid,leaf);
		if (n<0 || (size_t)n>=sizeof(path)) { saved=ENAMETOOLONG; break; }
		int fd=openat(proc,path,O_RDONLY|O_CLOEXEC);
		if (fd<0) { saved=errno; continue; }
		SOURCE_CONTEXT_OPEN_OBSERVE(1,pid);
		if (!source_context_validate(s,pid,before,&after)) {
			*mm_id=after; *opened_pid=pid;
			close(proc);
			return fd;
		}
		saved=errno;
		close(fd);
	}
	close(proc);
	errno=saved; return -1;
}

static inline ssize_t source_context_read_proc(const struct source_context *s,
		const char *leaf, char *out, size_t capacity)
{
	uint64_t mm, after;
	pid_t pid;
	if (!capacity) { errno=EINVAL; return -1; }
	int fd=source_context_proc_open(s,leaf,&mm,&pid);
	if (fd<0) return -1;
	ssize_t n;
	do { n=read(fd,out,capacity-1); } while (n<0 && errno==EINTR);
	int saved=errno;
	close(fd);
	if (n<0) { errno=saved; return -1; }
	if (source_context_validate(s,pid,mm,&after)) return -1;
	out[n]=0;
	return n;
}

/* Call before releasing a newly registered source for executor entry. After
 * that, tgid is only an alternative native name and never a lifetime key. */
static inline int source_context_group(struct source_context *s)
{
	struct vmctx_context q;
	if (source_context_info(s,&q)) return -1;
	if (q.flags & VMCTX_CONTEXT_ENDED) { errno=ESRCH; return -1; }
	if (!q.native_tgid || q.native_tgid>INT32_MAX) { errno=EPROTO; return -1; }
	s->tgid=(pid_t)q.native_tgid;
	return 0;
}
#endif
