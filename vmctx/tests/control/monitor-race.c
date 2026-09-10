// SPDX-License-Identifier: GPL-2.0
/* Exercise attach/drop/reference acquisition concurrently with task exit. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/vmctx.h>
static long ctl_nr;
static pid_t child;
static atomic_uint errors, calls;
static atomic_int stop;
static void deadline(int sig) { (void)sig; if (child > 0) kill(child, SIGKILL); _exit(124); }
static void *race(void *opaque)
{
	unsigned role = (unsigned)(uintptr_t)opaque;
	struct vmctx_cpu_state cpu;
	while (!atomic_load(&stop)) {
		unsigned op = role == 0 ? VMCTX_CTL_ATTACH : role == 1 ? VMCTX_CTL_DETACH : VMCTX_CTL_GETCPU;
		int r = syscall(ctl_nr, child, op, op == VMCTX_CTL_GETCPU ? &cpu : NULL);
		int e = errno;
		atomic_fetch_add(&calls, 1);
		if (r && e != EBUSY && e != EPERM && e != EAGAIN && e != ESRCH && e != EINVAL) {
			fprintf(stderr, "FAIL: concurrent op=%u errno=%d\n", op, e);
			atomic_fetch_add(&errors, 1); break;
		}
	}
	return NULL;
}
int main(int argc, char **argv)
{
	if (argc != 3) return 2;
	long run_nr = strtol(argv[1], NULL, 10); ctl_nr = strtol(argv[2], NULL, 10);
	signal(SIGALRM, deadline); alarm(20);
	struct vmctx_run_config cfg = {.flags = VMCTX_FLAG_SERVICE | VMCTX_FLAG_WAIT_MONITOR,
		.backing_fd = -1, .shared_fd = -1};
	pid_t parent = getpid(); child = fork();
	if (child < 0) return 2;
	if (!child) {
		if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent) _exit(125);
		syscall(run_nr, &cfg); _exit(126);
	}
	int attached = 0, status = 0, pass = 0;
	pthread_t threads[4]; unsigned started = 0;
	for (unsigned i = 0; i < 2000; i++) {
		if (!syscall(ctl_nr, child, VMCTX_CTL_ATTACH, NULL)) { attached = 1; break; }
		usleep(1000);
	}
	if (!attached) goto done;
	usleep(50000);
	for (unsigned i = 0; i < 4; i++) {
		if (pthread_create(&threads[i], NULL, race, (void *)(uintptr_t)(i % 3))) goto done;
		started++;
	}
	usleep(1500000);
	if (waitpid(child, &status, WNOHANG) != 0) { child = 0; goto done; }
	if (kill(child, SIGKILL)) goto done;
	/* Leave ctl readers active while native teardown clears the owner. Do
	 * not reap yet: retaining the zombie prevents numeric PID reuse. */
	usleep(250000);
	atomic_store(&stop, 1);
	for (unsigned i = 0; i < started; i++) pthread_join(threads[i], NULL);
	started = 0;
	if (waitpid(child, &status, 0) != child) goto done;
	child = 0;
	pass = WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL && !atomic_load(&errors) && atomic_load(&calls) > 1000;
	printf("%s: monitor attach/detach/state/exit calls=%u errors=%u status=%#x\n",
		pass ? "PASS" : "FAIL", atomic_load(&calls), atomic_load(&errors), status);
done:
	atomic_store(&stop, 1);
	for (unsigned i = 0; i < started; i++) pthread_join(threads[i], NULL);
	if (child > 0) { kill(child, SIGKILL); while (waitpid(child, &status, 0) < 0 && errno == EINTR) {} child = 0; }
	alarm(0);
	return pass ? 0 : 1;
}
