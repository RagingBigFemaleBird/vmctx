/* SPDX-License-Identifier: GPL-2.0 */
/* Linux execution children inherit a private descriptor table. Only their
 * two native memory objects and standard descriptors belong in that table.
 * Use native range closure: a fixed scan misses monitor channels and retained
 * context descriptors once a session has more than 1024 open descriptors.
 * These helpers do not allocate or use stdio in the child after fork. */
#ifndef VMCTX_LINUX_EXECUTION_FDS_H
#define VMCTX_LINUX_EXECUTION_FDS_H
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Consume a newly opened private monitor descriptor. Keeping standard slots
 * empty also prevents the child close-range helper from preserving a private
 * object, context handle or pidfd as if it were an inherited standard stream. */
static inline int linux_execution_private_fd(int fd)
{
	if (fd >= 0 && fd < 3) {
		int next = fcntl(fd, F_DUPFD_CLOEXEC, 3), error = errno;
		close(fd);
		fd = next;
		errno = error;
	}
	return fd;
}

static inline int linux_execution_object_fd(const char *name)
{
	return linux_execution_private_fd(syscall(SYS_memfd_create,name,MFD_CLOEXEC));
}

static inline int linux_execution_close_fds(int backing, int shared)
{
	int keep[2] = {backing, shared};
	unsigned first = 3;
	if (keep[0] > keep[1]) { int tmp=keep[0]; keep[0]=keep[1]; keep[1]=tmp; }
	for (unsigned i=0; i<2; i++) {
		if (keep[i]<3 || (unsigned)keep[i]<first) continue;
		unsigned retained = keep[i];
		if (first<retained && syscall(SYS_close_range,first,retained-1,0)) return -1;
		first = retained+1;
	}
	return syscall(SYS_close_range,first,UINT_MAX,0);
}
#endif
