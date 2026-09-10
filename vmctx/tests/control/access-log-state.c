// SPDX-License-Identifier: GPL-2.0
/* Run natively on a source kernel: access-log-state <ctl syscall>.
 * Exercise real partial mprotect and guard commits, repeatable reads, bad
 * copyout, identity fencing and retention beyond the legacy log capacity. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>
#include <linux/vmctx_access.h>

static volatile sig_atomic_t child;
static long ctl_nr;
static void expired(int sig) { (void)sig; if (child > 0) kill(child, SIGKILL); }
static long assisted(long nr, uintptr_t a, unsigned long b, unsigned long c,
		     unsigned long d, long e)
{
	struct vmctx_syscall call = {.nr = nr, .args = {a,b,c,d,e,0}};
	return syscall(ctl_nr, child, VMCTX_CTL_SYSCALL, &call) ? -10000 : (long)call.ret;
}
static struct vmctx_access_log query(unsigned op, uint64_t id, uint64_t cursor)
{
	return (struct vmctx_access_log){.version = VMCTX_ACCESS_ABI,
		.size = sizeof(struct vmctx_access_log), .op = op, .mm_id = id, .cursor = cursor};
}
#define CHECK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL line %d: %s errno=%d\n", __LINE__, #x, errno); \
	goto done; } } while (0)
int main(int argc, char **argv)
{
	int pass = 0, status;
	pid_t parent = getpid();
	void *pages = MAP_FAILED, *bad = MAP_FAILED;
	struct sigaction sa = {.sa_handler = expired};
	struct vmctx_access_log q, replay;
	uint64_t id = 0, cursor = 0;
	unsigned unmaps = 0, maps = 0;
	if (argc != 2 || sigaction(SIGALRM, &sa, NULL)) return 2;
	ctl_nr = strtol(argv[1], NULL, 10);
	pages = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	bad = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(pages != MAP_FAILED && bad != MAP_FAILED);
	memset(pages, 0x5a, 3 * 4096);
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(125);
		if (ptrace(PTRACE_TRACEME, 0, NULL, NULL)) _exit(125);
		raise(SIGSTOP);
		_exit(126);
	}
	alarm(30);
	CHECK(waitpid(child, &status, 0) == child && WIFSTOPPED(status));
	CHECK(!syscall(ctl_nr, child, VMCTX_CTL_ADOPT, NULL));
	CHECK(!ptrace(PTRACE_DETACH, child, NULL, NULL));
	CHECK(assisted(SYS_getpid, 0, 0, 0, 0, 0) == child);
	q = query(VMCTX_ACCESS_INFO, 0, 0);
	CHECK(!syscall(ctl_nr, child, VMCTX_CTL_ACCESS_LOG, &q) && q.mm_id && !q.acked);
	id = q.mm_id;
	CHECK(!assisted(SYS_munmap, (uintptr_t)pages + 4096, 4096, 0, 0, 0));
	CHECK(assisted(SYS_mprotect, (uintptr_t)pages, 3 * 4096, PROT_READ, 0, 0) == -ENOMEM);
	CHECK(!assisted(SYS_madvise, (uintptr_t)pages, 4096, 102, 0, 0));
	CHECK(!assisted(SYS_madvise, (uintptr_t)pages, 4096, 103, 0, 0));
	q = query(VMCTX_ACCESS_READ, id, 0);
	CHECK(!syscall(ctl_nr, child, VMCTX_CTL_ACCESS_LOG, &q));
	CHECK(q.n == 4 && q.event[0].kind == VMCTX_ACCESS_UNMAP &&
	      q.event[1].kind == VMCTX_ACCESS_PROTECT && q.event[1].prot == PROT_READ &&
	      q.event[1].start == (uintptr_t)pages && q.event[1].end == (uintptr_t)pages + 4096 &&
	      q.event[2].kind == VMCTX_ACCESS_DENY && q.event[3].kind == VMCTX_ACCESS_ALLOW);
	for (unsigned i = 1; i < q.n; i++) CHECK(q.event[i].seq > q.event[i-1].seq);
	replay = query(VMCTX_ACCESS_READ, id, 0);
	CHECK(!syscall(ctl_nr, child, VMCTX_CTL_ACCESS_LOG, &replay));
	CHECK(!memcmp(&q, &replay, sizeof(q)));
	*(struct vmctx_access_log *)bad = query(VMCTX_ACCESS_READ, id, 0);
	CHECK(!mprotect(bad, 4096, PROT_READ));
	errno = 0;
	CHECK(syscall(ctl_nr, child, VMCTX_CTL_ACCESS_LOG, bad) == -1 && errno == EFAULT);
	replay = query(VMCTX_ACCESS_READ, id, 0);
	CHECK(!syscall(ctl_nr, child, VMCTX_CTL_ACCESS_LOG, &replay));
	CHECK(!memcmp(&q, &replay, sizeof(q)));
	replay = query(VMCTX_ACCESS_ACK, id + 1, q.cursor);
	errno = 0;
	CHECK(syscall(ctl_nr, child, VMCTX_CTL_ACCESS_LOG, &replay) == -1 && errno == ESTALE);
	replay = query(VMCTX_ACCESS_ACK, id, q.head + 1);
	errno = 0;
	CHECK(syscall(ctl_nr, child, VMCTX_CTL_ACCESS_LOG, &replay) == -1 && errno == ERANGE);
	cursor = q.cursor;
	replay = query(VMCTX_ACCESS_ACK, id, cursor);
	CHECK(!syscall(ctl_nr, child, VMCTX_CTL_ACCESS_LOG, &replay) && replay.acked == cursor);
	/* More pending changes than either legacy fixed-size queue can retain. */
	for (unsigned i = 0; i < 96; i++) {
		CHECK(!assisted(SYS_munmap, (uintptr_t)pages, 4096, 0, 0, 0));
		CHECK(assisted(SYS_mmap, (uintptr_t)pages, 4096, PROT_READ | PROT_WRITE,
			MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1) == (long)pages);
	}
	for (;;) {
		q = query(VMCTX_ACCESS_READ, id, cursor);
		CHECK(!syscall(ctl_nr, child, VMCTX_CTL_ACCESS_LOG, &q) && q.n <= VMCTX_ACCESS_MAX);
		if (!q.n) break;
		for (unsigned i = 0; i < q.n; i++) {
			CHECK(q.event[i].seq > cursor && q.event[i].start == (uintptr_t)pages &&
			      q.event[i].end == (uintptr_t)pages + 4096);
			cursor = q.event[i].seq;
			if (q.event[i].kind == VMCTX_ACCESS_UNMAP) unmaps++;
			else if (q.event[i].kind == VMCTX_ACCESS_CONSTRUCT) maps++;
			else CHECK(0);
		}
		CHECK(q.cursor == cursor);
		replay = query(VMCTX_ACCESS_ACK, id, cursor);
		CHECK(!syscall(ctl_nr, child, VMCTX_CTL_ACCESS_LOG, &replay));
	}
	CHECK(unmaps == 96 && maps == 96 && q.acked == cursor);
	pass = 1;
done:
	if (child > 0) {
		kill(child, SIGKILL);
		while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
	}
	alarm(0);
	if (pages != MAP_FAILED) munmap(pages, 3 * 4096);
	if (bad != MAP_FAILED) munmap(bad, 4096);
	if (pass) puts("PASS: partial protection, guard commits, repeatable reads, bad-copy retention, identity fencing and 192 retained mutations");
	return pass ? 0 : 1;
}
