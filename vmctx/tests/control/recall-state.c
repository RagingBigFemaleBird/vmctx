// SPDX-License-Identifier: GPL-2.0
/* Source-adapter recall lifetime and mapping-incarnation control.
 * Build with -static -pthread -D__EXPORTED_HEADERS__ and the source UAPI.
 * Run: recall-state <ctl syscall>.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
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
#include <linux/vmctx_recall.h>

static volatile sig_atomic_t child, expired;
static long ctl_nr;
static uintptr_t address;
static unsigned char bytes[4096], observed[4096];
static struct vmctx_recall other_ticket;
static int other_status, other_errno;
static const char *stage = "setup";

static void deadline(int sig)
{
	(void)sig;
	expired = 1;
	if (child > 0) kill(child, SIGKILL);
}
static long control(unsigned op, void *arg)
{
	return syscall(ctl_nr, child, op, arg);
}
static int state(unsigned expected)
{
	struct vmctx_serve s = {.addr = address};
	return !control(VMCTX_CTL_PGSTATE, &s) && s.state == expected;
}
static int begin(struct vmctx_recall *t)
{
	*t = (struct vmctx_recall){.addr = address};
	return control(VMCTX_CTL_RECALL, t);
}
static long finish(struct vmctx_recall *t, unsigned op, const void *buf)
{
	t->op = op;
	t->buf = (uintptr_t)buf;
	t->gen = 19;
	return syscall(ctl_nr, 0, VMCTX_CTL_RECALL, t);
}
static int mark_remote(void)
{
	struct vmctx_pgset s = {.addr = address, .state = VMCTX_PG_REMOTE};
	return control(VMCTX_CTL_PGSET, &s);
}
static long assisted(long nr, uint64_t a, uint64_t b, uint64_t c,
		     uint64_t d, uint64_t e, uint64_t f)
{
	struct vmctx_syscall call = {.nr = nr, .args = {a,b,c,d,e,f}};
	if (control(VMCTX_CTL_SYSCALL, &call)) return -10000;
	return call.ret;
}
static void *abandon(void *unused)
{
	(void)unused;
	other_status = begin(&other_ticket);
	other_errno = errno;
	return NULL; /* kernel must release this thread's outstanding ticket */
}
static void *steal(void *unused)
{
	(void)unused;
	other_status = finish(&other_ticket, VMCTX_RECALL_CANCEL, NULL);
	other_errno = errno;
	return NULL;
}
static int peek_matches(void)
{
	struct vmctx_mem m = {.addr = address, .len = sizeof(observed),
		.buf = (uintptr_t)observed};
	return control(VMCTX_CTL_PEEK, &m) == 4096 &&
	       !memcmp(bytes, observed, sizeof(bytes));
}
#define REQUIRE(test) do { if (!(test)) { \
	fprintf(stderr, "assertion failed at line %d: %s\n", __LINE__, #test); \
	goto done; } } while (0)
int main(int argc, char **argv)
{
	int pass = 0, status = 0, fd = -1;
	int replacement_fd = -1;
	pid_t reaped = -1, parent = getpid();
	void *mapping = MAP_FAILED;
	void *replacement = MAP_FAILED;
	struct sigaction sa = {.sa_handler = deadline};
	struct vmctx_recall t, duplicate;
	pthread_t thread;
	if (argc != 2 || sigaction(SIGALRM, &sa, NULL)) return 2;
	ctl_nr = strtol(argv[1], NULL, 10);
	fd = memfd_create("recall-control", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	REQUIRE(fd >= 0 && !ftruncate(fd, 182));
	mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	REQUIRE(mapping != MAP_FAILED);
	address = (uintptr_t)mapping;
	replacement_fd = memfd_create("recall-replacement", MFD_CLOEXEC);
	REQUIRE(replacement_fd >= 0 && !ftruncate(replacement_fd, 8192));
	replacement = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, replacement_fd, 0);
	REQUIRE(replacement != MAP_FAILED);
	child = fork();
	REQUIRE(child >= 0);
	if (!child) {
		if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(125);
		if (ptrace(PTRACE_TRACEME, 0, NULL, NULL)) _exit(125);
		raise(SIGSTOP);
		_exit(126);
	}
	alarm(20);
	REQUIRE(waitpid(child, &status, 0) == child && WIFSTOPPED(status));
	/* ADOPT establishes the source mm's ownership records. The low-level
	 * SERVICE run mode used by CPU controls does not adopt an address space. */
	REQUIRE(!control(VMCTX_CTL_ADOPT, NULL));
	REQUIRE(!ptrace(PTRACE_DETACH, child, NULL, NULL));
	stage = "fresh shared source page and receiver acknowledgement";
	struct vmctx_serve s = {.addr = address, .buf = (uintptr_t)observed};
	memset(observed, 0xa5, sizeof(observed));
	REQUIRE(!control(VMCTX_CTL_SERVE, &s));
	REQUIRE(s.status == VMCTX_SERVE_COPIED && (s.flags & VMCTX_SERVE_GRANT));
	REQUIRE(!memcmp(bytes, observed, sizeof(bytes)) && state(VMCTX_PG_TRANSIT));
	REQUIRE(begin(&t) == -1 && errno == EAGAIN);
	struct vmctx_pgack ack = {.addr = address, .sum = s.sum};
	REQUIRE(control(VMCTX_CTL_PGACK, &ack) == 1 && state(VMCTX_PG_REMOTE));
	puts("PASS: untouched partial-EOF shmem page comes from source and waits for installation ACK");
	stage = "exclusive recall and owner identity";
	REQUIRE(!begin(&t) && t.ticket && state(VMCTX_PG_CLAIM));
	REQUIRE(begin(&duplicate) == -1 && errno == EAGAIN);
	REQUIRE(mark_remote() == -1 && errno == EBUSY);
	struct vmctx_land land = {.addr = address, .buf = (uintptr_t)bytes};
	REQUIRE(control(VMCTX_CTL_LAND, &land) == -1 && errno == EBUSY);
	struct vmctx_mem taken = {.addr = address, .len = sizeof(bytes), .buf = (uintptr_t)bytes};
	REQUIRE(control(VMCTX_CTL_TAKE, &taken) == -1 && errno == EBUSY);
	REQUIRE(control(VMCTX_CTL_POKE, &taken) == -1 && errno == EBUSY);
	s = (struct vmctx_serve){.addr = address, .buf = (uintptr_t)observed};
	REQUIRE(!control(VMCTX_CTL_SERVE, &s) && s.status == VMCTX_SERVE_CLAIMING);
	other_ticket = t;
	REQUIRE(!pthread_create(&thread, NULL, steal, NULL));
	REQUIRE(!pthread_join(thread, NULL) && other_status == -1 && other_errno == ENOENT);
	for (unsigned i = 0; i < sizeof(bytes); i++) bytes[i] = (i * 71 + 19) & 255;
	REQUIRE(finish(&t, VMCTX_RECALL_COMMIT, bytes) == 4096);
	REQUIRE(state(VMCTX_PG_HOME) && !memcmp(mapping, bytes, sizeof(bytes)));
	REQUIRE(finish(&t, VMCTX_RECALL_COMMIT, bytes) == -1 && errno == ENOENT);
	puts("PASS: recall excludes competitors, enforces monitor thread ownership, and commits the full page once");
	stage = "native partial-EOF page tail";
	s = (struct vmctx_serve){.addr = address, .buf = (uintptr_t)observed};
	REQUIRE(!control(VMCTX_CTL_SERVE, &s) && s.status == VMCTX_SERVE_COPIED);
	REQUIRE(!memcmp(bytes, observed, sizeof(bytes)));
	ack.sum = s.sum;
	REQUIRE(control(VMCTX_CTL_PGACK, &ack) == 1);
	stage = "monitor write after a completed recall was lent away again";
	/* COMMIT's ticket no longer protects a subsequent POKE. SERVE+ACK above
	 * deterministically places a new remote owner in that window. The write
	 * must fail before GUP can create a second writable copy or change bytes. */
	unsigned char patch[4] = {0x12, 0x34, 0x56, 0x78};
	struct vmctx_mem late = {.addr = address + 8, .len = sizeof(patch),
		.buf = (uintptr_t)patch};
	long late_result = control(VMCTX_CTL_POKE, &late);
	int late_errno = errno;
	if (late_result != -1 || late_errno != EAGAIN)
		fprintf(stderr, "late POKE result=%ld errno=%d\n", late_result, late_errno);
	REQUIRE(late_result == -1 && late_errno == EAGAIN);
	REQUIRE(state(VMCTX_PG_REMOTE) && !memcmp(mapping, bytes, sizeof(bytes)));
	puts("PASS: a monitor write cannot steal a page that was lent away after recall");
	stage = "failed user copy consumes ticket without installing bytes";
	REQUIRE(!begin(&t));
	REQUIRE(finish(&t, VMCTX_RECALL_COMMIT, (void *)1) == -1 && errno == EFAULT);
	REQUIRE(state(VMCTX_PG_REMOTE) && !memcmp(mapping, bytes, sizeof(bytes)));
	REQUIRE(finish(&t, VMCTX_RECALL_CANCEL, NULL) == -1 && errno == ENOENT);
	puts("PASS: native mapped tail survives recall; an invalid copy cannot mutate memory or leak a ticket");
	stage = "owner exit cancels outstanding recall";
	REQUIRE(!pthread_create(&thread, NULL, abandon, NULL));
	REQUIRE(!pthread_join(thread, NULL) && !other_status && state(VMCTX_PG_REMOTE));
	REQUIRE(!begin(&t));
	REQUIRE(!finish(&t, VMCTX_RECALL_CANCEL, NULL) && state(VMCTX_PG_REMOTE));
	puts("PASS: monitor thread exit releases its outstanding recall");
	stage = "partial monitor write across two guarded pages";
	struct vmctx_mem partial = {.addr = (uintptr_t)replacement + 4096 - 32,
		.len = 64, .buf = (uintptr_t)bytes};
	REQUIRE(control(VMCTX_CTL_POKE, &partial) == 64);
	REQUIRE(!memcmp((char *)replacement + 4096 - 32, bytes, 64));
	REQUIRE(!((unsigned char *)replacement)[4096 - 33] &&
		!((unsigned char *)replacement)[4096 + 32]);
	puts("PASS: a monitor write spanning two pages preserves bytes outside its range");
	stage = "second remote page rejects the whole cross-page monitor write";
	struct vmctx_pgset remote_second = {.addr = (uintptr_t)replacement + 4096,
		.state = VMCTX_PG_REMOTE};
	REQUIRE(!control(VMCTX_CTL_PGSET, &remote_second));
	unsigned char rejected[64] = {0};
	partial.buf = (uintptr_t)rejected;
	REQUIRE(control(VMCTX_CTL_POKE, &partial) == -1 && errno == EAGAIN);
	REQUIRE(!memcmp((char *)replacement + 4096 - 32, bytes, 64));
	remote_second.state = VMCTX_PG_HOME;
	REQUIRE(!control(VMCTX_CTL_PGSET, &remote_second));
	puts("PASS: remote ownership of the second page prevents a partial first-page write");
	stage = "repeated address reuse invalidates old replies";
	for (unsigned round = 0; round < 3; round++) {
		REQUIRE(!mark_remote() && !begin(&t));
		REQUIRE(!assisted(SYS_munmap, address, 4096, 0, 0, 0, 0));
		memset(bytes, 0x60 + round, sizeof(bytes));
		memcpy(replacement, bytes, sizeof(bytes));
		REQUIRE(assisted(SYS_mmap, address, 4096, PROT_READ | PROT_WRITE,
			MAP_FIXED | MAP_SHARED, replacement_fd, 0) == (long)address);
		memset(observed, 0x22, sizeof(observed));
		REQUIRE(finish(&t, VMCTX_RECALL_COMMIT, observed) == -1 && errno == ESTALE);
		REQUIRE(peek_matches());
		REQUIRE(finish(&t, VMCTX_RECALL_CANCEL, NULL) == -1 && errno == ENOENT);
	}
	puts("PASS: repeated identical unmaps cancel old tickets without overwriting the replacement mapping");
	pass = 1;
done:
	if (!pass) fprintf(stderr, "FAIL: %s errno=%d (%s) timeout=%d\n",
		stage, errno, strerror(errno), (int)expired);
	if (child > 0) {
		kill(child, SIGKILL);
		do { reaped = waitpid(child, &status, 0); } while (reaped < 0 && errno == EINTR);
	}
	alarm(0);
	if (mapping != MAP_FAILED) munmap(mapping, 4096);
	if (replacement != MAP_FAILED) munmap(replacement, 8192);
	if (fd >= 0) close(fd);
	if (replacement_fd >= 0) close(replacement_fd);
	return pass && !expired && reaped == child ? 0 : 1;
}
