// SPDX-License-Identifier: GPL-2.0
/* Native source log regression: identical ranges are distinct mutations.
 * gcc -O2 -Wall -Wextra -static -D__EXPORTED_HEADERS__ -I<source>/include/uapi
 *     mm-log-state.c -o mm-log-state
 * Run: mm-log-state <source ctl syscall>
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>

static volatile sig_atomic_t child;
static long ctl_nr;
static void expired(int sig) { (void)sig; if (child > 0) kill(child, SIGKILL); }
static long assisted(long nr, uintptr_t addr, unsigned long len,
		     unsigned long prot, unsigned long flags, long fd)
{
	struct vmctx_syscall call = {.nr = nr, .args = {addr,len,prot,flags,fd,0}};
	return syscall(ctl_nr, child, VMCTX_CTL_SYSCALL, &call) ? -10000 : (long)call.ret;
}
#define CHECK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL line %d: %s errno=%d\n", __LINE__, #x, errno); \
	goto done; } } while (0)
int main(int argc, char **argv)
{
	int pass = 0, status;
	pid_t parent = getpid();
	void *page = MAP_FAILED;
	struct sigaction sa = {.sa_handler = expired};
	struct vmctx_mmlog before = {.max = VMCTX_MMLOG_MAX}, after = {.max = VMCTX_MMLOG_MAX};
	if (argc != 2 || sigaction(SIGALRM, &sa, NULL)) return 2;
	ctl_nr = strtol(argv[1], NULL, 10);
	page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(page != MAP_FAILED);
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(125);
		if (ptrace(PTRACE_TRACEME, 0, NULL, NULL)) _exit(125);
		raise(SIGSTOP);
		_exit(126);
	}
	alarm(15);
	CHECK(waitpid(child, &status, 0) == child && WIFSTOPPED(status));
	CHECK(!syscall(ctl_nr, child, VMCTX_CTL_ADOPT, NULL));
	CHECK(!ptrace(PTRACE_DETACH, child, NULL, NULL));
	/* Complete one assisted call to ensure the service task subscribed. */
	CHECK(assisted(SYS_getpid, 0, 0, 0, 0, 0) == child);
	CHECK(!syscall(ctl_nr, child, VMCTX_CTL_MMLOG, &before) && !before.dropped);
	for (unsigned round = 0; round < 3; round++) {
		CHECK(!assisted(SYS_munmap, (uintptr_t)page, 4096, 0, 0, 0));
		CHECK(assisted(SYS_mmap, (uintptr_t)page, 4096, PROT_READ | PROT_WRITE,
			MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1) == (long)page);
	}
	/* No intervening drain may be required to distinguish incarnations. */
	CHECK(!syscall(ctl_nr, child, VMCTX_CTL_MMLOG, &after));
	CHECK(!after.dropped && after.n == 3 && after.cur > before.cur);
	for (unsigned i = 0; i < after.n; i++) {
		CHECK(after.ent[i].start == (uintptr_t)page &&
			after.ent[i].end == (uintptr_t)page + 4096 &&
			after.ent[i].seq > (i ? after.ent[i - 1].seq : before.cur) &&
			after.ent[i].seq <= after.cur);
	}
	pass = 1;
done:
	if (child > 0) {
		kill(child, SIGKILL);
		while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
	}
	alarm(0);
	if (page != MAP_FAILED) munmap(page, 4096);
	if (pass) puts("PASS: three identical unmaps remain three ordered events without intervening drains");
	return pass ? 0 : 1;
}
