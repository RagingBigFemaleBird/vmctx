// SPDX-License-Identifier: GPL-2.0
/* Native source mapping queries: actual mapped file, monitor fd lifetime,
 * alias offsets, anonymous memory, SysV objects and failure atomicity. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>
#include <linux/vmctx_mapping.h>

static pid_t child;
static long ctl;
static void expired(int sig) { (void)sig; if (child > 0) kill(child, SIGKILL); }
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s (errno %d)\n", __LINE__, #x, errno); goto done; } } while (0)
static long call(long nr, uint64_t a, uint64_t b)
{
	struct vmctx_syscall c = {.nr = nr, .args = {a, b}};
	return syscall(ctl, child, VMCTX_CTL_SYSCALL, &c) ? -10000 : (long)c.ret;
}
static int query(void *addr, struct vmctx_mapping *m)
{
	*m = (struct vmctx_mapping){.addr = (uintptr_t)addr, .fd = -1};
	return syscall(ctl, child, VMCTX_CTL_MAPPING, m);
}
int main(int argc, char **argv)
{
	struct vmctx_mapping m, alias, anon, sysv, readonly, private;
	struct stat original, got;
	int ok = 0, status, fd, replacement, shmid;
	pid_t parent = getpid();
	void *a, *b, *c, *d, *e, *f, *bad;
	char data[4096], readback[4096];
	if (argc != 2) return 2;
	ctl = strtol(argv[1], NULL, 10);
	_Static_assert(sizeof(m) == 48, "mapping query ABI");
	fd = memfd_create("mapping-original", MFD_CLOEXEC);
	CHECK(fd >= 0 && !ftruncate(fd, 8192) && !fstat(fd, &original));
	memset(data, 0x91, sizeof(data));
	CHECK(pwrite(fd, data, sizeof(data), 4096) == sizeof(data));
	a = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	b = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 4096);
	c = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	shmid = shmget(IPC_PRIVATE, 8192, IPC_CREAT | 0600);
	CHECK(shmid >= 0);
	d = shmat(shmid, NULL, 0);
	CHECK(a != MAP_FAILED && b != MAP_FAILED && c != MAP_FAILED && d != (void *)-1);
	CHECK(!shmctl(shmid, IPC_RMID, NULL));
	char path[64];
	snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
	int readfd = open(path, O_RDONLY | O_CLOEXEC);
	CHECK(readfd >= 0);
	e = mmap(NULL, 4096, PROT_READ, MAP_SHARED, readfd, 0);
	f = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, readfd, 0);
	CHECK(e != MAP_FAILED && f != MAP_FAILED);
	close(readfd);
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(125);
		/* Reuse the program's descriptor before adoption. Its mapping still
		 * denotes the original file; querying that descriptor would be wrong. */
		replacement = memfd_create("mapping-replacement", MFD_CLOEXEC);
		if (replacement < 0 || dup2(replacement, fd) != fd) _exit(125);
		close(replacement);
		if (ptrace(PTRACE_TRACEME, 0, NULL, NULL)) _exit(125);
		raise(SIGSTOP);
		_exit(126);
	}
	signal(SIGALRM, expired); alarm(20);
	CHECK(waitpid(child, &status, 0) == child && WIFSTOPPED(status));
	CHECK(!syscall(ctl, child, VMCTX_CTL_ADOPT, NULL));
	CHECK(!ptrace(PTRACE_DETACH, child, NULL, NULL));
	CHECK(!query(a, &m) && m.fd >= 0 && m.start == (uintptr_t)a && m.len == 8192);
	CHECK(m.flags == VMCTX_MAPPING_SHARED && m.prot == (PROT_READ | PROT_WRITE));
	CHECK(!fstat(m.fd, &got) && got.st_dev == original.st_dev && got.st_ino == original.st_ino);
	CHECK(fcntl(m.fd, F_GETFD) & FD_CLOEXEC);
	CHECK(!query(b, &alias) && alias.fd >= 0 && alias.offset == 4096 && alias.prot == PROT_READ);
	CHECK(!fstat(alias.fd, &got) && got.st_dev == original.st_dev && got.st_ino == original.st_ino);
	CHECK(!query(c, &anon) && anon.fd == -1 && !anon.flags);
	CHECK(!query(d, &sysv) && sysv.fd >= 0 && sysv.len == 8192 && sysv.flags == VMCTX_MAPPING_SHARED);
	CHECK(!query(e, &readonly) && readonly.fd >= 0 && readonly.flags == VMCTX_MAPPING_SHARED && readonly.prot == PROT_READ);
	CHECK(!query(f, &private) && private.fd >= 0 && private.flags == 0 && private.prot == PROT_READ);
	CHECK(!fstat(readonly.fd, &got) && got.st_dev == original.st_dev && got.st_ino == original.st_ino);
	CHECK((fcntl(readonly.fd, F_GETFL) & O_ACCMODE) == O_RDONLY);
	struct vmctx_serve probe = {.addr = (uintptr_t)e, .flags = VMCTX_SERVE_PROBE};
	CHECK(!syscall(ctl, child, VMCTX_CTL_SERVE, &probe) && (probe.class & VMCTX_PGC_SHARED));
	probe = (struct vmctx_serve){.addr = (uintptr_t)f, .flags = VMCTX_SERVE_PROBE};
	CHECK(!syscall(ctl, child, VMCTX_CTL_SERVE, &probe) && !(probe.class & VMCTX_PGC_SHARED));
	CHECK(!call(SYS_munmap, (uintptr_t)a, 8192));
	CHECK(query(a, &anon) == -1 && errno == ENOENT);
	CHECK(pread(m.fd, readback, sizeof(readback), 4096) == sizeof(readback) && !memcmp(data, readback, sizeof(data)));
	/* Input lies in a read-only monitor page. copy_from_user succeeds,
	 * copy_to_user fails; there must be no reserved or installed fd leak. */
	bad = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(bad != MAP_FAILED);
	*(struct vmctx_mapping *)bad = (struct vmctx_mapping){.addr = (uintptr_t)b};
	CHECK(!mprotect(bad, 4096, PROT_READ));
	int next = dup(fd);
	CHECK(next >= 0); close(next);
	for (int i = 0; i < 64; i++)
		CHECK(syscall(ctl, child, VMCTX_CTL_MAPPING, bad) == -1 && errno == EFAULT);
	int after = dup(fd);
	CHECK(after == next); close(after);
	close(m.fd); close(alias.fd); close(sysv.fd); close(readonly.fd); close(private.fd);
	ok = 1;
done:
	if (child > 0) { kill(child, SIGKILL); while (waitpid(child, &status, 0) < 0 && errno == EINTR) {} }
	alarm(0);
	if (ok) puts("PASS: mapped-file identity survives descriptor reuse and unmap; read-only sharing, private mappings, aliases, SysV, anonymous and failed-copy lifetime");
	return ok ? 0 : 1;
}
