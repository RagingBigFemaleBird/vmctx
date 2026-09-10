// SPDX-License-Identifier: GPL-2.0
/* Native source-adapter control: an assisted open pins the negotiated CPU view
 * even when a native monitor thread subsequently reads the descriptor.
 * Run: cpu-proc-state <run nr> <ctl nr>. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>
#include "cpu-proc-check.h"

static pid_t child;
static long ctl_nr;
static const char path[] = "/proc/cpuinfo";
static uint32_t words[5];
static int view_fd = -1;
static void expired(int sig) { (void)sig; if (child > 0) kill(child, SIGKILL); }
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s (errno %d)\n", __LINE__, #x, errno); goto done; } } while (0)

static int control(unsigned op, void *arg)
{
	for (unsigned i = 0; i < 2000; i++) {
		int r = syscall(ctl_nr, child, op, arg);
		if (!r || errno != EAGAIN) return r;
		usleep(1000);
	}
	errno = ETIMEDOUT;
	return -1;
}

static long open_source(void)
{
	struct vmctx_syscall c = {.nr = SYS_openat,
		.args = {AT_FDCWD, (uintptr_t)path, O_RDONLY | O_CLOEXEC}};
	return syscall(ctl_nr, child, VMCTX_CTL_SYSCALL, &c) ? -10000 : (long)c.ret;
}

static void *read_view(void *unused)
{
	(void)unused;
	int fd = dup(view_fd);
	if (fd < 0) return NULL;
	FILE *f = fdopen(fd, "r");
	if (!f) { close(fd); return NULL; }
	int ok = cpu_proc_check(f, words);
	fclose(f);
	return (void *)(uintptr_t)ok;
}

int main(int argc, char **argv)
{
	int ok = 0, status, attached = 0, pidfd = -1;
	struct vmctx_cpu_state cpu;
	struct vmctx_cpu_model model;
	struct vmctx_run_config cfg = {.flags = VMCTX_FLAG_SERVICE | VMCTX_FLAG_WAIT_MONITOR,
		.backing_fd = -1, .shared_fd = -1};
	if (argc != 3) return 2;
	long run_nr = strtol(argv[1], NULL, 10);
	ctl_nr = strtol(argv[2], NULL, 10);
	CHECK(!syscall(ctl_nr, 0, VMCTX_CTL_CPU_CAPS, &model));
	/* Deliberately smaller than either test machine's native capabilities. */
	model.leaf1_ecx = model.leaf7_ebx = model.ext1_ecx = 0;
	model.xcr0 = 3;
	CHECK(vmctx_cpu_model_valid(&model));
	words[0] = model.leaf1_edx; words[4] = model.ext1_edx;
	pid_t parent = getpid();
	child = fork();
	CHECK(child >= 0);
	if (!child) {
		if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(125);
		syscall(run_nr, &cfg);
		_exit(126);
	}
	signal(SIGALRM, expired); alarm(20);
	for (unsigned i = 0; i < 2000; i++) {
		if (!syscall(ctl_nr, child, VMCTX_CTL_ATTACH, NULL)) { attached = 1; break; }
		usleep(1000);
	}
	CHECK(attached);
	CHECK(!control(VMCTX_CTL_GETCPU, &cpu));
	CHECK(open_source() == -EAGAIN);
	CHECK(!control(VMCTX_CTL_CPU_MODEL, &model));
	long source_fd = open_source();
	CHECK(source_fd >= 0);
	pidfd = syscall(SYS_pidfd_open, child, 0);
	CHECK(pidfd >= 0);
	view_fd = syscall(SYS_pidfd_getfd, pidfd, source_fd, 0);
	CHECK(view_fd >= 0);
	CHECK((uintptr_t)read_view(NULL) == 1);
	CHECK(lseek(view_fd, 0, SEEK_SET) == 0);
	pthread_t worker;
	void *result;
	CHECK(!pthread_create(&worker, NULL, read_view, NULL));
	CHECK(!pthread_join(worker, &result) && (uintptr_t)result == 1);
	ok = 1;
done:
	if (view_fd >= 0) close(view_fd);
	if (pidfd >= 0) close(pidfd);
	if (child > 0) { kill(child, SIGKILL); while (waitpid(child, &status, 0) < 0 && errno == EINTR) {} }
	alarm(0);
	if (ok) puts("PASS: unnegotiated CPU view refused; reduced model retained across native monitor and worker reads");
	return ok ? 0 : 1;
}
